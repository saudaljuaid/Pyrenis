/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_THREAD_H
#define SENERI_THREAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/paging.h>

/*
 * More than one thread of control.
 *
 * Everything below this layer runs on the single stack boot.S sets up, so any
 * operation that has to wait blocks the whole machine - which is why
 * timer_sleep_ns halts rather than yields, and why no driver can own a device
 * that takes milliseconds to answer. A thread is what a wait can belong to.
 *
 * This layer is cooperative: a thread runs until it calls thread_yield or
 * returns. Preemption is the increment after, and the context switch here saves
 * the flags register precisely so that adding it does not silently change
 * whether a resumed thread has interrupts enabled.
 */

/*
 * Where thread stacks live. Above the kernel heap's window and its guard, so a
 * stack cannot collide with a heap block, and far above the identity map.
 */
#define THREAD_STACK_REGION UINT64_C(0x0000000800000000)

/*
 * Four pages, which is the same 16 KiB boot.S gives the thread this kernel
 * starts on. Nothing here needs more, and a thread that does should say so
 * rather than discover it as a guard fault.
 */
#define THREAD_STACK_PAGES 4U
#define THREAD_STACK_SIZE (THREAD_STACK_PAGES * PAGING_PAGE_SIZE)

/*
 * One unmapped page below every stack. The stride places each slot's guard
 * directly above the previous slot's highest stack address, so a stack that
 * runs off either end lands on a page that is never mapped rather than in a
 * neighbouring thread's frame. A stack overflow is a page fault naming the
 * guard, which is the same guarantee the kernel heap makes about its window.
 */
#define THREAD_STACK_STRIDE (THREAD_STACK_SIZE + PAGING_PAGE_SIZE)

/*
 * A Seneri policy bound on how many threads may exist at once, not an
 * architectural one. The table is one kernel heap allocation made at
 * thread_start and released at thread_stop, following the pattern
 * src/kernel/timer.c set: never per operation.
 */
#define THREAD_MAX 8U

/* Identifiers are never zero, so zero can mean "no thread" without a flag. */
#define THREAD_ID_NONE UINT64_C(0)

enum thread_state {
    THREAD_STATE_UNUSED = 0,
    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,
    THREAD_STATE_EXITED
};

enum thread_status {
    THREAD_STATUS_OK = 0,
    THREAD_STATUS_NULL_ARGUMENT,
    THREAD_STATUS_ALREADY_STARTED,
    THREAD_STATUS_NOT_STARTED,
    THREAD_STATUS_INTERRUPTS_ENABLED,
    THREAD_STATUS_NO_HEAP,
    THREAD_STATUS_NO_MEMORY,
    THREAD_STATUS_NO_CAPACITY,
    THREAD_STATUS_OUT_OF_FRAMES,
    THREAD_STATUS_MAPPING_FAILURE,
    THREAD_STATUS_BAD_IDENTIFIER,
    THREAD_STATUS_NOT_THE_BOOT_THREAD,
    THREAD_STATUS_THREADS_STILL_RUNNABLE,
    THREAD_STATUS_VALIDATION_FAILURE
};

/*
 * Runs on its own stack with interrupts in whatever state its creator had. It
 * may yield, and it may return - returning is how a thread exits, and the
 * trampoline that called it turns that into thread_exit rather than into a
 * return to nowhere.
 */
typedef void (*thread_entry_t)(void *context);

struct thread_state_report {
    uint64_t identifier;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t guard_page;
    uint64_t switches;
    enum thread_state state;
    bool boot_thread;
};

struct thread_system_state {
    bool active;
    size_t capacity;
    size_t live;
    size_t ready;
    size_t exited;
    uint64_t current;
    uint64_t switches;
    uint64_t stack_frames;
};

/*
 * Adopt the context this kernel is already running on as the first thread, so
 * that switching away from it can come back to it. It is the only thread whose
 * stack this layer did not allocate, and the only one that may stop the system.
 */
enum thread_status thread_start(void);
enum thread_status thread_stop(void);
bool thread_is_started(void);

enum thread_status thread_create(
    thread_entry_t entry,
    void *context,
    uint64_t *identifier
);

/*
 * Give the processor to the next ready thread. With nothing else ready this
 * returns without switching, which is a description of the run queue rather
 * than a failure.
 */
void thread_yield(void);

/* Never returns. Reached implicitly when a thread's entry function returns. */
_Noreturn void thread_exit(void);

/* Yield until the named thread has exited. */
enum thread_status thread_join(uint64_t identifier);

uint64_t thread_current(void);
enum thread_status thread_report(
    uint64_t identifier,
    struct thread_state_report *report
);
struct thread_system_state thread_get_state(void);
enum thread_status thread_verify(void);
bool thread_self_test(void);
const char *thread_status_string(enum thread_status status);
const char *thread_state_string(enum thread_state state);

/*
 * Save the callee-saved registers and the flags on the current stack, record
 * the resulting stack pointer, and resume the one handed in. Written in
 * assembly because the set of registers that must survive a switch is an ABI
 * fact, and because a compiler is entitled to assume a C function returns on
 * the stack it was called on.
 */
void thread_switch_context(
    uint64_t *save_stack_pointer,
    uint64_t load_stack_pointer
);

/*
 * Where a newly created thread begins. It is never called; it is returned into
 * by the first switch onto that thread's stack.
 */
void thread_trampoline(void);

#endif
