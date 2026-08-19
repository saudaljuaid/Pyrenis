/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * What boot proves before it calls a layer established.
 *
 * Every subsystem in Seneri carries a self-test that runs on synthetic data,
 * and every subsystem is also exercised here against the machine it actually
 * booted on. Those are different claims: a self-test says the arithmetic is
 * right, a proof says the hardware agreed.
 *
 * These lived in kernel.c until that file reached 2,211 lines and stopped
 * having a single responsibility. docs/DEBT.md predicted the cost of leaving
 * them there and then measured it - the increment after the warning added 221
 * more lines. They are here so kernel.c can be read as what it is: the order
 * boot happens in.
 *
 * The rule for this file: a function here either proves something and panics
 * when it is not true, or brings a layer up and panics when it will not come
 * up. Nothing here returns a status a caller could ignore.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/boot.h>
#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/framebuffer.h>
#include <seneri/font.h>
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/logo.h>
#include <seneri/ioapic.h>
#include <seneri/keyboard.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
#include <seneri/pci.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>
#include <seneri/screen.h>
#include <seneri/self_test.h>
#include <seneri/shell.h>
#include <seneri/test.h>
#include <seneri/thread.h>
#include <seneri/timer.h>
#include <seneri/tsc.h>
#include <seneri/boot_stages.h>

/*
 * Ten milliseconds of the ACPI power management timer, whose rate the ACPI
 * specification fixes at 3.579545 MHz. Short enough not to lengthen boot,
 * and long enough that the time-stamp counter's opinion of it is meaningful.
 */
#define PM_TIMER_PROOF_TICKS UINT32_C(35795)

/*
 * The interval the post-retirement proof compares: twenty local APIC timer ticks
 * at 100 Hz, so 200 ms. Wide enough that the PM timer and the TSC both resolve
 * it comfortably, and 4.3% of the narrowest PM timer counter's period, so the
 * measurement stays far inside a single wrap.
 */
#define CLOCK_PROOF_FREQUENCY UINT32_C(100)
#define CLOCK_PROOF_TICKS UINT64_C(20)

/*
 * The deadline the boot sleep asks for: 50 ms. Long enough that the fixed cost
 * of programming the timer is a small fraction of it, so the measured sleep can
 * be held to a tight tolerance rather than a generous one.
 */
#define SLEEP_PROOF_NS UINT64_C(50000000)

/*
 * Three threads, four rounds each. Three is the smallest number that can tell a
 * rotation from a ping-pong, and four rounds is enough that a scheduler which
 * gets the first pass right and then loses a thread is caught.
 */
#define THREAD_PROOF_THREADS 3U
#define THREAD_PROOF_ROUNDS 4U
#define THREAD_PROOF_LOG (THREAD_PROOF_THREADS * THREAD_PROOF_ROUNDS)

/*
 * How many times each preemption worker must be scheduled before the proof is
 * satisfied. Three is enough to distinguish "the scheduler rotated once" from
 * "the scheduler keeps rotating", and small enough that the whole proof fits in
 * a handful of quanta.
 */
#define PREEMPT_PROOF_TURNS 3U

/*
 * The proof refuses to wait longer than this. Every worker spins without ever
 * yielding, so a scheduler that stopped preempting would otherwise hang the
 * boot instead of failing it - and a hang is a timeout rather than a diagnosis.
 */
#define PREEMPT_PROOF_LIMIT_NS UINT64_C(2000000000)

/*
 * What the screen is cleared to before the logo is drawn: a near-black with a
 * slight blue bias, so the logo's own edges have something to sit against
 * rather than a pure black that hides how they were composited.
 */
#define BOOT_BACKGROUND_RED UINT8_C(0x08)
#define BOOT_BACKGROUND_GREEN UINT8_C(0x0A)
#define BOOT_BACKGROUND_BLUE UINT8_C(0x0E)

/*
 * The same timer, counted over both delivery paths. Proving the legacy path
 * still works after the I/O APIC path is programmed is what keeps this
 * increment reversible.
 */
void prove_timer_route(enum pit_route route)
{
    enum pit_status pit_status = pit_start(UINT32_C(100), route);

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(UINT64_C(8));

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        console_panic(pit_status_string(pit_status));
    }

    if (pit_ticks() < UINT64_C(8)) {
        console_panic("timer route delivered too few interrupts");
    }
}

/*
 * Retire the inherited interrupt path once the discovered one has been proved.
 * The 8259 pair is masked and latched shut, and the local APIC stops carrying
 * its output, so nothing can reach the processor except through the I/O APIC.
 */
void retire_legacy_interrupt_path(void)
{
    const enum pic_status pic_status = pic_retire();
    enum apic_status apic_status;

    if (pic_status != PIC_STATUS_OK) {
        console_panic(pic_status_string(pic_status));
    }

    apic_status = apic_retire_legacy_routing();

    if (apic_status != APIC_STATUS_OK) {
        console_panic(apic_status_string(apic_status));
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF) ||
        pic_set_mask(0U, false) != PIC_STATUS_RETIRED) {
        console_panic("legacy PIC did not stay retired");
    }
}

/*
 * Calibrate the local APIC timer against the ACPI power management timer and run
 * it. The APIC timer's input clock rate is not reported anywhere, so it can only
 * become a clock by being counted against one whose rate is known.
 */
void prove_apic_timer(void)
{
    enum apic_timer_status status = apic_timer_calibrate();

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    console_write("Seneri OS: local APIC timer calibrated at ");
    console_write_u64(apic_timer_counts_per_second());
    console_write(" counts per second\n");

    status = apic_timer_start(UINT32_C(100));

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    status = apic_timer_wait_for_ticks(UINT64_C(8));

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    status = apic_timer_stop();

    if (status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(status));
    }

    if (apic_timer_ticks() < UINT64_C(8)) {
        console_panic("local APIC timer delivered too few interrupts");
    }
}

/*
 * Calibrate the time-stamp counter against the same reference the APIC timer
 * used. Both now derive from a rate the specification states rather than one
 * this kernel measured, which is what lets the PIT retire.
 */
void prove_tsc(void)
{
    const enum tsc_status status = tsc_calibrate();
    struct tsc_state tsc;

    if (status != TSC_STATUS_OK) {
        console_panic(tsc_status_string(status));
    }

    tsc = tsc_get_state();
    console_write("Seneri OS: TSC calibrated at ");
    console_write_u64(tsc.frequency_hz);
    console_write(" Hz, invariant ");
    console_write(tsc.invariant ? "yes" : "no");
    console_putc('\n');
}

/*
 * Establish the reference the other two clocks are now built on. Its rate is
 * fixed by the ACPI specification rather than measured, so it is asked only to
 * demonstrate that its counter actually advances; there is nothing left on this
 * machine that could independently check it, and the whole point of the timer is
 * that it does not need checking.
 *
 * This runs before either calibration, because both now depend on it.
 */
void prove_pm_timer(void)
{
    uint64_t elapsed_ticks = 0U;
    enum pm_timer_status status;

    if (!pm_timer_is_present()) {
        console_panic(pm_timer_status_string(PM_TIMER_STATUS_ABSENT));
    }

    status = pm_timer_wait(PM_TIMER_PROOF_TICKS, &elapsed_ticks);

    if (status != PM_TIMER_STATUS_OK) {
        console_panic(pm_timer_status_string(status));
    }

    console_write("Seneri OS: PM timer counted ");
    console_write_u64(elapsed_ticks);
    console_write(" ticks in ");
    console_write_u64(pm_timer_ticks_to_nanoseconds(elapsed_ticks));
    console_write(" ns\n");
}

/*
 * Take the 8254 off the machine. Nothing measures time against it any more, so
 * it is stopped, masked at the I/O APIC and latched shut. This is the increment
 * the PM timer existed to make possible: the reference that replaced it is not
 * calibrated against anything, so retiring the PIT loses no accuracy - it
 * removes the source of a factor-of-two error the kernel could not see.
 */
void retire_pit(void)
{
    const enum pit_status status = pit_retire();

    if (status != PIT_STATUS_OK) {
        console_panic(pit_status_string(status));
    }

    if (!pit_is_retired() ||
        pit_start(UINT32_C(100), PIT_ROUTE_IO_APIC) != PIT_STATUS_RETIRED) {
        console_panic("PIT did not stay retired");
    }
}

/*
 * Prove time survives the retirement. The local APIC timer defines an interval
 * by counting its own ticks, and the PM timer and the TSC each measure it
 * without being told what it should be. All three are now derived from, or are,
 * a rate no part of this kernel measured, and the 8254 is latched shut while
 * they do it.
 */
void prove_clocks_without_pit(void)
{
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;
    enum apic_timer_status timer_status;

    if (!pit_is_retired()) {
        console_panic("clock proof ran before the PIT was retired");
    }

    timer_status = apic_timer_start(CLOCK_PROOF_FREQUENCY);

    if (timer_status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(timer_status));
    }

    start = pm_timer_read();
    tsc_start = cpu_read_tsc();
    timer_status = apic_timer_wait_for_ticks(CLOCK_PROOF_TICKS);

    if (timer_status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(timer_status));
    }

    reference_ns = tsc_span_nanoseconds(tsc_start, cpu_read_tsc());
    timer_status = apic_timer_stop();

    if (timer_status != APIC_TIMER_STATUS_OK) {
        console_panic(apic_timer_status_string(timer_status));
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        console_panic("PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = CLOCK_PROOF_TICKS * UINT64_C(1000000000) /
        CLOCK_PROOF_FREQUENCY;

    console_write("Seneri OS: clocks agree: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        console_panic("PM timer and local APIC timer disagree on interval");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        console_panic("PM timer and TSC disagree about an interval");
    }
}

/*
 * Turn three calibrated rates into time a kernel can use. Everything before this
 * could measure an interval that had already happened; nothing could name an
 * instant or ask to be woken at one. The clock supplies the instant and the
 * deadline timers supply the waking, and a sleep proves both at once: it can
 * only return on time if the clock's origin, the conversion, the one-shot count
 * and the expiry path are all right together.
 */
void prove_monotonic_time(void)
{
    struct clock_state clock;
    uint64_t before;
    uint64_t slept_ns;
    enum clock_status clock_status = clock_start();
    enum timer_status timer_status;

    if (clock_status != CLOCK_STATUS_OK) {
        console_panic(clock_status_string(clock_status));
    }

    clock = clock_get_state();
    console_write("Seneri OS: monotonic clock on ");
    console_write(clock_source_string(clock.source));
    console_putc('\n');

    timer_status = timer_start();

    if (timer_status != TIMER_STATUS_OK) {
        console_panic(timer_status_string(timer_status));
    }

    /*
     * The first fixed array in this kernel to stop being one. Its capacity is
     * now whatever timer_start obtained from the heap rather than an array
     * bound the compiler fixed.
     */
    console_write("Seneri OS: deadline table of ");
    console_write_u64(timer_capacity());
    console_write(" entries on the heap\n");

    if (timer_capacity() == 0U) {
        console_panic("deadline timers started without a table");
    }

    before = clock_monotonic_ns();
    timer_status = timer_sleep_ns(SLEEP_PROOF_NS);

    if (timer_status != TIMER_STATUS_OK) {
        console_panic(timer_status_string(timer_status));
    }

    slept_ns = clock_monotonic_ns() - before;

    console_write("Seneri OS: slept ");
    console_write_u64(slept_ns);
    console_write(" ns for a ");
    console_write_u64(SLEEP_PROOF_NS);
    console_write(" ns deadline\n");

    /*
     * A sleep may overshoot and must never undershoot: returning early is the
     * failure that would silently break every caller built on it.
     */
    if (slept_ns < SLEEP_PROOF_NS) {
        console_panic("sleep returned before its deadline");
    }

    if (!pm_timer_durations_agree(
            slept_ns,
            SLEEP_PROOF_NS,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        console_panic("sleep overshot its deadline");
    }

    if (timer_expiry_count() == 0U || timer_pending_count() != 0U) {
        console_panic("deadline timer table did not settle after a sleep");
    }

    if (clock_get_state().backward_steps != 0U) {
        console_panic("monotonic clock had to repair a backwards reading");
    }
}

/*
 * Take ownership of the page tables.
 *
 * Everything before this ran on the hierarchy boot.S builds in 32-bit mode:
 * 2048 huge pages covering the first 4 GiB, every one of them present, writable
 * and executable, because EFER.NXE is never set there and so the no-execute bit
 * does not exist on the processor. linker.ld has carried separate R, RX and RW
 * segments since day one and `make verify` has refused an RWX load segment for
 * just as long. Neither fact ever reached the hardware, and nothing noticed.
 *
 * This is the third property Seneri has found to be verified in name only,
 * after the QEMU suite that never ran and the PIT that delivered twice per
 * period. The pattern each time is the same: the check was necessary and was
 * never sufficient.
 */
void install_page_tables(
    const struct acpi_topology *topology,
    const struct acpi_mcfg *mcfg,
    const struct boot_framebuffer *framebuffer
)
{
    struct paging_state paging;
    struct paging_audit audit;
    enum paging_status status =
        paging_initialize(topology, mcfg, framebuffer);

    if (status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(status));
    }

    paging = paging_get_state();
    status = paging_audit_hierarchy(&audit);

    if (status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(status));
    }

    console_write("Seneri OS: paging root ");
    console_write_hex(paging.root_physical_address);
    console_write(" table frames ");
    console_write_u64(paging.table_frames);
    console_write(" regions ");
    console_write_u64(paging.fine_regions);
    console_write(" NX ");
    console_write(paging.no_execute_active ? "yes" : "no");
    console_write(" write protect ");
    console_write(paging.write_protect_active ? "yes" : "no");
    console_putc('\n');

    console_write("Seneri OS: paging leaves ");
    console_write_u64(audit.leaf_count);
    console_write(" writable ");
    console_write_u64(audit.writable_leaves);
    console_write(" executable ");
    console_write_u64(audit.executable_leaves);
    console_write(" both ");
    console_write_u64(audit.write_execute_leaves);
    console_putc('\n');

    /*
     * The check `make verify` cannot make. Its assertion reads the ELF file;
     * this one walks the tables the processor is now translating through.
     */
    if (audit.write_execute_leaves != 0U) {
        console_panic("an installed page is writable and executable");
    }

    if (audit.user_leaves != 0U) {
        console_panic("an installed page is reachable from user mode");
    }

    /*
     * Without either bit the permissions above are decoration: no-execute needs
     * EFER.NXE to exist at all, and a read-only page is writable from ring zero
     * unless CR0.WP is set. Reporting a guarantee neither could enforce is the
     * exact failure this increment exists to end.
     */
    if (!paging.no_execute_active || !paging.write_protect_active) {
        console_panic("W^X cannot be enforced on this processor");
    }

    console_write("Seneri OS: kernel page tables installed\n");
    console_write("Seneri OS: no writable executable mapping\n");
}

/*
 * The mapping counterpart to prove_frame_lifecycle. A frame the allocator just
 * handed out is mapped outside the identity window, written and read back,
 * narrowed to read-only, read back again, then unmapped and released. Nothing
 * here faults, so it runs on every boot; the fault that proves the narrowing is
 * enforced by the hardware belongs to the paging scenario.
 */
void prove_paging_lifecycle(void)
{
    /*
     * The page is written and read back through a volatile pointer so the
     * compiler cannot assume it knows what a freshly mapped frame holds, and
     * cannot drop the read that proves the write reached memory.
     */
    volatile uint8_t *probe =
        (volatile uint8_t *)(uintptr_t)PAGING_PROBE_ADDRESS;
    struct paging_translation translation;
    struct frame_allocator_stats before;
    struct frame_allocator_stats after;
    uintptr_t frame;
    enum frame_status frame_status;
    enum paging_status status;

    before = frame_allocator_get_stats();
    frame_status = frame_allocate(&frame);

    if (frame_status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(frame_status));
    }

    status = paging_map(
        PAGING_PROBE_ADDRESS,
        frame,
        SENERI_PAGE_SIZE,
        PAGING_WRITE
    );

    if (status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(status));
    }

    *probe = UINT8_C(0xA5);

    if (*probe != UINT8_C(0xA5)) {
        console_panic("a writable mapping did not hold a write");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.physical_address != (uint64_t)frame ||
        translation.permissions != PAGING_WRITE ||
        translation.level != 1U) {
        console_panic("a fresh mapping does not translate to its frame");
    }

    status = paging_protect(
        PAGING_PROBE_ADDRESS,
        SENERI_PAGE_SIZE,
        PAGING_READ
    );

    if (status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(status));
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != (uint64_t)frame) {
        console_panic("narrowing a mapping changed what it points at");
    }

    if (*probe != UINT8_C(0xA5)) {
        console_panic("a read-only mapping lost the page contents");
    }

    status = paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE);

    if (status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(status));
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        console_panic("an unmapped page still translates");
    }

    frame_status = frame_release(frame);

    if (frame_status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(frame_status));
    }

    /*
     * Everything this proof took has been given back, including the interior
     * page tables the mapping needed. An unmap that cleared its leaf but kept
     * the table would show up here as a frame that never came home, which is
     * the leak the reclamation exists to close.
     */
    after = frame_allocator_get_stats();

    if (after.free_frames != before.free_frames) {
        console_panic("mapping a page and undoing it did not return every frame");
    }
}

/*
 * Turn the address space into memory a caller can ask for by the byte.
 *
 * Everything below this layer deals in whole 4 KiB frames and fixed static
 * storage: the deadline table, the ACPI topology, the interrupt handler table
 * are all sized by a compile-time policy bound because there was nowhere else
 * to put them. The heap is what replaces those bounds, and it could not exist
 * before the previous increment - it needs a virtual window it can grow into
 * one page at a time, which is exactly what paging_map now provides.
 */
void bring_up_heap(void)
{
    struct heap_state heap;
    enum heap_status status = heap_initialize();

    if (status != HEAP_STATUS_OK) {
        console_panic(heap_status_string(status));
    }

    heap = heap_get_state();
    console_write("Seneri OS: heap window ");
    console_write_hex(heap.base_address);
    console_write(" size ");
    console_write_u64(heap.size);
    console_write(" guards ");
    console_write_hex(HEAP_GUARD_BELOW);
    console_putc(' ');
    console_write_hex(HEAP_GUARD_ABOVE);
    console_putc('\n');
}

/*
 * The same shape of proof as the frame and paging lifecycles, one layer up.
 * Three allocations that must not overlap, written and read back so an overlap
 * would corrupt a pattern rather than merely look wrong in a table, then freed
 * in an order that forces a coalesce from both sides. Nothing here faults; the
 * fault that proves the guard pages belongs to the heap scenario.
 */
void prove_heap_lifecycle(void)
{
    /*
     * Written and read back through volatile pointers so the compiler cannot
     * assume it knows what memory it has never seen holds, and cannot drop the
     * reads that prove the three blocks are actually disjoint.
     */
    volatile uint8_t *first;
    volatile uint8_t *second;
    volatile uint8_t *third;
    struct heap_state heap;
    void *pointers[3] = {NULL, NULL, NULL};
    static const uint64_t sizes[3] = {64U, 4000U, 17U};
    enum heap_status status;

    for (size_t index = 0; index < 3U; ++index) {
        status = heap_allocate(sizes[index], &pointers[index]);

        if (status != HEAP_STATUS_OK) {
            console_panic(heap_status_string(status));
        }

        if (((uint64_t)(uintptr_t)pointers[index] & (HEAP_ALIGNMENT - 1U)) !=
            0U) {
            console_panic("heap returned a misaligned allocation");
        }
    }

    first = (volatile uint8_t *)pointers[0];
    second = (volatile uint8_t *)pointers[1];
    third = (volatile uint8_t *)pointers[2];

    for (uint64_t index = 0; index < sizes[0]; ++index) {
        first[index] = UINT8_C(0x11);
    }

    for (uint64_t index = 0; index < sizes[1]; ++index) {
        second[index] = UINT8_C(0x22);
    }

    for (uint64_t index = 0; index < sizes[2]; ++index) {
        third[index] = UINT8_C(0x33);
    }

    /*
     * Read every byte back after all three have been written. Checking each
     * block as it is filled would not catch a later allocation overlapping an
     * earlier one, which is the failure worth hunting here.
     */
    for (uint64_t index = 0; index < sizes[0]; ++index) {
        if (first[index] != UINT8_C(0x11)) {
            console_panic("heap allocations overlap");
        }
    }

    for (uint64_t index = 0; index < sizes[1]; ++index) {
        if (second[index] != UINT8_C(0x22)) {
            console_panic("heap allocations overlap");
        }
    }

    for (uint64_t index = 0; index < sizes[2]; ++index) {
        if (third[index] != UINT8_C(0x33)) {
            console_panic("heap allocations overlap");
        }
    }

    heap = heap_get_state();
    console_write("Seneri OS: heap committed ");
    console_write_u64(heap.committed_bytes);
    console_write(" bytes in ");
    console_write_u64(heap.mapped_pages);
    console_write(" pages, live ");
    console_write_u64(heap.live_allocations);
    console_putc('\n');

    if (heap.live_allocations != 3U || heap.committed_bytes == 0U) {
        console_panic("heap did not account for its live allocations");
    }

    /*
     * Free the outer two first and the middle one last, so the final free has a
     * free neighbour on each side and has to merge in both directions. A heap
     * that only ever merges forwards passes every other check and fragments.
     */
    if (heap_free(pointers[0]) != HEAP_STATUS_OK) {
        console_panic("heap refused to release its own allocation");
    }

    /*
     * Released while both its neighbours are still live, so the block is still
     * there to be found and freeing it again is named for what it is.
     */
    if (heap_free(pointers[0]) != HEAP_STATUS_DOUBLE_FREE) {
        console_panic("heap failed to reject a double free");
    }

    if (heap_free(pointers[2]) != HEAP_STATUS_OK ||
        heap_free(pointers[1]) != HEAP_STATUS_OK) {
        console_panic("heap refused to release its own allocation");
    }

    heap = heap_get_state();

    if (heap.live_allocations != 0U || heap.allocated_bytes != 0U) {
        console_panic("heap lifecycle leaked an allocation");
    }

    /*
     * Everything is free again, so complete coalescing means exactly one block
     * covering the whole committed region. Checked before anything below, so a
     * heap that quietly stopped merging is named for that rather than for
     * whichever later expectation it happens to break first.
     */
    if (heap.block_count != 1U) {
        console_panic("heap did not coalesce back to one free block");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        console_panic(heap_status_string(status));
    }

    /*
     * The same two pointers once everything has merged into one block, and the
     * one place this heap's refusals are not interchangeable.
     *
     * The merged block starts at offset zero, which is where the first
     * allocation started, so that pointer still names a block and freeing it
     * again is still a double free. The middle allocation's start was swallowed
     * by the merge and now names nothing, so it is refused as a pointer the
     * heap never returned. Both refuse and neither corrupts anything, but the
     * name depends on what the neighbours did; docs/KERNEL_HEAP.md says so
     * rather than leaving a caller to discover it.
     */
    if (heap_free(pointers[0]) != HEAP_STATUS_DOUBLE_FREE ||
        heap_free(pointers[1]) != HEAP_STATUS_BAD_POINTER) {
        console_panic("heap accepted a pointer it had already merged away");
    }

    console_write("Seneri OS: kernel heap online\n");
    console_write("Seneri OS: heap coalesced to one free block\n");
}

/*
 * Count what is actually on the machine.
 *
 * Every driver Seneri will ever have begins here, because nothing above this
 * layer can name a device that nothing below it has found. The enumeration is
 * read-only on purpose: sizing a base address register means writing all ones
 * into it, and a boot that only counts devices has no business disturbing one.
 *
 * It runs last because it needs everything under it - the heap for its table,
 * the page tables for the uncacheable window, and interrupts disabled so the
 * two halves of a port access cannot be separated.
 */
void bring_up_pci(const struct acpi_mcfg *mcfg, bool present)
{
    struct pci_state pci;
    enum pci_status status;

    if (cpu_interrupts_enabled()) {
        console_panic("PCI enumeration ran with interrupts enabled");
    }

    status = pci_initialize(mcfg, present);

    if (status != PCI_STATUS_OK) {
        console_panic(pci_status_string(status));
    }

    pci = pci_get_state();
    console_write("Seneri OS: PCI mechanism 1 online, ");
    console_write(pci.ecam_active ? "window mapped at " : "no window mapped");

    if (pci.ecam_active) {
        console_write_hex(pci.ecam_base);
        console_write(" buses ");
        console_write_u64(pci.ecam_start_bus);
        console_write(" to ");
        console_write_u64(pci.ecam_end_bus);
    }

    console_putc('\n');
    console_write("Seneri OS: PCI buses ");
    console_write_u64(pci.bus_count);
    console_write(" functions ");
    console_write_u64(pci.function_count);
    console_write(" bridges ");
    console_write_u64(pci.bridge_count);
    console_putc('\n');

    for (size_t index = 0; index < pci.function_count; ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function == NULL) {
            console_panic("PCI reported a function it cannot return");
        }

        console_write("Seneri OS: PCI ");
        console_write_u64(function->address.bus);
        console_putc(':');
        console_write_u64(function->address.device);
        console_putc('.');
        console_write_u64(function->address.function);
        console_write(" vendor ");
        console_write_hex(function->vendor_id);
        console_write(" device ");
        console_write_hex(function->device_id);
        console_write(" class ");
        console_write_hex(function->class_code);
        console_putc('.');
        console_write_hex(function->subclass);
        console_putc(' ');
        console_write(pci_class_string(function->class_code));
        console_write(" caps ");
        console_write_u64(function->capability_count);

        if (function->msi_offset != 0U) {
            console_write(" MSI");
        }

        if (function->msi_x_offset != 0U) {
            console_write(" MSI-X");
        }

        if (function->express_offset != 0U) {
            console_write(" PCIe");
        }

        console_putc('\n');
    }

    /*
     * The host bridge is function zero of device zero of bus zero on every
     * machine that has PCI at all. Requiring it by class rather than by count
     * is what makes this a check on the decoding rather than on the machine
     * happening to have some devices.
     */
    if (pci_find_class(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST_BRIDGE) == NULL) {
        console_panic("PCI enumeration found no host bridge");
    }

    if (pci.function_count == 0U || pci.bus_count == 0U) {
        console_panic("PCI enumeration found no functions");
    }

    console_write("Seneri OS: PCI configuration space enumerated\n");

    /*
     * The claim the second mechanism exists to make. Two readers built
     * independently, reaching the same registers by completely different
     * routes, agreeing register for register. On a machine with no window there
     * is nothing to compare, and saying so is better than reporting a
     * comparison that did not happen.
     */
    console_write("Seneri OS: PCI mechanisms agree on ");
    console_write_u64(pci.compared_dwords);
    console_write(" registers of ");
    console_write_u64(pci.compared_functions);
    console_write(" functions, ");
    console_write_u64(pci.volatile_dwords);
    console_write(" unstable\n");

    if (pci.ecam_active && pci.compared_functions == 0U) {
        console_panic("a mapped configuration window compared nothing");
    }
}

/*
 * Written inside three separate threads, on three separate stacks, and read
 * back on a fourth. Volatile because the only thing that orders these writes is
 * the switch between the threads making them, which the compiler cannot see.
 */
static volatile uint8_t thread_rotation[THREAD_PROOF_LOG];
static volatile size_t thread_rotation_length;

static void proof_worker(void *context)
{
    const uint64_t label = (uint64_t)(uintptr_t)context;

    for (unsigned int round = 0; round < THREAD_PROOF_ROUNDS; ++round) {
        if (thread_rotation_length < THREAD_PROOF_LOG) {
            thread_rotation[thread_rotation_length] = (uint8_t)label;
            thread_rotation_length += 1U;
        }

        thread_yield();
    }

    /*
     * Returning is how a thread ends. The trampoline turns it into
     * thread_exit, so there is deliberately nothing here to say so.
     */
}

/*
 * Turn one thread of control into several.
 *
 * Everything below this layer runs on the single stack boot.S set up, so every
 * wait blocks the machine. This is the increment that makes a wait belong to
 * something: three threads created from the heap and the page tables, each on
 * its own guarded stack, rotating through a shared log in an order the
 * scheduler fixes rather than one the hardware happens to produce.
 *
 * The order is asserted exactly. A scheduler that runs everything eventually
 * but not in the order it claims is a scheduler whose behaviour no caller can
 * reason about, and "all three threads ran" would not have caught it.
 */
void prove_threads(void)
{
    struct frame_allocator_stats before;
    struct frame_allocator_stats after;
    struct thread_system_state threads;
    struct thread_state_report report;
    uint64_t identifiers[THREAD_PROOF_THREADS];
    enum thread_status status;

    before = frame_allocator_get_stats();
    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    if (thread_start() != THREAD_STATUS_ALREADY_STARTED) {
        console_panic("threads accepted a second start");
    }

    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        status = thread_create(
            proof_worker,
            (void *)(uintptr_t)(index + 1U),
            &identifiers[index]
        );

        if (status != THREAD_STATUS_OK) {
            console_panic(thread_status_string(status));
        }

        if (identifiers[index] == THREAD_ID_NONE) {
            console_panic("a created thread has no identifier");
        }
    }

    threads = thread_get_state();
    console_write("Seneri OS: threads online, ");
    console_write_u64(threads.ready);
    console_write(" ready of ");
    console_write_u64(threads.capacity);
    console_write(" on ");
    console_write_u64(threads.stack_frames);
    console_write(" stack frames\n");

    /*
     * Every created thread must sit in its own stack, above its own unmapped
     * guard page. Checked before anything runs, because a stack that overlaps
     * another produces a corruption rather than a fault.
     */
    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        if (thread_report(identifiers[index], &report) != THREAD_STATUS_OK) {
            console_panic("a created thread cannot be reported");
        }

        if (report.stack_base != report.guard_page + PAGING_PAGE_SIZE ||
            report.stack_top != report.stack_base + THREAD_STACK_SIZE ||
            report.state != THREAD_STATE_READY || report.boot_thread) {
            console_panic("a created thread has a malformed stack");
        }

        for (size_t other = 0; other < index; ++other) {
            struct thread_state_report earlier;

            if (thread_report(identifiers[other], &earlier) !=
                THREAD_STATUS_OK) {
                console_panic("a created thread cannot be reported");
            }

            if (report.stack_base < earlier.stack_top &&
                earlier.stack_base < report.stack_top) {
                console_panic("two thread stacks overlap");
            }
        }
    }

    if (thread_verify() != THREAD_STATUS_OK) {
        console_panic("thread table does not match the address space");
    }

    /*
     * Waiting is yielding, so this is also what drives the rotation: the boot
     * thread hands the processor on every time round the loop.
     */
    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        status = thread_join(identifiers[index]);

        if (status != THREAD_STATUS_OK) {
            console_panic(thread_status_string(status));
        }
    }

    console_write("Seneri OS: thread rotation ");

    for (size_t index = 0; index < thread_rotation_length; ++index) {
        console_write_u64(thread_rotation[index]);
    }

    console_putc('\n');

    if (thread_rotation_length != THREAD_PROOF_LOG) {
        console_panic("threads did not all run to completion");
    }

    /*
     * The scheduler picks the next ready thread after the current one and
     * wraps, so three threads created in order must run in that order, every
     * round, without exception. Anything else is a different scheduler.
     */
    for (size_t index = 0; index < THREAD_PROOF_LOG; ++index) {
        if (thread_rotation[index] !=
            (uint8_t)(index % THREAD_PROOF_THREADS + 1U)) {
            console_panic("threads did not rotate in order");
        }
    }

    threads = thread_get_state();

    if (threads.exited != THREAD_PROOF_THREADS || threads.ready != 0U) {
        console_panic("threads did not all exit");
    }

    if (threads.current != thread_current() || !thread_is_started()) {
        console_panic("the boot thread did not resume");
    }

    console_write("Seneri OS: threads switched ");
    console_write_u64(threads.switches);
    console_write(" times, ");
    console_write_u64(threads.exited);
    console_write(" exited\n");

    if (thread_verify() != THREAD_STATUS_OK) {
        console_panic("thread table does not match the address space");
    }

    status = thread_stop();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    if (thread_is_started() || thread_current() != THREAD_ID_NONE) {
        console_panic("threads did not stop");
    }

    /*
     * Every stack frame and every interior page table those mappings needed has
     * to come home, exactly as prove_paging_lifecycle requires. A scheduler that
     * leaks a stack per thread is invisible in one boot and fatal in a long one.
     */
    after = frame_allocator_get_stats();

    if (after.free_frames != before.free_frames) {
        console_panic("starting and stopping threads did not return every frame");
    }

    console_write("Seneri OS: kernel threads established\n");
}

/*
 * Put the picture on the screen, and prove every pixel of it.
 *
 * CONTRIBUTING.md says screenshots are not proof, and this is the first layer
 * where that is a temptation rather than a slogan: a framebuffer looks right
 * long before it is right. So the colour of each pixel is a function of its
 * coordinates, and every one is read back. A pitch mistaken for a width shears
 * the image, two coordinates that alias give one of them the wrong colour, and
 * a mapping that stopped short faults - none of which a picture would report.
 */
static uint32_t proof_colour(uint32_t x, uint32_t y)
{
    return framebuffer_pack(
        (uint8_t)x,
        (uint8_t)y,
        (uint8_t)(x ^ y)
    );
}

void prove_framebuffer(const struct boot_framebuffer *framebuffer)
{
    struct framebuffer_state screen;
    const uint32_t mask = framebuffer_visible_mask();
    uint64_t checked = 0U;
    enum framebuffer_status status = framebuffer_initialize(framebuffer);

    /*
     * A loader that set no graphics mode is not a failure. Seneri has run on
     * the serial console since day one and continues to; this says so and moves
     * on, the same shape as a machine that declares no MCFG.
     */
    if (status == FRAMEBUFFER_STATUS_ABSENT) {
        console_write("Seneri OS: no framebuffer, serial console only\n");
        return;
    }

    if (status != FRAMEBUFFER_STATUS_OK) {
        console_panic(framebuffer_status_string(status));
    }

    screen = framebuffer_get_state();
    console_write("Seneri OS: framebuffer ");
    console_write_u64(screen.width);
    console_putc('x');
    console_write_u64(screen.height);
    console_write(" at ");
    console_write_hex(screen.address);
    console_write(" pitch ");
    console_write_u64(screen.pitch);
    console_write(" RGB ");
    console_write_u64(screen.red_position);
    console_putc('/');
    console_write_u64(screen.green_position);
    console_putc('/');
    console_write_u64(screen.blue_position);
    console_putc('\n');

    if (framebuffer_initialize(framebuffer) !=
        FRAMEBUFFER_STATUS_ALREADY_INITIALIZED) {
        console_panic("the framebuffer accepted a second initialization");
    }

    /*
     * Clear first, so that a pattern which fails to reach some pixel is caught
     * as the clear colour rather than as whatever the loader happened to leave.
     */
    if (framebuffer_fill(framebuffer_pack(0U, 0U, 0U)) !=
        FRAMEBUFFER_STATUS_OK) {
        console_panic("the framebuffer would not clear");
    }

    for (uint32_t y = 0; y < screen.height; ++y) {
        for (uint32_t x = 0; x < screen.width; ++x) {
            if (framebuffer_write_pixel(x, y, proof_colour(x, y)) !=
                FRAMEBUFFER_STATUS_OK) {
                console_panic("the framebuffer refused a visible pixel");
            }
        }
    }

    /*
     * Read every one back. Only the bits the loader called channels are
     * compared: the fourth byte of a 32-bit pixel is not a channel it
     * described, so nothing is claimed about what the hardware keeps there.
     */
    for (uint32_t y = 0; y < screen.height; ++y) {
        for (uint32_t x = 0; x < screen.width; ++x) {
            uint32_t pixel = 0U;

            if (framebuffer_read_pixel(x, y, &pixel) !=
                FRAMEBUFFER_STATUS_OK) {
                console_panic("the framebuffer refused a visible pixel");
            }

            if ((pixel & mask) != (proof_colour(x, y) & mask)) {
                console_panic("a framebuffer pixel did not hold its colour");
            }

            ++checked;
        }
    }

    console_write("Seneri OS: framebuffer verified ");
    console_write_u64(checked);
    console_write(" pixels\n");

    if (checked != (uint64_t)screen.width * screen.height) {
        console_panic("the framebuffer proof skipped part of the picture");
    }

    /*
     * One coordinate past each edge must be refused rather than written. This
     * is the check between a bounded framebuffer and a stray store into the
     * page after the picture.
     */
    if (framebuffer_write_pixel(screen.width, 0U, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS ||
        framebuffer_write_pixel(0U, screen.height, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS) {
        console_panic("the framebuffer accepted a pixel off the screen");
    }

    if (framebuffer_verify() != FRAMEBUFFER_STATUS_OK) {
        console_panic("the framebuffer is not device memory");
    }

    console_write("Seneri OS: framebuffer established\n");
}

/*
 * Draw the logo, and prove it arrived.
 *
 * The decoder is Rust; docs/RUST.md argues why that one file is. From here it
 * is an ordinary subsystem with ordinary refusals, and the proof is the same
 * shape as every other: the image is decoded into memory, blitted, and then
 * every pixel of the blitted region is read back off the screen and compared
 * against what the decoder produced. A logo that looks right is not evidence;
 * a logo whose 65,536 pixels each match the decode is.
 */
void draw_logo(void)
{
    struct framebuffer_state screen = framebuffer_get_state();
    const uint32_t background = framebuffer_pack(
        BOOT_BACKGROUND_RED,
        BOOT_BACKGROUND_GREEN,
        BOOT_BACKGROUND_BLUE
    );
    const uint32_t mask = framebuffer_visible_mask();
    uint32_t *decoded = NULL;
    void *allocation = NULL;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t origin_x;
    uint32_t origin_y;
    uint64_t compared = 0U;
    int32_t status = seneri_logo_geometry(&width, &height);

    if (status != LOGO_STATUS_OK) {
        console_panic(logo_status_string(status));
    }

    console_write("Seneri OS: logo ");
    console_write_u64(width);
    console_putc('x');
    console_write_u64(height);
    console_write(" from ");
    console_write_u64(seneri_logo_size());
    console_write(" bytes, decoded by Rust\n");

    if (width > screen.width || height > screen.height) {
        console_panic("the logo does not fit the screen");
    }

    /*
     * One heap allocation for the decoded image, released before this returns.
     * The decoder is handed a buffer of exactly the pixels it declared, so a
     * decode that filled less than the whole image cannot be mistaken for one
     * that filled all of it.
     */
    if (heap_allocate((uint64_t)width * height * sizeof(uint32_t),
            &allocation) != HEAP_STATUS_OK) {
        console_panic("no memory for the decoded logo");
    }

    decoded = (uint32_t *)allocation;

    /*
     * A buffer one pixel short must be refused. Checked here rather than only
     * in the decoder's own tests, because this is the call site whose length
     * argument would be wrong if anything upstream of it were.
     */
    if (seneri_logo_decode(decoded, (size_t)((uint64_t)width * height - 1U),
            screen.red_position, screen.green_position, screen.blue_position,
            background) != LOGO_STATUS_BUFFER_TOO_SMALL) {
        console_panic("the logo decoder accepted a short buffer");
    }

    if (seneri_logo_decode(NULL, (size_t)((uint64_t)width * height),
            screen.red_position, screen.green_position, screen.blue_position,
            background) != LOGO_STATUS_NULL_ARGUMENT) {
        console_panic("the logo decoder accepted a null buffer");
    }

    status = seneri_logo_decode(decoded, (size_t)((uint64_t)width * height),
        screen.red_position, screen.green_position, screen.blue_position,
        background);

    if (status != LOGO_STATUS_OK) {
        console_panic(logo_status_string(status));
    }

    if (framebuffer_fill(background) != FRAMEBUFFER_STATUS_OK) {
        console_panic("the framebuffer would not clear");
    }

    origin_x = (screen.width - width) / 2U;
    origin_y = (screen.height - height) / 2U;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            if (framebuffer_write_pixel(origin_x + x, origin_y + y,
                    decoded[(uint64_t)y * width + x]) !=
                FRAMEBUFFER_STATUS_OK) {
                console_panic("the logo did not fit where it was drawn");
            }
        }
    }

    /*
     * Read the whole logo back off the screen. This is what makes the claim
     * "the logo is on the screen" mean something a serial line can carry.
     */
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t pixel = 0U;

            if (framebuffer_read_pixel(origin_x + x, origin_y + y, &pixel) !=
                FRAMEBUFFER_STATUS_OK) {
                console_panic("the logo did not fit where it was drawn");
            }

            if ((pixel & mask) !=
                (decoded[(uint64_t)y * width + x] & mask)) {
                console_panic("a logo pixel did not reach the screen");
            }

            ++compared;
        }
    }

    /*
     * The pixel just outside the logo must still be the background. A blit
     * that ran one row or column long would otherwise be invisible.
     */
    if (origin_x > 0U) {
        uint32_t edge = 0U;

        if (framebuffer_read_pixel(origin_x - 1U, origin_y, &edge) !=
                FRAMEBUFFER_STATUS_OK ||
            (edge & mask) != (background & mask)) {
            console_panic("the logo was drawn outside its own area");
        }
    }

    if (heap_free(allocation) != HEAP_STATUS_OK) {
        console_panic("the decoded logo could not be released");
    }

    console_write("Seneri OS: logo verified ");
    console_write_u64(compared);
    console_write(" pixels on screen\n");

    if (compared != (uint64_t)width * height) {
        console_panic("the logo proof skipped part of the image");
    }

    console_write("Seneri OS: logo established\n");
}

/*
 * Written by threads that never call into the scheduler, read by a thread that
 * is itself being preempted. Volatile because nothing in the C abstract machine
 * orders these: the only thing that does is a timer interrupt.
 */
static volatile bool preempt_stop;
static volatile uint64_t preempt_work[THREAD_PROOF_THREADS];

static void preempt_worker(void *context)
{
    const size_t index = (size_t)(uintptr_t)context;

    /*
     * The entire point: this loop contains no thread_yield, no sleep, and no
     * call that could reach the scheduler. If it is ever descheduled, something
     * took the processor away from it.
     */
    while (!preempt_stop) {
        preempt_work[index] += 1U;
    }
}

/*
 * Take the processor back from a thread that never offers it.
 *
 * Cooperative scheduling was proved by threads that yield; that proves the
 * switch works, not that the scheduler is in charge. This proves the harder
 * claim, and it is the one every driver and every sleep above this layer
 * depends on: a thread that does nothing but spin is still descheduled, on a
 * schedule the kernel sets rather than one the thread agrees to.
 */
void prove_preemption(void)
{
    struct thread_state_report report;
    struct thread_system_state threads;
    uint64_t identifiers[THREAD_PROOF_THREADS];
    uint64_t started_ns;
    size_t satisfied = 0U;
    enum thread_status status;

    preempt_stop = false;

    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        preempt_work[index] = 0U;
    }

    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    /*
     * Preemption is a deadline, so the deadline layer must be running. Asked
     * for before it is, so the refusal is proved rather than assumed.
     */
    if (!timer_is_started()) {
        console_panic("the deadline timer stopped before preemption");
    }

    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        status = thread_create(
            preempt_worker,
            (void *)(uintptr_t)index,
            &identifiers[index]
        );

        if (status != THREAD_STATUS_OK) {
            console_panic(thread_status_string(status));
        }
    }

    status = thread_enable_preemption();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    if (thread_enable_preemption() != THREAD_STATUS_ALREADY_PREEMPTIVE) {
        console_panic("preemption was enabled twice");
    }

    started_ns = clock_monotonic_ns();

    /*
     * Nothing below here yields. The boot thread spins on the same terms as
     * the workers, and only makes progress because it is preempted too.
     */
    cpu_interrupt_enable();

    while (satisfied < THREAD_PROOF_THREADS) {
        satisfied = 0U;

        for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
            if (thread_report(identifiers[index], &report) ==
                    THREAD_STATUS_OK &&
                report.switches >= PREEMPT_PROOF_TURNS) {
                ++satisfied;
            }
        }

        if (clock_monotonic_ns() - started_ns > PREEMPT_PROOF_LIMIT_NS) {
            cpu_interrupt_disable();
            console_panic("threads were not preempted within the time limit");
        }
    }

    preempt_stop = true;

    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        status = thread_join(identifiers[index]);

        if (status != THREAD_STATUS_OK) {
            cpu_interrupt_disable();
            console_panic(thread_status_string(status));
        }
    }

    cpu_interrupt_disable();
    threads = thread_get_state();

    console_write("Seneri OS: preempted ");
    console_write_u64(threads.preemptions);
    console_write(" times across ");
    console_write_u64(threads.switches);
    console_write(" switches in ");
    console_write_u64((clock_monotonic_ns() - started_ns) / 1000000U);
    console_write(" ms\n");

    console_write("Seneri OS: unyielding threads ran");

    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        console_putc(' ');
        console_write_u64(preempt_work[index]);
    }

    console_putc('\n');

    /*
     * Every worker must have done work and been scheduled repeatedly. A
     * scheduler that started all three and then let one monopolise the
     * processor would satisfy neither.
     */
    for (size_t index = 0; index < THREAD_PROOF_THREADS; ++index) {
        if (preempt_work[index] == 0U) {
            console_panic("a thread was never given the processor");
        }

        if (thread_report(identifiers[index], &report) != THREAD_STATUS_OK ||
            report.switches < PREEMPT_PROOF_TURNS) {
            console_panic("a thread was not preempted back into");
        }
    }

    /*
     * The claim this proof exists for. Every switch here was involuntary:
     * nothing in preempt_worker can reach the scheduler, and the boot thread
     * only called thread_join after the spinning was over.
     */
    if (threads.preemptions < PREEMPT_PROOF_TURNS) {
        console_panic("the scheduler never took the processor back");
    }

    status = thread_disable_preemption();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    if (thread_preemption_enabled() ||
        thread_disable_preemption() != THREAD_STATUS_NOT_PREEMPTIVE) {
        console_panic("preemption did not stop");
    }

    status = thread_stop();

    if (status != THREAD_STATUS_OK) {
        console_panic(thread_status_string(status));
    }

    console_write("Seneri OS: preemption established\n");
}

void prove_frame_lifecycle(void)
{
    uintptr_t first_frame;
    uintptr_t second_frame;
    enum frame_status status;

    status = frame_allocate(&first_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    status = frame_allocate(&second_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    if (first_frame == second_frame ||
        (first_frame & (SENERI_PAGE_SIZE - 1U)) != 0U ||
        (second_frame & (SENERI_PAGE_SIZE - 1U)) != 0U) {
        console_panic("frame allocator returned an invalid address");
    }

    console_write("Seneri OS: frame probe: ");
    console_write_hex(first_frame);
    console_write(" and ");
    console_write_hex(second_frame);
    console_putc('\n');

    status = frame_release(second_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    status = frame_release(first_frame);

    if (status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(status));
    }

    if (frame_release(first_frame) != FRAME_STATUS_DOUBLE_FREE) {
        console_panic("frame allocator failed to reject a double free");
    }

    if (frame_release(1U) != FRAME_STATUS_UNALIGNED_ADDRESS) {
        console_panic("frame allocator failed to reject an unaligned release");
    }

    if (frame_release(0U) != FRAME_STATUS_FRAME_NOT_ALLOCATABLE) {
        console_panic("frame allocator released permanently reserved memory");
    }
}

/*
 * Text on the framebuffer, and proof that it is really there.
 *
 * Everything else this console does is a write into device memory, and a write
 * nothing reads back proves nothing: the pattern could be off by a row, the
 * glyph could be mirrored, the foreground and background could be swapped, and
 * every one of those draws something. So the proof reads the pixels back and
 * compares them against what the font says they should be, cell by cell.
 *
 * It runs after the logo on purpose. The logo is a splash and this replaces it,
 * so the last thing on the screen at the end of boot is the boot log - which is
 * the thing somebody standing in front of the machine actually needs.
 */
void prove_screen_console(void)
{
    static const char sample[] = "Seneri OS";
    static const size_t sample_length = sizeof(sample) - 1U;

    struct screen_state before;
    struct screen_state after;
    enum screen_status status;

    status = screen_initialize();

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    before = screen_get_state();

    console_write("Seneri OS: screen console ");
    console_write_u64(before.columns);
    console_write("x");
    console_write_u64(before.rows);
    console_write(" cells of ");
    console_write_u64(before.cell_width);
    console_write("x");
    console_write_u64(before.cell_height);
    console_write(", font ");
    console_write_u64(seneri_font_size());
    console_write(" bytes\n");

    /*
     * From a known screen, so the cells checked below hold what this proof put
     * in them rather than whatever the transcript had reached.
     */
    status = screen_clear();

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    status = screen_write(sample);

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    for (size_t index = 0; index < sample_length; ++index) {
        if (screen_verify_cell((uint32_t)index, 0U, sample[index]) !=
            SCREEN_STATUS_OK) {
            console_panic("screen console does not match the font it drew from");
        }
    }

    /*
     * A cell the sample never wrote to must be background, not a leftover. This
     * is what catches a draw that is one cell wide when it should be eight, or
     * a clear that did not.
     */
    if (screen_verify_cell((uint32_t)sample_length + 1U, 0U, ' ') !=
        SCREEN_STATUS_OK) {
        console_panic("screen console left something in a cell it never drew");
    }

    /*
     * A byte the font does not cover is substituted rather than refused. A
     * console that stops on an unexpected character eventually swallows the
     * message explaining what went wrong.
     */
    status = screen_putc('\x01');

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    if (screen_verify_cell((uint32_t)sample_length, 0U, '?') !=
        SCREEN_STATUS_OK) {
        console_panic("screen console did not substitute for an uncovered byte");
    }

    /*
     * Filling the last column must wrap to the next row rather than draw off
     * the edge, and the framebuffer's own bounds check must never be the thing
     * that catches it.
     */
    before = screen_get_state();

    while (screen_get_state().column < before.columns) {
        if (screen_putc('#') != SCREEN_STATUS_OK) {
            console_panic("screen console refused to fill a row");
        }
    }

    if (screen_putc('#') != SCREEN_STATUS_OK) {
        console_panic("screen console refused to wrap");
    }

    after = screen_get_state();

    if (after.row != before.row + 1U || after.column != 1U) {
        console_panic("screen console did not wrap at the last column");
    }

    /*
     * And running off the bottom scrolls rather than stopping. The cursor stays
     * on the last row while the picture moves, so the count is what changes.
     */
    before = screen_get_state();

    while (screen_get_state().row + 1U < before.rows) {
        if (screen_putc('\n') != SCREEN_STATUS_OK) {
            console_panic("screen console refused a newline");
        }
    }

    if (screen_putc('\n') != SCREEN_STATUS_OK) {
        console_panic("screen console refused to scroll");
    }

    after = screen_get_state();

    if (after.scrolls != before.scrolls + 1U) {
        console_panic("screen console did not scroll at the last row");
    }

    if (after.row + 1U != before.rows || after.column != 0U) {
        console_panic("screen console moved the cursor off the last row");
    }

    /*
     * A scroll moves every pixel on the screen, so the cheapest complete check
     * that it did not corrupt the mapping is to draw once more and read it
     * back. Anything that broke the pitch arithmetic shows up here.
     */
    status = screen_clear();

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    status = screen_write(sample);

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    for (size_t index = 0; index < sample_length; ++index) {
        if (screen_verify_cell((uint32_t)index, 0U, sample[index]) !=
            SCREEN_STATUS_OK) {
            console_panic("screen console does not survive a scroll");
        }
    }

    status = screen_clear();

    if (status != SCREEN_STATUS_OK) {
        console_panic(screen_status_string(status));
    }

    after = screen_get_state();
    console_write("Seneri OS: screen console drew ");
    console_write_u64(after.characters);
    console_write(" characters and scrolled ");
    console_write_u64(after.scrolls);
    console_write(" times\n");
    console_write("Seneri OS: screen console established\n");
}

/*
 * The keyboard, proved without a person at the machine.
 *
 * Every other device Seneri brings up either announces itself or can be asked a
 * question. A keyboard does neither: it says nothing until somebody presses a
 * key, and boot cannot wait for that.
 *
 * The way through is a real controller command rather than a test hook. 8042
 * command 0xD2 writes a byte into the output buffer exactly as though the
 * keyboard had sent it, which raises IRQ 1. So the whole path is exercised for
 * real - controller, I/O APIC routing, vector, handler, decode, queue - and the
 * only thing simulated is the finger.
 */
void prove_keyboard(void)
{
    /* 'h', 'i', then left shift down, 'i' again, and shift up: "hiI". */
    static const uint8_t script[] = {
        0x23U, 0x17U, 0x2AU, 0x17U, (uint8_t)(0x2AU | 0x80U)
    };
    static const char expected[] = "hiI";

    struct keyboard_state before;
    struct keyboard_state after;
    struct keyboard_event event;
    enum keyboard_status status;
    size_t characters = 0U;
    char seen[8];

    status = keyboard_initialize();

    if (status != KEYBOARD_STATUS_OK) {
        console_panic(keyboard_status_string(status));
    }

    before = keyboard_get_state();

    if (before.queued != 0U) {
        console_panic("the keyboard queue was not empty at bring-up");
    }

    /*
     * Interrupts have to be on for this to prove anything: the whole point is
     * that the bytes arrive through IRQ 1 rather than by being polled.
     */
    cpu_interrupt_enable();

    for (size_t index = 0; index < sizeof(script); ++index) {
        status = keyboard_inject_scancode(script[index]);

        if (status != KEYBOARD_STATUS_OK) {
            cpu_interrupt_disable();
            console_panic(keyboard_status_string(status));
        }
    }

    /*
     * Bounded, like every other wait in this kernel. A keyboard that never
     * delivers must fail the boot rather than hang it.
     */
    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        if (keyboard_get_state().events >= sizeof(script)) {
            break;
        }
    }

    cpu_interrupt_disable();
    after = keyboard_get_state();

    if (after.interrupts == before.interrupts) {
        console_panic("the keyboard delivered no interrupt");
    }

    if (after.events < sizeof(script)) {
        console_panic("the keyboard lost an injected scancode");
    }

    if (after.dropped != 0U) {
        console_panic("the keyboard queue overflowed during its own proof");
    }

    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        if (event.character == '\0') {
            continue;
        }

        if (characters >= sizeof(seen)) {
            console_panic("the keyboard produced more characters than it was sent");
        }

        seen[characters] = event.character;
        characters += 1U;
    }

    if (characters != sizeof(expected) - 1U) {
        console_panic("the keyboard produced the wrong number of characters");
    }

    for (size_t index = 0; index < characters; ++index) {
        if (seen[index] != expected[index]) {
            console_panic("the keyboard decoded a scancode to the wrong character");
        }
    }

    /*
     * The shift released above must have left no residue. A modifier that
     * stayed down would capitalise everything typed afterwards.
     */
    if (after.shift) {
        console_panic("the keyboard left shift held after its release");
    }

    console_write("Seneri OS: keyboard 8042 online, IRQ 1 routed, ");
    console_write_u64(after.interrupts - before.interrupts);
    console_write(" interrupts for ");
    console_write_u64(after.events);
    console_write(" events\n");
    console_write("Seneri OS: keyboard decoded \"");
    for (size_t index = 0; index < characters; ++index) {
        console_putc(seen[index]);
    }
    console_write("\" from injected scancodes\n");
    console_write("Seneri OS: keyboard established\n");
}

/*
 * The shell, proved end to end.
 *
 * Every layer under this one has been proved in isolation. This is the first
 * proof that runs the whole chain at once, and it is worth doing that way
 * because the chain is the product: a keystroke is worthless if it decodes
 * correctly and never reaches a command, and a command is worthless if it runs
 * and nothing appears.
 *
 * So the scancodes for "echo hi" are pushed through the 8042's own 0xD2
 * command, taken by IRQ 1, decoded by the keyboard driver, drained out of its
 * queue, fed to the shell one character at a time exactly as shell_run does,
 * executed, written to the console, drawn by the screen console - and then read
 * back out of the framebuffer, pixel by pixel, and compared against the font.
 *
 * Nothing in that sentence is simulated except the finger.
 */
void prove_shell(void)
{
    /* e c h o <space> h i <enter>, in scancode set 1. */
    static const uint8_t typed[] = {
        0x12U, 0x2EU, 0x23U, 0x18U, 0x39U, 0x23U, 0x17U, 0x1CU
    };
    static const char echoed[] = "echo hi";
    static const char output[] = "hi";
    static const char prompt[] = "seneri> ";

    struct shell_state before;
    struct shell_state after;
    struct keyboard_event event;
    enum shell_status status;
    size_t fed = 0U;

    status = shell_initialize();

    if (status != SHELL_STATUS_OK) {
        console_panic(shell_status_string(status));
    }

    if (!screen_is_active()) {
        console_panic("the shell proof needs a screen to read back");
    }

    if (screen_clear() != SCREEN_STATUS_OK) {
        console_panic("the shell proof could not clear the screen");
    }

    before = shell_get_state();

    cpu_interrupt_enable();

    for (size_t index = 0; index < sizeof(typed); ++index) {
        if (keyboard_inject_scancode(typed[index]) != KEYBOARD_STATUS_OK) {
            cpu_interrupt_disable();
            console_panic("the shell proof could not type");
        }

        /*
         * Drained after every key rather than at the end. The controller holds
         * one byte, so a key that is not taken before the next is injected is a
         * key that never happened - and this is exactly the loop shell_run
         * runs, so proving it here proves that.
         */
        for (uint64_t spins = 0; spins < UINT64_C(20000000); ++spins) {
            if (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
                if (event.pressed && event.character != '\0') {
                    (void)shell_feed(event.character);
                    fed += 1U;
                }

                break;
            }
        }
    }

    cpu_interrupt_disable();
    after = shell_get_state();

    if (fed != sizeof(typed)) {
        console_panic("the shell did not receive every key that was typed");
    }

    if (after.lines != before.lines + 1U) {
        console_panic("the shell did not run the line it was given");
    }

    if (after.unknown != before.unknown) {
        console_panic("the shell did not recognise a command it has");
    }

    if (after.length != 0U) {
        console_panic("the shell did not clear its line after running it");
    }

    /*
     * And now the part that makes this a proof rather than a count. The screen
     * was cleared before anything was typed, so every cell below is at a known
     * place, and each is compared against what the font says that character
     * looks like.
     */
    for (size_t index = 0; index < sizeof(echoed) - 1U; ++index) {
        if (screen_verify_cell((uint32_t)index, 0U, echoed[index]) !=
            SCREEN_STATUS_OK) {
            console_panic("the shell did not echo what was typed");
        }
    }

    for (size_t index = 0; index < sizeof(output) - 1U; ++index) {
        if (screen_verify_cell((uint32_t)index, 1U, output[index]) !=
            SCREEN_STATUS_OK) {
            console_panic("the command produced no output on the screen");
        }
    }

    for (size_t index = 0; index < sizeof(prompt) - 1U; ++index) {
        if (screen_verify_cell((uint32_t)index, 2U, prompt[index]) !=
            SCREEN_STATUS_OK) {
            console_panic("the shell did not offer a prompt afterwards");
        }
    }

    if (screen_clear() != SCREEN_STATUS_OK) {
        console_panic("the shell proof could not clear the screen afterwards");
    }

    /*
     * The prompt above has no newline after it, because a prompt waits. Boot is
     * about to carry on talking, so it gets one here rather than leaving the
     * next transcript line beginning halfway across the screen.
     */
    console_putc('\n');
    console_write("Seneri OS: shell ran \"");
    console_write(echoed);
    console_write("\" from ");
    console_write_u64(sizeof(typed));
    console_write(" injected scancodes\n");
    console_write("Seneri OS: shell output verified on screen\n");
    console_write("Seneri OS: shell established\n");
}
