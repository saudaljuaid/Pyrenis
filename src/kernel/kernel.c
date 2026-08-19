/* SPDX-License-Identifier: GPL-3.0-only */
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
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/logo.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
#include <seneri/pci.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>
#include <seneri/self_test.h>
#include <seneri/test.h>
#include <seneri/thread.h>
#include <seneri/timer.h>
#include <seneri/tsc.h>

#define MAX_REPORTED_BOOT_LOADER_NAME 64U

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
 * What the screen is cleared to before the logo is drawn: a near-black with a
 * slight blue bias, so the logo's own edges have something to sit against
 * rather than a pure black that hides how they were composited.
 */
#define BOOT_BACKGROUND_RED UINT8_C(0x08)
#define BOOT_BACKGROUND_GREEN UINT8_C(0x0A)
#define BOOT_BACKGROUND_BLUE UINT8_C(0x0E)

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

/*
 * The discovered interrupt topology outlives kernel_main's frame and is larger
 * than early boot should place on the 16 KiB kernel stack.
 */
static struct acpi_topology boot_topology;

/*
 * The configuration window description, kept for the same reason: it outlives
 * kernel_main's frame and src/kernel/pci.c reads it after boot.
 */
static struct acpi_mcfg boot_mcfg;
static bool boot_mcfg_present;

static void report_boot_context(const struct boot_context *context)
{
    console_write("Seneri OS: boot loader: ");

    if (context->boot_loader_name == NULL) {
        console_write("unnamed");
    } else {
        size_t reported_length = context->boot_loader_name_length;

        if (reported_length > MAX_REPORTED_BOOT_LOADER_NAME) {
            reported_length = MAX_REPORTED_BOOT_LOADER_NAME;
        }

        console_write_n(context->boot_loader_name, reported_length);

        if (reported_length != context->boot_loader_name_length) {
            console_write("...");
        }
    }

    console_putc('\n');

    console_write("Seneri OS: memory map entries: ");
    console_write_u64(context->memory_map_entry_count);
    console_putc('\n');

    console_write("Seneri OS: reported usable bytes: ");
    console_write_u64(context->reported_usable_bytes);
    console_putc('\n');

    console_write("Seneri OS: highest reported address: ");
    console_write_hex(context->highest_reported_address);
    console_putc('\n');
}

static void report_allocator(const struct frame_allocator_stats *stats)
{
    console_write("Seneri OS: allocatable frames: ");
    console_write_u64(stats->allocatable_frames);
    console_putc('\n');

    console_write("Seneri OS: free frames: ");
    console_write_u64(stats->free_frames);
    console_putc('\n');

    console_write("Seneri OS: reserved frames: ");
    console_write_u64(stats->reserved_frames);
    console_putc('\n');

    console_write("Seneri OS: highest allocatable address: ");
    console_write_hex(stats->highest_allocatable_address);
    console_putc('\n');
}

static void report_acpi_root(const struct acpi_root *root)
{
    console_write("Seneri OS: ACPI ");
    console_write(acpi_root_kind_string(root->kind));
    console_write(" at ");
    console_write_hex(root->physical_address);
    console_write(" OEM ");
    console_write_n(root->oem_id, 6U);
    console_putc('\n');
}

static void report_acpi_madt(const struct acpi_madt *madt)
{
    console_write("Seneri OS: ACPI MADT at ");
    console_write_hex(madt->physical_address);
    console_write(" local APIC ");
    console_write_hex(madt->local_apic_address);
    console_write(" flags ");
    console_write_hex(madt->flags);
    console_putc('\n');

    console_write("Seneri OS: ACPI root entries: ");
    console_write_u64(madt->root_entry_count);
    console_write(" MADT OEM ");
    console_write_n(madt->oem_id, 6U);
    console_putc(' ');
    console_write_n(madt->oem_table_id, 8U);
    console_putc('\n');
}

static void report_acpi_fadt(const struct acpi_fadt *fadt)
{
    console_write("Seneri OS: ACPI FADT at ");
    console_write_hex(fadt->physical_address);
    console_write(" revision ");
    console_write_u64(fadt->revision);
    console_write(" flags ");
    console_write_hex(fadt->flags);
    console_putc('\n');
}

static void report_pm_timer(const struct pm_timer_state *pm_timer)
{
    console_write("Seneri OS: ACPI PM timer port ");
    console_write_hex(pm_timer->port);
    console_write(" width ");
    console_write_u64(pm_timer->counter_bits);
    console_write(" bits address ");
    console_write(pm_timer->extended_address ? "extended" : "fixed");
    console_putc('\n');
}

/*
 * The one firmware table Seneri reads whose absence is not a fault. A machine
 * with no PCI Express host bridge publishes no MCFG, and configuration space is
 * still reachable through the I/O ports the PCI specification has always
 * defined, so absence is reported and boot continues.
 */
static void report_acpi_mcfg(const struct acpi_mcfg *mcfg, bool present)
{
    if (!present) {
        console_write("Seneri OS: ACPI MCFG absent\n");
        return;
    }

    console_write("Seneri OS: ACPI MCFG at ");
    console_write_hex(mcfg->physical_address);
    console_write(" windows ");
    console_write_u64(mcfg->allocation_count);
    console_putc('\n');

    for (size_t index = 0; index < mcfg->allocation_count; ++index) {
        const struct acpi_ecam_allocation *allocation =
            &mcfg->allocations[index];

        console_write("Seneri OS: ACPI ECAM segment ");
        console_write_u64(allocation->segment);
        console_write(" base ");
        console_write_hex(allocation->base_address);
        console_write(" buses ");
        console_write_u64(allocation->start_bus);
        console_write(" to ");
        console_write_u64(allocation->end_bus);
        console_write(" size ");
        console_write_u64(acpi_ecam_allocation_size(allocation));
        console_putc('\n');
    }
}

static void report_acpi_topology(const struct acpi_topology *topology)
{
    console_write("Seneri OS: ACPI local APIC base ");
    console_write_hex(topology->local_apic_address);

    if (topology->local_apic_address_overridden) {
        console_write(" overridden");
    }

    console_putc('\n');

    console_write("Seneri OS: ACPI processors: ");
    console_write_u64(topology->local_apic_count);
    console_write(" enabled ");
    console_write_u64(topology->enabled_processor_count);
    console_write(" NMI entries ");
    console_write_u64(topology->nmi_entry_count);
    console_write(" unmodelled ");
    console_write_u64(topology->ignored_entry_count);
    console_putc('\n');

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const struct acpi_io_apic *io_apic = &topology->io_apics[index];

        console_write("Seneri OS: ACPI I/O APIC id ");
        console_write_u64(io_apic->identifier);
        console_write(" at ");
        console_write_hex(io_apic->address);
        console_write(" base GSI ");
        console_write_u64(io_apic->interrupt_base);
        console_putc('\n');
    }

    for (size_t index = 0; index < topology->interrupt_override_count; ++index) {
        const struct acpi_interrupt_override *override =
            &topology->interrupt_overrides[index];

        console_write("Seneri OS: ACPI override ISA IRQ ");
        console_write_u64(override->source);
        console_write(" to GSI ");
        console_write_u64(override->global_system_interrupt);
        console_write(" flags ");
        console_write_hex(override->flags);
        console_putc('\n');
    }
}

static void report_apic(const struct apic_state *apic)
{
    console_write("Seneri OS: local APIC id ");
    console_write_u64(apic->id);
    console_write(" version ");
    console_write_hex(apic->version);
    console_write(" LVT entries ");
    console_write_u64((uint64_t)apic->max_lvt_entry + 1U);
    console_write(" at ");
    console_write_hex(apic->base_address);
    console_putc('\n');

    console_write("Seneri OS: local APIC legacy routing ");
    console_write(apic->legacy_interrupts_routed ? "LINT0 ExtINT" : "masked");
    console_putc('\n');
}

static void report_ioapic(const struct ioapic_state *ioapic)
{
    for (size_t index = 0; index < ioapic->count; ++index) {
        const struct ioapic_unit *unit = &ioapic->units[index];

        console_write("Seneri OS: I/O APIC id ");
        console_write_u64(unit->identifier);
        console_write(" version ");
        console_write_hex(unit->version);
        console_write(" entries ");
        console_write_u64(unit->entry_count);
        console_write(" base GSI ");
        console_write_u64(unit->interrupt_base);
        console_putc('\n');
    }
}

/*
 * The same timer, counted over both delivery paths. Proving the legacy path
 * still works after the I/O APIC path is programmed is what keeps this
 * increment reversible.
 */
static void prove_timer_route(enum pit_route route)
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
static void retire_legacy_interrupt_path(void)
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
static void prove_apic_timer(void)
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
static void prove_tsc(void)
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
static void prove_pm_timer(void)
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
static void retire_pit(void)
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
static void prove_clocks_without_pit(void)
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
static void prove_monotonic_time(void)
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
static void install_page_tables(
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
static void prove_paging_lifecycle(void)
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
static void bring_up_heap(void)
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
static void prove_heap_lifecycle(void)
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
static void bring_up_pci(void)
{
    struct pci_state pci;
    enum pci_status status;

    if (cpu_interrupts_enabled()) {
        console_panic("PCI enumeration ran with interrupts enabled");
    }

    status = pci_initialize(&boot_mcfg, boot_mcfg_present);

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
static void prove_threads(void)
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

static void prove_framebuffer(const struct boot_framebuffer *framebuffer)
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
static void draw_logo(void)
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

static void prove_frame_lifecycle(void)
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

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information)
{
    struct acpi_fadt acpi_fadt;
    struct acpi_madt acpi_madt;
    struct acpi_root acpi_root;
    struct apic_state apic_state;
    struct boot_context context;
    struct frame_allocator_stats stats;
    struct ioapic_state ioapic_state;
    struct pm_timer_state pm_timer_state;
    enum boot_status boot_status;
    enum acpi_status acpi_status;
    enum apic_status apic_status;
    enum ioapic_status ioapic_status;
    enum frame_status frame_status;
    enum interrupt_status interrupt_status;
    enum kernel_test_scenario test_scenario;
    enum heap_status heap_status;
    enum paging_status paging_status;
    enum pci_status pci_status;
    enum pm_timer_status pm_timer_status;

    console_initialize();
    interrupt_status = interrupts_initialize();

    if (interrupt_status != INTERRUPT_STATUS_OK) {
        if (interrupt_status == INTERRUPT_STATUS_CPU_TABLE_FAILURE) {
            console_write("Seneri OS: CPU table detail: ");
            console_write(cpu_status_string(cpu_tables_validate()));
            console_putc('\n');
        }

        console_panic(interrupt_status_string(interrupt_status));
    }

    console_write("Seneri OS: kernel online\n");
    console_write("Seneri OS: descriptor tables verified\n");
    console_write("Seneri OS: interrupt foundation online\n");

    if (!boot_parser_self_test()) {
        console_panic("Multiboot2 parser self-test failed");
    }

    if (!acpi_self_test()) {
        console_panic("ACPI RSDP rejection self-test failed");
    }

    if (!acpi_tables_self_test()) {
        console_panic("ACPI table rejection self-test failed");
    }

    if (!acpi_topology_self_test()) {
        console_panic("ACPI topology rejection self-test failed");
    }

    if (!apic_self_test()) {
        console_panic("local APIC rejection self-test failed");
    }

    if (!ioapic_self_test()) {
        console_panic("I/O APIC routing self-test failed");
    }

    if (!apic_timer_self_test()) {
        console_panic("local APIC timer calibration self-test failed");
    }

    if (!tsc_self_test()) {
        console_panic("TSC conversion self-test failed");
    }

    if (!pm_timer_self_test()) {
        console_panic("ACPI PM timer arithmetic self-test failed");
    }

    if (!clock_self_test()) {
        console_panic("monotonic clock self-test failed");
    }

    if (!timer_self_test()) {
        console_panic("deadline timer table self-test failed");
    }

    if (!paging_self_test()) {
        console_panic("page table arithmetic self-test failed");
    }

    if (!heap_self_test()) {
        console_panic("kernel heap block table self-test failed");
    }

    if (!pci_self_test()) {
        console_panic("PCI configuration arithmetic self-test failed");
    }

    if (!thread_self_test()) {
        console_panic("thread table and stack layout self-test failed");
    }

    if (!framebuffer_self_test()) {
        console_panic("framebuffer geometry self-test failed");
    }

    /*
     * The first self-test in this kernel that is not C. It runs beside the
     * others because a caller should not have to care which language answered.
     */
    if (seneri_logo_self_test() != 1) {
        console_panic("logo decoder self-test failed");
    }

    console_write("Seneri OS: parser rejection tests passed\n");

    boot_status = boot_context_parse(magic, boot_information, &context);

    if (boot_status != BOOT_STATUS_OK) {
        console_panic(boot_status_string(boot_status));
    }

    acpi_status = acpi_root_discover(&context, &acpi_root);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    acpi_status = acpi_madt_discover(&acpi_root, &acpi_madt);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    acpi_status = acpi_topology_discover(&acpi_madt, &boot_topology);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    acpi_status = acpi_fadt_discover(&acpi_root, &acpi_fadt);

    if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    }

    /*
     * The MCFG is optional, so its absence is recorded rather than fatal, and
     * every other refusal is still a refusal. Discovering it here keeps every
     * firmware table read through the early identity map before paging takes
     * ownership of the address space.
     */
    acpi_status = acpi_mcfg_discover(&acpi_root, &boot_mcfg);

    if (acpi_status == ACPI_STATUS_MISSING_MCFG) {
        boot_mcfg_present = false;
    } else if (acpi_status != ACPI_STATUS_OK) {
        console_panic(acpi_status_string(acpi_status));
    } else {
        boot_mcfg_present = true;
    }

    pm_timer_status = pm_timer_initialize(&acpi_fadt);

    if (pm_timer_status != PM_TIMER_STATUS_OK) {
        console_panic(pm_timer_status_string(pm_timer_status));
    }

    report_boot_context(&context);
    report_acpi_root(&acpi_root);
    report_acpi_madt(&acpi_madt);
    report_acpi_topology(&boot_topology);
    pm_timer_state = pm_timer_get_state();
    report_acpi_fadt(&acpi_fadt);
    report_pm_timer(&pm_timer_state);
    report_acpi_mcfg(&boot_mcfg, boot_mcfg_present);
    console_write("Seneri OS: ACPI root verified\n");
    console_write("Seneri OS: ACPI MADT verified\n");
    console_write("Seneri OS: ACPI topology verified\n");
    console_write("Seneri OS: ACPI FADT verified\n");
    console_write("Seneri OS: ACPI configuration windows verified\n");
    apic_status = apic_bring_online(&boot_topology);

    if (apic_status != APIC_STATUS_OK) {
        console_panic(apic_status_string(apic_status));
    }

    apic_state = apic_get_state();
    report_apic(&apic_state);
    console_write("Seneri OS: local APIC online\n");
    ioapic_status = ioapic_initialize(&boot_topology);

    if (ioapic_status != IOAPIC_STATUS_OK) {
        console_panic(ioapic_status_string(ioapic_status));
    }

    ioapic_state = ioapic_get_state();
    report_ioapic(&ioapic_state);
    console_write("Seneri OS: I/O APIC online\n");
    frame_status = frame_allocator_initialize(&context);

    if (frame_status != FRAME_STATUS_OK) {
        console_panic(frame_status_string(frame_status));
    }

    stats = frame_allocator_get_stats();
    report_allocator(&stats);
    prove_frame_lifecycle();

    stats = frame_allocator_get_stats();

    if (stats.allocated_frames != 0U) {
        console_panic("frame lifecycle leaked a physical frame");
    }

    /*
     * The tables come from the frame allocator, so this cannot run earlier than
     * here. It has to run before the scenarios, because every one of them now
     * executes on the kernel's own hierarchy rather than on boot.S's.
     */
    install_page_tables(&boot_topology,
        boot_mcfg_present ? &boot_mcfg : NULL, &context.framebuffer);
    prove_paging_lifecycle();
    bring_up_heap();
    prove_heap_lifecycle();

    console_write("Seneri OS: day one passed\n");
    console_write("Seneri OS: memory foundation passed\n");
    test_scenario = kernel_test_select(&context);
    kernel_test_run(test_scenario, boot_mcfg_present ? &boot_mcfg : NULL,
        boot_mcfg_present, &context.framebuffer);

    if (!interrupt_breakpoint_self_test()) {
        console_panic("breakpoint register self-test failed");
    }

    if (!interrupt_ist_self_test()) {
        console_panic("IST routing self-test failed");
    }

    if (!interrupt_pic_spurious_self_test()) {
        console_panic("PIC spurious interrupt self-test failed");
    }

    prove_timer_route(PIT_ROUTE_LEGACY_PIC);
    prove_timer_route(PIT_ROUTE_IO_APIC);
    retire_legacy_interrupt_path();
    prove_timer_route(PIT_ROUTE_IO_APIC);

    /*
     * Establish the reference before anything is calibrated from it, calibrate
     * both derived clocks against it, then retire the 8254 and prove the three
     * still agree with it gone. The order is the argument: the PIT may only be
     * retired after something that does not depend on it can time an interval.
     */
    prove_pm_timer();
    prove_apic_timer();
    prove_tsc();
    retire_pit();
    prove_clocks_without_pit();
    prove_monotonic_time();
    bring_up_pci();
    prove_threads();
    prove_framebuffer(&context.framebuffer);

    if (framebuffer_is_active()) {
        draw_logo();
    }

    /*
     * Re-walk the installed hierarchy at the end of boot. Everything between
     * the switch and here ran on it, including three subsystems that write
     * device memory through it, so this is what proves none of them corrupted
     * a table or turned a permission back off.
     */
    paging_status = paging_verify();

    if (paging_status != PAGING_STATUS_OK) {
        console_panic(paging_status_string(paging_status));
    }

    heap_status = heap_verify();

    if (heap_status != HEAP_STATUS_OK) {
        console_panic(heap_status_string(heap_status));
    }

    pci_status = pci_verify();

    if (pci_status != PCI_STATUS_OK) {
        console_panic(pci_status_string(pci_status));
    }

    console_write("Seneri OS: exception probes passed\n");
    console_write("Seneri OS: PIC spurious paths passed\n");
    console_write("Seneri OS: PIT delivered eight interrupts\n");
    console_write("Seneri OS: I/O APIC delivered eight interrupts\n");
    console_write("Seneri OS: legacy 8259 retired\n");
    console_write("Seneri OS: timer survives legacy retirement\n");
    console_write("Seneri OS: local APIC timer delivered eight interrupts\n");
    console_write("Seneri OS: TSC reference established\n");
    console_write("Seneri OS: PM timer independent reference established\n");
    console_write("Seneri OS: PIT retired\n");
    console_write("Seneri OS: clocks survive PIT retirement\n");
    console_write("Seneri OS: deadline timers online\n");
    console_write("Seneri OS: monotonic time established\n");
    console_write("Seneri OS: virtual memory established\n");
    console_write("Seneri OS: kernel heap established\n");
    console_write("Seneri OS: PCI enumeration established\n");
    console_write("Seneri OS: kernel threads passed\n");
    console_write("Seneri OS: framebuffer passed\n");
    console_write("Seneri OS: logo passed\n");
    console_write("Seneri OS: never triple fault milestone passed\n");

    if (test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    console_halt();
}
