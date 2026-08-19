/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/heap.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
#include <seneri/thread.h>
#include <seneri/timer.h>

/*
 * More than one thread of control, on one core, cooperatively scheduled.
 *
 * The whole layer rests on one observation: a call to thread_switch_context
 * leaves its return address on the stack it was called on, so changing the
 * stack pointer changes which return address the final ret uses. Every thread
 * that is not running is sitting inside that one function, and resuming it is
 * letting it finish.
 *
 * What is deliberately not here: preemption, priorities, blocking, and any
 * notion of a second processor. The run queue is a linear scan over a fixed
 * table because that is predictable enough for a self-test to pin exact
 * rotations, exactly as heap.c's first fit is.
 */

/*
 * The seven values thread_switch_context pops before its ret, plus the return
 * address that ret consumes. A new thread's stack is built to look like it is
 * already suspended inside that function.
 */
#define PREPARED_SLOTS 8U
#define PREPARED_FRAME_SIZE (PREPARED_SLOTS * sizeof(uint64_t))

/*
 * Offsets into that frame, counted in slots from the saved stack pointer, in
 * the order the pops consume them.
 */
#define SLOT_R15 0U
#define SLOT_R14 1U
#define SLOT_R13 2U
#define SLOT_R12 3U
#define SLOT_RBX 4U
#define SLOT_RBP 5U
#define SLOT_RFLAGS 6U
#define SLOT_RETURN 7U

/*
 * The flags a thread starts life with.
 *
 * Intel SDM volume 1 section 3.4.3: bit 1 of RFLAGS is reserved and always
 * reads as one, and bit 9 is the interrupt enable. The direction flag stays
 * clear, as the ABI requires of any function entry.
 *
 * Interrupts start *enabled*, and that is load-bearing rather than incidental.
 * A thread that begins with them disabled and never yields can never be
 * preempted, because the quantum that would take the processor back is an
 * interrupt it is refusing to take - so the first such thread scheduled locks
 * the machine. This layer shipped with them disabled, which was invisible for
 * exactly as long as every thread yielded voluntarily; the preemption proof
 * hung on its first run. A schedulable thread is an interruptible one.
 *
 * The consequence is worth stating: a thread cannot assume it holds a critical
 * section on entry. Anything that needs one takes it itself.
 */
#define RFLAGS_RESERVED UINT64_C(0x0000000000000002)
#define RFLAGS_INTERRUPT_ENABLE UINT64_C(0x0000000000000200)
#define RFLAGS_DIRECTION UINT64_C(0x0000000000000400)
#define INITIAL_RFLAGS (RFLAGS_RESERVED | RFLAGS_INTERRUPT_ENABLE)

struct thread {
    uint64_t stack_pointer;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t identifier;
    uint64_t switches;
    thread_entry_t entry;
    void *context;
    enum thread_state state;
    bool boot_thread;
};

static struct thread_system_state state;
static struct thread *threads;
static size_t current_slot;
static uint64_t next_identifier;

/*
 * Set by the quantum callback inside the timer interrupt, read and cleared by
 * thread_on_interrupt_return once that interrupt has been acknowledged.
 * Volatile because the only thing ordering those two is the hardware.
 */
static volatile bool reschedule_pending;
static uint64_t quantum_identifier;

static void quantum_expired(uint64_t deadline_ns, void *context);

static uint64_t slot_guard_page(size_t slot)
{
    return THREAD_STACK_REGION + (uint64_t)slot * THREAD_STACK_STRIDE;
}

static uint64_t slot_stack_base(size_t slot)
{
    return slot_guard_page(slot) + PAGING_PAGE_SIZE;
}

static uint64_t slot_stack_top(size_t slot)
{
    return slot_stack_base(slot) + THREAD_STACK_SIZE;
}

static struct thread *find(uint64_t identifier)
{
    if (identifier == THREAD_ID_NONE || threads == NULL) {
        return NULL;
    }

    for (size_t slot = 0; slot < state.capacity; ++slot) {
        if (threads[slot].state != THREAD_STATE_UNUSED &&
            threads[slot].identifier == identifier) {
            return &threads[slot];
        }
    }

    return NULL;
}

/*
 * The whole scheduling policy: the next ready thread after this one, wrapping,
 * and this one again only if nothing else is ready. Deterministic, so a test
 * can require an exact rotation rather than merely that everything eventually
 * ran.
 */
static size_t next_ready_slot(size_t from)
{
    for (size_t step = 1; step <= state.capacity; ++step) {
        const size_t slot = (from + step) % state.capacity;

        if (threads[slot].state == THREAD_STATE_READY) {
            return slot;
        }
    }

    return from;
}

static void account_switch(size_t to)
{
    threads[to].switches += 1U;
    state.switches += 1U;
    state.current = threads[to].identifier;
    current_slot = to;
}

static void recount(void)
{
    state.live = 0U;
    state.ready = 0U;
    state.exited = 0U;

    for (size_t slot = 0; slot < state.capacity; ++slot) {
        switch (threads[slot].state) {
        case THREAD_STATE_READY:
            ++state.ready;
            ++state.live;
            break;
        case THREAD_STATE_RUNNING:
            ++state.live;
            break;
        case THREAD_STATE_EXITED:
            ++state.exited;
            break;
        case THREAD_STATE_UNUSED:
        default:
            break;
        }
    }
}

static void clear_slot(struct thread *thread)
{
    thread->stack_pointer = 0U;
    thread->stack_base = 0U;
    thread->stack_top = 0U;
    thread->identifier = THREAD_ID_NONE;
    thread->switches = 0U;
    thread->entry = NULL;
    thread->context = NULL;
    thread->state = THREAD_STATE_UNUSED;
    thread->boot_thread = false;
}

/*
 * Map a slot's stack and leave its guard page absent. The frames need not be
 * contiguous, so they are taken and mapped one at a time, and a failure part
 * way through unwinds what it already did - a half-mapped stack that is then
 * used is a fault at an address nothing can explain.
 */
static enum thread_status map_stack(size_t slot)
{
    const uint64_t base = slot_stack_base(slot);
    size_t mapped = 0U;

    for (size_t page = 0; page < THREAD_STACK_PAGES; ++page) {
        const uint64_t address = base + (uint64_t)page * PAGING_PAGE_SIZE;
        uintptr_t frame = 0U;

        if (frame_allocate(&frame) != FRAME_STATUS_OK) {
            break;
        }

        if (paging_map(address, (uint64_t)frame, PAGING_PAGE_SIZE,
                PAGING_WRITE) != PAGING_STATUS_OK) {
            (void)frame_release(frame);
            break;
        }

        ++mapped;
    }

    if (mapped != THREAD_STACK_PAGES) {
        for (size_t page = 0; page < mapped; ++page) {
            const uint64_t address = base + (uint64_t)page * PAGING_PAGE_SIZE;
            struct paging_translation translation;

            if (paging_translate(address, &translation) == PAGING_STATUS_OK &&
                paging_unmap(address, PAGING_PAGE_SIZE) == PAGING_STATUS_OK) {
                (void)frame_release((uintptr_t)translation.physical_address);
            }
        }

        return THREAD_STATUS_OUT_OF_FRAMES;
    }

    state.stack_frames += THREAD_STACK_PAGES;
    return THREAD_STATUS_OK;
}

static enum thread_status unmap_stack(size_t slot)
{
    const uint64_t base = slot_stack_base(slot);

    for (size_t page = 0; page < THREAD_STACK_PAGES; ++page) {
        const uint64_t address = base + (uint64_t)page * PAGING_PAGE_SIZE;
        struct paging_translation translation;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }

        if (paging_unmap(address, PAGING_PAGE_SIZE) != PAGING_STATUS_OK) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }

        if (frame_release((uintptr_t)translation.physical_address) !=
            FRAME_STATUS_OK) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }

        --state.stack_frames;
    }

    return THREAD_STATUS_OK;
}

/*
 * Build the frame the first switch onto this stack will consume. Written
 * through a volatile pointer so the compiler cannot decide that memory no C
 * code reads is dead: the only thing that ever reads it is the pop sequence in
 * thread_switch_context.
 */
static void prepare_frame(struct thread *thread)
{
    volatile uint64_t *frame =
        (volatile uint64_t *)(uintptr_t)(thread->stack_top -
            PREPARED_FRAME_SIZE);

    for (size_t slot = 0; slot < PREPARED_SLOTS; ++slot) {
        frame[slot] = 0U;
    }

    /*
     * The trampoline reads the entry function from R12 and its argument from
     * R13. They travel as ordinary callee-saved registers, so the switch that
     * starts the thread delivers them at no extra cost.
     */
    frame[SLOT_R12] = (uint64_t)(uintptr_t)thread->entry;
    frame[SLOT_R13] = (uint64_t)(uintptr_t)thread->context;
    frame[SLOT_RFLAGS] = INITIAL_RFLAGS;
    frame[SLOT_RETURN] = (uint64_t)(uintptr_t)&thread_trampoline;
    thread->stack_pointer = thread->stack_top - PREPARED_FRAME_SIZE;
}

enum thread_status thread_start(void)
{
    void *table = NULL;
    struct thread *boot;

    if (state.active) {
        return THREAD_STATUS_ALREADY_STARTED;
    }

    /*
     * The table comes from the heap and the stacks come from the page tables,
     * so both have to be up. Interrupts stay off because the run queue is not
     * reentrant and nothing yet arrives to make it so.
     */
    if (cpu_interrupts_enabled()) {
        return THREAD_STATUS_INTERRUPTS_ENABLED;
    }

    if (!heap_is_active() || !paging_is_active()) {
        return THREAD_STATUS_NO_HEAP;
    }

    if (heap_allocate((uint64_t)THREAD_MAX * sizeof(struct thread), &table) !=
        HEAP_STATUS_OK) {
        return THREAD_STATUS_NO_MEMORY;
    }

    threads = (struct thread *)table;
    state.capacity = THREAD_MAX;
    state.switches = 0U;
    state.stack_frames = 0U;
    next_identifier = 1U;

    for (size_t slot = 0; slot < state.capacity; ++slot) {
        clear_slot(&threads[slot]);
    }

    /*
     * Slot zero is the context this kernel has been running on since boot.S. It
     * is adopted rather than created: its stack already exists, this layer did
     * not map it, and it has no prepared frame because it is not suspended -
     * it is the thing doing the adopting. Its stack pointer becomes real the
     * first time it switches away.
     *
     * Its stack slot in the region is deliberately left unmapped, so slot index
     * and stack address stay the same arithmetic for every thread.
     */
    boot = &threads[0];
    boot->identifier = next_identifier++;
    boot->state = THREAD_STATE_RUNNING;
    boot->boot_thread = true;
    boot->stack_base = 0U;
    boot->stack_top = 0U;
    current_slot = 0U;
    state.current = boot->identifier;
    state.preemptions = 0U;
    state.preemptive = false;
    reschedule_pending = false;
    quantum_identifier = THREAD_ID_NONE;
    state.active = true;
    recount();
    return THREAD_STATUS_OK;
}

enum thread_status thread_create(
    thread_entry_t entry,
    void *context,
    uint64_t *identifier
)
{
    struct thread *thread = NULL;
    size_t chosen = 0U;
    enum thread_status status;

    if (entry == NULL || identifier == NULL) {
        return THREAD_STATUS_NULL_ARGUMENT;
    }

    *identifier = THREAD_ID_NONE;

    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    if (cpu_interrupts_enabled()) {
        return THREAD_STATUS_INTERRUPTS_ENABLED;
    }

    /*
     * Slot zero is never reused: it belongs to the boot thread for as long as
     * the system is up, and its stack region is the one nothing maps.
     */
    for (size_t slot = 1; slot < state.capacity; ++slot) {
        if (threads[slot].state == THREAD_STATE_UNUSED) {
            thread = &threads[slot];
            chosen = slot;
            break;
        }
    }

    if (thread == NULL) {
        return THREAD_STATUS_NO_CAPACITY;
    }

    status = map_stack(chosen);

    if (status != THREAD_STATUS_OK) {
        return status;
    }

    thread->identifier = next_identifier++;
    thread->entry = entry;
    thread->context = context;
    thread->stack_base = slot_stack_base(chosen);
    thread->stack_top = slot_stack_top(chosen);
    thread->switches = 0U;
    thread->boot_thread = false;
    prepare_frame(thread);
    thread->state = THREAD_STATE_READY;
    recount();
    *identifier = thread->identifier;
    return THREAD_STATUS_OK;
}

/*
 * Hand the processor to the next ready thread, if there is one.
 *
 * The run queue is mutated with interrupts disabled and restored afterwards to
 * whatever this thread had. Once a timer can preempt, a quantum expiring
 * halfway through this function would leave the table describing a switch that
 * had not happened - so the window is closed rather than argued about.
 *
 * `interrupted` lives on this thread's own stack, so when this thread is
 * resumed - possibly minutes later, from another thread's quantum - it still
 * holds this thread's answer rather than the resumer's.
 */
static void switch_to_next(void)
{
    const bool interrupted = cpu_interrupts_enabled();
    size_t previous;
    size_t next;

    cpu_interrupt_disable();
    previous = current_slot;
    next = next_ready_slot(previous);

    /*
     * Nothing else is ready. Returning is the honest answer: the caller asked
     * to be descheduled and there is no one to schedule, so it keeps running.
     */
    if (next == previous) {
        if (interrupted) {
            cpu_interrupt_enable();
        }

        return;
    }

    if (threads[previous].state == THREAD_STATE_RUNNING) {
        threads[previous].state = THREAD_STATE_READY;
    }

    threads[next].state = THREAD_STATE_RUNNING;
    account_switch(next);
    recount();

    /*
     * Everything after this call happens on the other thread's stack. This
     * returns only when something switches back, which may be much later and
     * from anywhere.
     */
    thread_switch_context(&threads[previous].stack_pointer,
        threads[next].stack_pointer);

    if (interrupted) {
        cpu_interrupt_enable();
    }
}

void thread_yield(void)
{
    if (!state.active) {
        return;
    }

    switch_to_next();
}

static enum thread_status arm_quantum(void)
{
    const uint64_t deadline = clock_monotonic_ns() + THREAD_QUANTUM_NS;

    if (timer_arm(deadline, quantum_expired, NULL, &quantum_identifier) !=
        TIMER_STATUS_OK) {
        quantum_identifier = THREAD_ID_NONE;
        return THREAD_STATUS_NO_QUANTUM;
    }

    return THREAD_STATUS_OK;
}

/*
 * The end of a thread's turn, running inside the timer interrupt.
 *
 * It does two things and neither of them is the switch. It arms the next
 * quantum - unconditionally, so that a quantum which finds nothing to switch to
 * still leaves a successor behind - and it records that a switch is owed. The
 * switch itself waits for thread_on_interrupt_return, because the end of
 * interrupt has not been sent yet and a callback that switched away would
 * strand it.
 */
static void quantum_expired(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;
    (void)context;

    quantum_identifier = THREAD_ID_NONE;

    if (!state.active || !state.preemptive) {
        return;
    }

    if (arm_quantum() != THREAD_STATUS_OK) {
        console_panic("the scheduler could not arm its next quantum");
    }

    /*
     * Only worth a switch if somebody else is ready. A single runnable thread
     * preempted into itself would count a preemption that never happened.
     */
    if (next_ready_slot(current_slot) != current_slot) {
        reschedule_pending = true;
        state.preemptions += 1U;
    }
}

void thread_on_interrupt_return(void)
{
    if (!reschedule_pending) {
        return;
    }

    reschedule_pending = false;

    if (!state.active) {
        return;
    }

    switch_to_next();
}

enum thread_status thread_enable_preemption(void)
{
    enum thread_status status;

    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    if (state.preemptive) {
        return THREAD_STATUS_ALREADY_PREEMPTIVE;
    }

    /*
     * The quantum is a deadline, so the deadline layer has to be running. This
     * is the refusal that keeps the dependency explicit rather than letting a
     * scheduler start that can never be interrupted.
     */
    if (!timer_is_started()) {
        return THREAD_STATUS_NO_TIMER;
    }

    state.preemptive = true;
    status = arm_quantum();

    if (status != THREAD_STATUS_OK) {
        state.preemptive = false;
    }

    return status;
}

enum thread_status thread_disable_preemption(void)
{
    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    if (!state.preemptive) {
        return THREAD_STATUS_NOT_PREEMPTIVE;
    }

    state.preemptive = false;

    if (quantum_identifier != THREAD_ID_NONE) {
        (void)timer_cancel(quantum_identifier);
        quantum_identifier = THREAD_ID_NONE;
    }

    /*
     * A quantum may already have fired and be waiting to be honoured. Dropping
     * it here means disabling preemption cannot be followed by one last
     * involuntary switch.
     */
    reschedule_pending = false;
    return THREAD_STATUS_OK;
}

bool thread_preemption_enabled(void)
{
    return state.active && state.preemptive;
}

_Noreturn void thread_exit(void)
{
    size_t previous;
    size_t next;

    if (!state.active) {
        console_panic("a thread exited before threads were started");
    }

    previous = current_slot;

    /*
     * The boot thread cannot exit: nothing created it, its stack is the
     * kernel's, and there would be nothing to return to.
     */
    if (threads[previous].boot_thread) {
        console_panic("the boot thread cannot exit");
    }

    threads[previous].state = THREAD_STATE_EXITED;
    next = next_ready_slot(previous);

    /*
     * An exited thread is never ready, so next_ready_slot returning this slot
     * means there is genuinely nothing left to run. That is a scheduler with no
     * runnable thread and no way to get one, which is a deadlock rather than an
     * idle state, and it is named rather than halted into.
     */
    if (next == previous) {
        console_panic("the last runnable thread exited");
    }

    threads[next].state = THREAD_STATE_RUNNING;
    account_switch(next);
    recount();

    /*
     * The stack this is running on stays mapped. Unmapping it here would pull
     * the ground out from under the switch that has not happened yet; it is
     * released by thread_stop, and docs/THREADS.md records that as deferred for
     * the same reason the heap never shrinks.
     */
    thread_switch_context(&threads[previous].stack_pointer,
        threads[next].stack_pointer);

    console_panic("an exited thread was scheduled again");
}

enum thread_status thread_join(uint64_t identifier)
{
    const struct thread *target;

    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    target = find(identifier);

    if (target == NULL) {
        return THREAD_STATUS_BAD_IDENTIFIER;
    }

    if (target->identifier == threads[current_slot].identifier) {
        return THREAD_STATUS_BAD_IDENTIFIER;
    }

    /*
     * Cooperative, so waiting is yielding. The loop terminates because a thread
     * that never exits keeps being scheduled by these yields rather than
     * starved by them - which is also why this is not a spin.
     */
    while (target->state != THREAD_STATE_EXITED) {
        thread_yield();
    }

    return THREAD_STATUS_OK;
}

enum thread_status thread_stop(void)
{
    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    if (cpu_interrupts_enabled()) {
        return THREAD_STATUS_INTERRUPTS_ENABLED;
    }

    if (!threads[current_slot].boot_thread) {
        return THREAD_STATUS_NOT_THE_BOOT_THREAD;
    }

    /*
     * Stop being preemptible before dismantling the table the scheduler reads.
     * A quantum landing between here and the last free would find a run queue
     * that is being taken apart.
     */
    if (state.preemptive) {
        (void)thread_disable_preemption();
    }

    /*
     * Every other thread must have exited. Tearing down while one is merely
     * ready would unmap a stack that still holds a suspended thread's frame,
     * and the failure would surface as a fault at an address belonging to a
     * thread that no longer exists.
     */
    for (size_t slot = 1; slot < state.capacity; ++slot) {
        if (threads[slot].state == THREAD_STATE_READY ||
            threads[slot].state == THREAD_STATE_RUNNING) {
            return THREAD_STATUS_THREADS_STILL_RUNNABLE;
        }
    }

    for (size_t slot = 1; slot < state.capacity; ++slot) {
        if (threads[slot].state == THREAD_STATE_EXITED) {
            const enum thread_status status = unmap_stack(slot);

            if (status != THREAD_STATUS_OK) {
                return status;
            }

            clear_slot(&threads[slot]);
        }
    }

    if (state.stack_frames != 0U) {
        return THREAD_STATUS_VALIDATION_FAILURE;
    }

    if (heap_free(threads) != HEAP_STATUS_OK) {
        return THREAD_STATUS_VALIDATION_FAILURE;
    }

    threads = NULL;
    state.active = false;
    state.preemptive = false;
    reschedule_pending = false;
    quantum_identifier = THREAD_ID_NONE;
    state.capacity = 0U;
    state.live = 0U;
    state.ready = 0U;
    state.exited = 0U;
    state.current = THREAD_ID_NONE;
    current_slot = 0U;
    return THREAD_STATUS_OK;
}

bool thread_is_started(void)
{
    return state.active;
}

uint64_t thread_current(void)
{
    return state.active ? threads[current_slot].identifier : THREAD_ID_NONE;
}

enum thread_status thread_report(
    uint64_t identifier,
    struct thread_state_report *report
)
{
    const struct thread *thread;
    size_t slot;

    if (report == NULL) {
        return THREAD_STATUS_NULL_ARGUMENT;
    }

    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    thread = find(identifier);

    if (thread == NULL) {
        return THREAD_STATUS_BAD_IDENTIFIER;
    }

    slot = (size_t)(thread - threads);
    report->identifier = thread->identifier;
    report->stack_base = thread->stack_base;
    report->stack_top = thread->stack_top;
    report->guard_page = thread->boot_thread ? 0U : slot_guard_page(slot);
    report->switches = thread->switches;
    report->state = thread->state;
    report->boot_thread = thread->boot_thread;
    return THREAD_STATUS_OK;
}

struct thread_system_state thread_get_state(void)
{
    return state;
}

/*
 * Re-derive what the table claims from the address space it claims it. Called
 * at the end of boot for the same reason paging_verify and heap_verify are:
 * every thread that ran did so on a stack this asserts is still there, still
 * mapped writable, and still preceded by a guard page nothing mapped.
 */
enum thread_status thread_verify(void)
{
    size_t live = 0U;
    size_t running = 0U;

    if (!state.active) {
        return THREAD_STATUS_NOT_STARTED;
    }

    if (threads == NULL) {
        return THREAD_STATUS_VALIDATION_FAILURE;
    }

    for (size_t slot = 0; slot < state.capacity; ++slot) {
        const struct thread *thread = &threads[slot];
        struct paging_translation translation;

        if (thread->state == THREAD_STATE_UNUSED) {
            continue;
        }

        ++live;

        if (thread->state == THREAD_STATE_RUNNING) {
            ++running;
        }

        if (thread->boot_thread) {
            if (slot != 0U || thread->stack_top != 0U) {
                return THREAD_STATUS_VALIDATION_FAILURE;
            }

            continue;
        }

        if (thread->stack_base != slot_stack_base(slot) ||
            thread->stack_top != slot_stack_top(slot)) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }

        /*
         * The guard must be absent and the stack must be present. Checking both
         * is what makes this a statement about the address space rather than
         * about the table's own arithmetic.
         */
        if (paging_translate(slot_guard_page(slot), &translation) !=
            PAGING_STATUS_NOT_MAPPED) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }

        for (size_t page = 0; page < THREAD_STACK_PAGES; ++page) {
            const uint64_t address =
                thread->stack_base + (uint64_t)page * PAGING_PAGE_SIZE;

            if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
                translation.permissions != PAGING_WRITE ||
                translation.level != 1U) {
                return THREAD_STATUS_VALIDATION_FAILURE;
            }
        }

        /*
         * A suspended thread's stack pointer must point inside its own stack.
         * This is the check that would catch a switch that recorded the wrong
         * one, which is otherwise only visible as a resumed thread running on
         * somebody else's frame.
         */
        if (thread->state != THREAD_STATE_EXITED &&
            thread->state != THREAD_STATE_RUNNING &&
            (thread->stack_pointer < thread->stack_base ||
                thread->stack_pointer > thread->stack_top)) {
            return THREAD_STATUS_VALIDATION_FAILURE;
        }
    }

    if (running != 1U || live != state.live + state.exited) {
        return THREAD_STATUS_VALIDATION_FAILURE;
    }

    return THREAD_STATUS_OK;
}

const char *thread_state_string(enum thread_state thread_state)
{
    switch (thread_state) {
    case THREAD_STATE_READY:
        return "ready";
    case THREAD_STATE_RUNNING:
        return "running";
    case THREAD_STATE_EXITED:
        return "exited";
    case THREAD_STATE_UNUSED:
    default:
        return "unused";
    }
}

const char *thread_status_string(enum thread_status status)
{
    static const char *const messages[] = {
        "ok",
        "null thread argument",
        "threads are already started",
        "threads are not started",
        "thread operation needs interrupts disabled",
        "threads need the kernel heap and page tables",
        "thread table could not be allocated",
        "thread capacity is exhausted",
        "no physical frame for a thread stack",
        "a thread stack could not be mapped",
        "no thread carries that identifier",
        "only the boot thread may stop the scheduler",
        "a thread is still runnable",
        "preemption needs the deadline timer started",
        "the scheduler could not arm a quantum",
        "preemption is already enabled",
        "preemption is not enabled",
        "thread table does not match the address space"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)THREAD_STATUS_VALIDATION_FAILURE + 1U,
        "thread status messages are out of sync"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown thread status";
    }

    return messages[status];
}

/*
 * Everything below is arithmetic and table walking over a private table. It
 * runs before any stack is mapped and switches to nothing, so a wrong stack
 * address or a wrong rotation is named here rather than discovered as a thread
 * running on another thread's frame.
 */

static struct thread self_test_threads[THREAD_MAX];

static bool stack_layout_is_disjoint(void)
{
    /*
     * Every slot's guard sits directly above the previous slot's last stack
     * byte, so a stack that runs off either end meets a page that is never
     * mapped. Checked as an ordering rather than asserted in a comment.
     */
    for (size_t slot = 0; slot < THREAD_MAX; ++slot) {
        if (slot_stack_base(slot) != slot_guard_page(slot) +
                PAGING_PAGE_SIZE ||
            slot_stack_top(slot) != slot_stack_base(slot) +
                THREAD_STACK_SIZE) {
            return false;
        }

        if (slot + 1U < THREAD_MAX &&
            slot_stack_top(slot) != slot_guard_page(slot + 1U)) {
            return false;
        }
    }

    /* No stack may overlap another, and none may reach the heap's window. */
    for (size_t left = 0; left < THREAD_MAX; ++left) {
        if (slot_stack_base(left) < THREAD_STACK_REGION) {
            return false;
        }

        for (size_t right = left + 1U; right < THREAD_MAX; ++right) {
            if (slot_stack_base(left) < slot_stack_top(right) &&
                slot_stack_base(right) < slot_stack_top(left)) {
                return false;
            }
        }
    }

    return slot_guard_page(0U) == THREAD_STACK_REGION;
}

static bool rotation_is_round_robin(void)
{
    const struct thread_system_state saved_state = state;
    struct thread *const saved_threads = threads;
    const size_t saved_slot = current_slot;
    bool correct;

    for (size_t slot = 0; slot < THREAD_MAX; ++slot) {
        clear_slot(&self_test_threads[slot]);
    }

    threads = self_test_threads;
    state.capacity = THREAD_MAX;

    self_test_threads[0].state = THREAD_STATE_RUNNING;
    self_test_threads[2].state = THREAD_STATE_READY;
    self_test_threads[5].state = THREAD_STATE_READY;

    /* Forward from each position, wrapping, skipping everything not ready. */
    correct = next_ready_slot(0U) == 2U &&
        next_ready_slot(2U) == 5U &&
        next_ready_slot(5U) == 2U &&
        next_ready_slot(3U) == 5U &&
        next_ready_slot(6U) == 2U;

    /* An exited thread is never chosen, however recently it ran. */
    self_test_threads[2].state = THREAD_STATE_EXITED;
    correct = correct && next_ready_slot(0U) == 5U;

    /* With nothing ready, the caller keeps the processor rather than a wrong
     * slot being invented. */
    self_test_threads[5].state = THREAD_STATE_EXITED;
    correct = correct && next_ready_slot(0U) == 0U &&
        next_ready_slot(3U) == 3U;

    /* A running thread is not ready either: it is already on the processor. */
    self_test_threads[4].state = THREAD_STATE_RUNNING;
    correct = correct && next_ready_slot(0U) == 0U;

    self_test_threads[4].state = THREAD_STATE_READY;
    correct = correct && next_ready_slot(0U) == 4U;

    threads = saved_threads;
    state = saved_state;
    current_slot = saved_slot;
    return correct;
}

static bool prepared_frame_is_right(void)
{
    static uint64_t arena[64];
    const struct thread_system_state saved_state = state;
    struct thread *const saved_threads = threads;
    struct thread thread;
    const volatile uint64_t *frame;
    bool correct;

    for (size_t index = 0; index < sizeof(arena) / sizeof(arena[0]); ++index) {
        arena[index] = UINT64_C(0xDEADBEEFDEADBEEF);
    }

    clear_slot(&thread);
    thread.entry = (thread_entry_t)(uintptr_t)&thread_trampoline;
    thread.context = (void *)(uintptr_t)UINT64_C(0x1234);
    thread.stack_top =
        (uint64_t)(uintptr_t)&arena[sizeof(arena) / sizeof(arena[0])];
    prepare_frame(&thread);

    frame = (const volatile uint64_t *)(uintptr_t)thread.stack_pointer;
    correct = thread.stack_pointer == thread.stack_top - PREPARED_FRAME_SIZE &&
        frame[SLOT_R12] == (uint64_t)(uintptr_t)thread.entry &&
        frame[SLOT_R13] == (uint64_t)(uintptr_t)thread.context &&
        frame[SLOT_RFLAGS] == INITIAL_RFLAGS &&
        frame[SLOT_RETURN] == (uint64_t)(uintptr_t)&thread_trampoline &&
        frame[SLOT_R15] == 0U && frame[SLOT_R14] == 0U &&
        frame[SLOT_RBX] == 0U && frame[SLOT_RBP] == 0U;

    /*
     * The reserved bit must be set, the direction flag must be clear - a thread
     * that started with it set would corrupt every string operation it made -
     * and interrupts must be *enabled*, because a thread that cannot take an
     * interrupt cannot be preempted and will hold the processor for ever.
     */
    correct = correct && (INITIAL_RFLAGS & RFLAGS_RESERVED) != 0U &&
        (INITIAL_RFLAGS & RFLAGS_INTERRUPT_ENABLE) != 0U &&
        (INITIAL_RFLAGS & RFLAGS_DIRECTION) == 0U;

    /* Nothing below the frame was touched, so a stack cannot be over-written
     * on creation. */
    correct = correct &&
        arena[sizeof(arena) / sizeof(arena[0]) - PREPARED_SLOTS - 1U] ==
            UINT64_C(0xDEADBEEFDEADBEEF);

    threads = saved_threads;
    state = saved_state;
    return correct;
}

static bool refusals_are_named(void)
{
    const struct thread_system_state saved_state = state;
    struct thread *const saved_threads = threads;
    struct thread_state_report report;
    uint64_t identifier = UINT64_C(0xFFFF);
    bool correct;

    threads = NULL;
    state.active = false;
    state.capacity = 0U;

    correct = thread_create(NULL, NULL, &identifier) ==
            THREAD_STATUS_NULL_ARGUMENT &&
        thread_create((thread_entry_t)(uintptr_t)&thread_trampoline, NULL,
            NULL) == THREAD_STATUS_NULL_ARGUMENT &&
        thread_report(1U, NULL) == THREAD_STATUS_NULL_ARGUMENT;

    /* Every entry point refuses by name before the system is started. */
    correct = correct &&
        thread_create((thread_entry_t)(uintptr_t)&thread_trampoline, NULL,
            &identifier) == THREAD_STATUS_NOT_STARTED &&
        identifier == THREAD_ID_NONE &&
        thread_stop() == THREAD_STATUS_NOT_STARTED &&
        thread_join(1U) == THREAD_STATUS_NOT_STARTED &&
        thread_report(1U, &report) == THREAD_STATUS_NOT_STARTED &&
        thread_verify() == THREAD_STATUS_NOT_STARTED &&
        thread_current() == THREAD_ID_NONE &&
        !thread_is_started();

    /* Yielding before there is anything to yield to is a no-op, not a fault. */
    thread_yield();

    /* An identifier of zero names nothing, and neither does a stale one. */
    state.active = true;
    state.capacity = THREAD_MAX;
    threads = self_test_threads;

    for (size_t slot = 0; slot < THREAD_MAX; ++slot) {
        clear_slot(&self_test_threads[slot]);
    }

    self_test_threads[0].state = THREAD_STATE_RUNNING;
    self_test_threads[0].identifier = 1U;
    self_test_threads[0].boot_thread = true;
    current_slot = 0U;

    correct = correct &&
        find(THREAD_ID_NONE) == NULL &&
        find(UINT64_C(9999)) == NULL &&
        thread_join(UINT64_C(9999)) == THREAD_STATUS_BAD_IDENTIFIER &&
        thread_report(UINT64_C(9999), &report) ==
            THREAD_STATUS_BAD_IDENTIFIER;

    /* A thread may not wait for itself: it would wait forever. */
    correct = correct && thread_join(1U) == THREAD_STATUS_BAD_IDENTIFIER;

    /* The boot thread's report names no guard, because it has no mapped stack. */
    correct = correct &&
        thread_report(1U, &report) == THREAD_STATUS_OK &&
        report.boot_thread && report.guard_page == 0U &&
        report.state == THREAD_STATE_RUNNING;

    threads = saved_threads;
    state = saved_state;
    return correct;
}

bool thread_self_test(void)
{
    return stack_layout_is_disjoint() &&
        rotation_is_round_robin() &&
        prepared_frame_is_right() &&
        refusals_are_named();
}
