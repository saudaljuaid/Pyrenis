/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/acpi_util.h>
#include <seneri/apic.h>
#include <seneri/apic_timer.h>
#include <seneri/clock.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/framebuffer.h>
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/paging.h>
#include <seneri/pci.h>
#include <seneri/pic.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>
#include <seneri/test.h>
#include <seneri/thread.h>
#include <seneri/timer.h>
#include <seneri/tsc.h>

#define QEMU_EXIT_PORT UINT16_C(0x00F4)
#define QEMU_FAILURE_VALUE UINT8_C(0x7F)
#define PAGE_FAULT_TEST_ADDRESS UINT64_C(0x0000000100000000)
#define PIT_TEST_FREQUENCY UINT32_C(100)
#define PIT_TEST_TICKS UINT64_C(8)
#define APIC_TIMER_TEST_FREQUENCY UINT32_C(100)
#define APIC_TIMER_TEST_TICKS UINT64_C(20)
#define TSC_MONOTONIC_READS 64U

/*
 * Intel SDM volume 3A section 4.7 defines the page-fault error code: bit 0 is
 * P, bit 1 is W/R and bit 2 is U/S. A supervisor write to a present read-only
 * page is therefore P=1 W=1 U=0. That is what distinguishes this scenario's
 * fault from the page-fault scenario's absent page, which is P=0 W=0 U=0.
 */
#define PAGING_TEST_FAULT_ERROR_CODE UINT64_C(0x03)
#define PAGING_TEST_PATTERN UINT8_C(0x5A)

/* Enough repetitions that one leaked table per cycle is unmistakable. */
#define PAGING_TEST_CYCLES 64U

/* An address inside the bulk 2 MiB identity map, above the linked image. */
#define PAGING_TEST_HUGE_ADDRESS UINT64_C(0x0000000000A00000)

/*
 * A supervisor write to an absent page is P=0 W=1 U=0. That is a third distinct
 * error code: the page-fault scenario reads an absent page at P=0 W=0 U=0, and
 * the paging scenario writes a present read-only page at P=1 W=1 U=0. No two of
 * the three scenarios can pass on each other's fault.
 */
#define HEAP_TEST_FAULT_ERROR_CODE UINT64_C(0x02)
#define HEAP_TEST_PATTERN UINT8_C(0xC3)

/*
 * Ten milliseconds of the ACPI timer, which counts at 3.579545 MHz, and the
 * 200 ms interval the local APIC timer defines by counting twenty of its own
 * ticks at 100 Hz. Two hundred milliseconds is 4.3% of the narrowest counter's
 * period, so the measurement stays far inside a single wrap.
 */
#define PM_TIMER_TEST_TICKS UINT32_C(35795)
#define PM_TIMER_TEST_FREQUENCY UINT32_C(100)
#define PM_TIMER_TEST_APIC_TICKS UINT64_C(20)

/*
 * Three deadlines, 20 ms apart. Far enough apart that the fixed cost of
 * reprogramming between them cannot reorder them, and short enough that the
 * whole scenario stays well inside its QEMU timeout.
 */
#define TIMERS_TEST_COUNT 3U
#define TIMERS_TEST_STEP_NS UINT64_C(20000000)

volatile uint8_t kernel_test_double_fault_armed;
static enum kernel_test_scenario active_scenario;

static size_t literal_length(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static bool token_equals(const char *token, size_t token_length, const char *literal)
{
    const size_t expected_length = literal_length(literal);

    if (token_length != expected_length) {
        return false;
    }

    for (size_t index = 0; index < token_length; ++index) {
        if (token[index] != literal[index]) {
            return false;
        }
    }

    return true;
}

static bool token_has_prefix(
    const char *token,
    size_t token_length,
    const char *prefix,
    size_t *value_offset
)
{
    const size_t prefix_length = literal_length(prefix);

    if (token_length < prefix_length) {
        return false;
    }

    for (size_t index = 0; index < prefix_length; ++index) {
        if (token[index] != prefix[index]) {
            return false;
        }
    }

    *value_offset = prefix_length;
    return true;
}

static enum kernel_test_scenario scenario_from_value(
    const char *value,
    size_t length
)
{
    if (token_equals(value, length, "normal")) {
        return KERNEL_TEST_NORMAL;
    }

    if (token_equals(value, length, "breakpoint")) {
        return KERNEL_TEST_BREAKPOINT;
    }

    if (token_equals(value, length, "invalid-opcode")) {
        return KERNEL_TEST_INVALID_OPCODE;
    }

    if (token_equals(value, length, "page-fault")) {
        return KERNEL_TEST_PAGE_FAULT;
    }

    if (token_equals(value, length, "ist")) {
        return KERNEL_TEST_IST;
    }

    if (token_equals(value, length, "pit")) {
        return KERNEL_TEST_PIT;
    }

    if (token_equals(value, length, "unexpected")) {
        return KERNEL_TEST_UNEXPECTED;
    }

    if (token_equals(value, length, "double-fault")) {
        return KERNEL_TEST_DOUBLE_FAULT;
    }

    if (token_equals(value, length, "apic")) {
        return KERNEL_TEST_APIC;
    }

    if (token_equals(value, length, "ioapic")) {
        return KERNEL_TEST_IOAPIC;
    }

    if (token_equals(value, length, "retired")) {
        return KERNEL_TEST_RETIRED;
    }

    if (token_equals(value, length, "apic-timer")) {
        return KERNEL_TEST_APIC_TIMER;
    }

    if (token_equals(value, length, "tsc")) {
        return KERNEL_TEST_TSC;
    }

    if (token_equals(value, length, "pm-timer")) {
        return KERNEL_TEST_PM_TIMER;
    }

    if (token_equals(value, length, "pit-retired")) {
        return KERNEL_TEST_PIT_RETIRED;
    }

    if (token_equals(value, length, "timers")) {
        return KERNEL_TEST_TIMERS;
    }

    if (token_equals(value, length, "paging")) {
        return KERNEL_TEST_PAGING;
    }

    if (token_equals(value, length, "heap")) {
        return KERNEL_TEST_HEAP;
    }

    if (token_equals(value, length, "pci")) {
        return KERNEL_TEST_PCI;
    }

    if (token_equals(value, length, "pci-ecam")) {
        return KERNEL_TEST_PCI_ECAM;
    }

    if (token_equals(value, length, "threads")) {
        return KERNEL_TEST_THREADS;
    }

    if (token_equals(value, length, "thread-guard")) {
        return KERNEL_TEST_THREAD_GUARD;
    }

    if (token_equals(value, length, "framebuffer")) {
        return KERNEL_TEST_FRAMEBUFFER;
    }

    return KERNEL_TEST_INVALID;
}

static uint8_t scenario_exit_value(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return UINT8_C(0x10);
    case KERNEL_TEST_BREAKPOINT:
        return UINT8_C(0x11);
    case KERNEL_TEST_INVALID_OPCODE:
        return UINT8_C(0x12);
    case KERNEL_TEST_PAGE_FAULT:
        return UINT8_C(0x13);
    case KERNEL_TEST_IST:
        return UINT8_C(0x14);
    case KERNEL_TEST_PIT:
        return UINT8_C(0x15);
    case KERNEL_TEST_UNEXPECTED:
        return UINT8_C(0x16);
    case KERNEL_TEST_DOUBLE_FAULT:
        return UINT8_C(0x17);
    case KERNEL_TEST_APIC:
        return UINT8_C(0x18);
    case KERNEL_TEST_IOAPIC:
        return UINT8_C(0x19);
    case KERNEL_TEST_RETIRED:
        return UINT8_C(0x1A);
    case KERNEL_TEST_APIC_TIMER:
        return UINT8_C(0x1B);
    case KERNEL_TEST_TSC:
        return UINT8_C(0x1C);
    case KERNEL_TEST_PM_TIMER:
        return UINT8_C(0x1D);
    case KERNEL_TEST_PIT_RETIRED:
        return UINT8_C(0x1E);
    case KERNEL_TEST_TIMERS:
        return UINT8_C(0x1F);
    case KERNEL_TEST_PAGING:
        return UINT8_C(0x20);
    case KERNEL_TEST_HEAP:
        return UINT8_C(0x21);
    case KERNEL_TEST_PCI:
        return UINT8_C(0x22);
    case KERNEL_TEST_PCI_ECAM:
        return UINT8_C(0x23);
    case KERNEL_TEST_THREADS:
        return UINT8_C(0x24);
    case KERNEL_TEST_THREAD_GUARD:
        return UINT8_C(0x25);
    case KERNEL_TEST_FRAMEBUFFER:
        return UINT8_C(0x26);
    default:
        return QEMU_FAILURE_VALUE;
    }
}

static void test_marker(const char *kind, enum kernel_test_scenario scenario)
{
    console_write("ST ");
    console_write(kind);
    console_putc(' ');
    console_write(kernel_test_scenario_name(scenario));
    console_putc('\n');
}

static _Noreturn void kernel_test_pass(void)
{
    const uint8_t exit_value = scenario_exit_value(active_scenario);

    test_marker("PASS", active_scenario);
    cpu_out32(QEMU_EXIT_PORT, exit_value);
    console_halt();
}

enum kernel_test_scenario kernel_test_select(const struct boot_context *context)
{
    static const char prefix[] = "seneri.test=";
    enum kernel_test_scenario selected = KERNEL_TEST_NONE;
    size_t offset = 0;

    kernel_test_double_fault_armed = 0U;
    active_scenario = KERNEL_TEST_NONE;

    if (context == NULL || context->command_line == NULL) {
        return KERNEL_TEST_NONE;
    }

    while (offset < context->command_line_length) {
        size_t token_start;
        size_t token_length;
        size_t value_offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] == ' ') {
            ++offset;
        }

        token_start = offset;

        while (offset < context->command_line_length &&
               context->command_line[offset] != ' ') {
            ++offset;
        }

        token_length = offset - token_start;

        if (token_length == 0U || !token_has_prefix(
                context->command_line + token_start,
                token_length,
                prefix,
                &value_offset
            )) {
            continue;
        }

        if (selected != KERNEL_TEST_NONE) {
            return KERNEL_TEST_INVALID;
        }

        selected = scenario_from_value(
            context->command_line + token_start + value_offset,
            token_length - value_offset
        );

        if (selected == KERNEL_TEST_INVALID) {
            return selected;
        }
    }

    active_scenario = selected;
    return selected;
}

/*
 * Enabling the local APIC takes the 8259 pair off the processor's direct
 * interrupt path. This scenario proves the replacement path: the APIC is online
 * and agrees with firmware, and the PIT still delivers through LINT0.
 */
static void apic_scenario(void)
{
    const struct apic_state apic = apic_get_state();
    enum pit_status pit_status;

    if (!apic_is_online() || !apic.online) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic.base_address == 0U || apic.max_lvt_entry < 4U) {
        kernel_test_fail("local APIC reported an unusable register window");
    }

    if (!apic.legacy_interrupts_routed) {
        kernel_test_fail("local APIC did not route legacy interrupts");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("PIT stopped delivering once the local APIC was on");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Prove the timer arrives through the I/O APIC rather than the 8259 pair: the
 * legacy line stays masked, the redirection entry carries the ACPI override,
 * and the interrupt is acknowledged at the local APIC.
 */
static void ioapic_scenario(void)
{
    const struct ioapic_state ioapic = ioapic_get_state();
    enum pit_status pit_status;

    if (!ioapic_is_initialized() || ioapic.count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (ioapic.units[0].entry_count < 16U) {
        kernel_test_fail("I/O APIC cannot redirect the ISA interrupts");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC) {
        kernel_test_fail("timer did not take the I/O APIC route");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("I/O APIC routing unmasked a legacy PIC line");
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("I/O APIC delivered too few timer interrupts");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8259 pair and prove the machine keeps its timer. This is the
 * scenario that would catch a retirement which silently took interrupt
 * delivery with it.
 */
static void retired_scenario(void)
{
    enum pit_status pit_status;
    enum pic_status pic_status;

    if (!pic_is_initialized() || pic_is_retired()) {
        kernel_test_fail("legacy PIC was not in its expected initial state");
    }

    pic_status = pic_retire();

    if (pic_status != PIC_STATUS_OK) {
        kernel_test_fail(pic_status_string(pic_status));
    }

    if (apic_retire_legacy_routing() != APIC_STATUS_OK) {
        kernel_test_fail("local APIC kept carrying legacy interrupts");
    }

    if (!pic_is_retired() || pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("legacy PIC is not fully masked after retirement");
    }

    if (pic_set_mask(0U, false) != PIC_STATUS_RETIRED ||
        pic_retire() != PIC_STATUS_RETIRED) {
        kernel_test_fail("retired PIC accepted a further mutation");
    }

    if (apic_get_state().legacy_interrupts_routed) {
        kernel_test_fail("local APIC still reports legacy routing");
    }

    pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    pit_status = pit_stop();

    if (pit_status != PIT_STATUS_OK) {
        kernel_test_fail(pit_status_string(pit_status));
    }

    if (pit_ticks() < PIT_TEST_TICKS) {
        kernel_test_fail("timer stopped once the legacy PIC was retired");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Calibrate and run the local APIC timer, then check the rate it measured is
 * consistent with the reference it measured against. A timer that ticks but
 * counts at the wrong rate is the failure this scenario exists to find.
 */
static void apic_timer_scenario(void)
{
    enum apic_timer_status status;
    uint64_t elapsed_ticks;
    uint64_t measured_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!apic_is_online()) {
        kernel_test_fail("local APIC is not online");
    }

    if (apic_timer_is_calibrated() || apic_timer_is_running()) {
        kernel_test_fail("local APIC timer was already in use");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_NOT_CALIBRATED) {
        kernel_test_fail("uncalibrated local APIC timer agreed to run");
    }

    status = apic_timer_calibrate();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_counts_per_second() == 0U) {
        kernel_test_fail("local APIC timer calibrated to a zero rate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("local APIC timer accepted a second calibration");
    }

    status = apic_timer_start(APIC_TIMER_TEST_FREQUENCY);

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) !=
        APIC_TIMER_STATUS_ALREADY_RUNNING) {
        kernel_test_fail("local APIC timer started twice");
    }

    /*
     * Let the timer count out a known number of its own ticks and measure how
     * long that took on the ACPI timer it was calibrated from. Checking the
     * duration rather than a tick count catches the failure a tick count cannot:
     * a timer whose rate is wrong still delivers every tick it is asked for, it
     * just takes the wrong amount of time doing it.
     */
    start = pm_timer_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    elapsed_ticks = apic_timer_ticks();
    status = apic_timer_stop();

    if (status != APIC_TIMER_STATUS_OK) {
        kernel_test_fail(apic_timer_status_string(status));
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("reference clock reported no duration");
    }

    if (elapsed_ticks < APIC_TIMER_TEST_TICKS) {
        kernel_test_fail("local APIC timer delivered too few interrupts");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("local APIC timer rate disagrees with its reference");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Establish the time-stamp counter as a second reference and check it against
 * the first. Two clocks calibrated from the same ruler must agree about the
 * same interval; a clock that only agrees with itself proves nothing.
 */
static void tsc_scenario(void)
{
    struct tsc_state tsc;
    uint64_t previous;
    uint64_t start;
    uint64_t measured_ns;
    uint64_t expected_ns;
    enum tsc_status status;

    if (tsc_is_calibrated()) {
        kernel_test_fail("TSC was already calibrated");
    }

    if (tsc_span_nanoseconds(0U, UINT64_C(1000)) != 0U) {
        kernel_test_fail("uncalibrated TSC reported a duration");
    }

    status = tsc_calibrate();

    if (status != TSC_STATUS_OK) {
        kernel_test_fail(tsc_status_string(status));
    }

    tsc = tsc_get_state();

    if (!tsc.present || tsc.frequency_hz == 0U) {
        kernel_test_fail("TSC calibrated to an unusable rate");
    }

    if (tsc_calibrate() != TSC_STATUS_ALREADY_CALIBRATED) {
        kernel_test_fail("TSC accepted a second calibration");
    }

    /* A counter that steps backwards cannot order anything. */
    previous = tsc_read();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        const uint64_t current = tsc_read();

        if (current < previous) {
            kernel_test_fail("TSC ran backwards between reads");
        }

        previous = current;
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not calibrate");
    }

    if (apic_timer_start(APIC_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not start");
    }

    start = tsc_read();

    if (apic_timer_wait_for_ticks(APIC_TIMER_TEST_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock stopped delivering");
    }

    measured_ns = tsc_span_nanoseconds(start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("second clock would not stop");
    }

    expected_ns = APIC_TIMER_TEST_TICKS * UINT64_C(1000000000) /
        APIC_TIMER_TEST_FREQUENCY;

    if (measured_ns < expected_ns / 2U || measured_ns > expected_ns * 2U) {
        kernel_test_fail("TSC and local APIC timer disagree about an interval");
    }
}

/*
 * Establish the ACPI power management timer as an independent reference, then
 * check the two calibrated clocks against it.
 *
 * The local APIC timer and the TSC were both measured against the PIT, so they
 * agree with each other even if that shared measurement was wrong. This timer's
 * rate is fixed by the ACPI specification and is measured against nothing, so
 * one interval described by all three is the first evidence that the PIT
 * measurement itself was right. Retiring the PIT is the increment after this
 * one, and only on the strength of this agreement.
 */
static void pm_timer_scenario(void)
{
    struct pm_timer_state pm;
    struct acpi_fadt probe;
    uint64_t waited_ticks = 0U;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t end;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U ||
        (pm.counter_bits != ACPI_PM_TIMER_BASE_BITS &&
         pm.counter_bits != ACPI_PM_TIMER_EXTENDED_BITS)) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /*
     * The timer is discovered once. A second description is refused before any
     * of its fields are read, so a zeroed one is enough to prove the refusal.
     */
    acpi_bytes_zero(&probe, sizeof(probe));

    if (pm_timer_initialize(&probe) != PM_TIMER_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("ACPI PM timer accepted a second description");
    }

    /* The counter has to advance on its own before it can time anything. */
    if (pm_timer_wait(PM_TIMER_TEST_TICKS, &waited_ticks) !=
        PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer did not advance within its bound");
    }

    if (waited_ticks < PM_TIMER_TEST_TICKS) {
        kernel_test_fail("ACPI PM timer wait returned early");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("time-stamp counter would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    /*
     * One interval, three opinions. The APIC timer defines it by counting its
     * own ticks; the PM timer and the TSC each measure it without being told.
     */
    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    end = pm_timer_read();
    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, end, &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pm-timer: PM ");
    console_write_u64(measured_ns);
    console_write(" ns, APIC timer ");
    console_write_u64(expected_ns);
    console_write(" ns, TSC ");
    console_write_u64(reference_ns);
    console_write(" ns\n");

    /*
     * The local APIC timer is held to the tight bound: it and the PIT it was
     * calibrated against are driven from the same source under emulation, so
     * this comparison is the one that must catch a rate wrong by a factor.
     */
    if (!pm_timer_durations_agree(
            measured_ns,
            expected_ns,
            PM_TIMER_TOLERANCE_QUARTER
        )) {
        kernel_test_fail("PM timer and local APIC timer disagree on interval");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree about an interval");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Retire the 8254 and prove the machine keeps its clocks.
 *
 * This is the mirror of the `retired` scenario, which proved the machine keeps
 * its timer after the 8259 pair is latched shut. Here the timer itself goes: the
 * PIT is proved working, retired, and then refuses every further mutation, and
 * both derived clocks are calibrated and cross-checked with it dead. Calibration
 * used to spin the PIT, so a retirement that broke that path would show up here
 * as a clock that will not calibrate at all rather than as one running slow.
 */
static void pit_retired_scenario(void)
{
    struct pm_timer_state pm;
    uint64_t tsc_start;
    uint64_t measured_ns;
    uint64_t reference_ns;
    uint64_t expected_ns;
    uint32_t start;
    uint32_t span = 0U;

    if (!pm_timer_is_present()) {
        kernel_test_fail("ACPI PM timer was not discovered during boot");
    }

    pm = pm_timer_get_state();

    if (pm.port == 0U) {
        kernel_test_fail("ACPI PM timer reported an unusable description");
    }

    /* The PIT still works at this point, and is proved so before it goes. */
    if (pit_is_retired()) {
        kernel_test_fail("PIT was already retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not start before its retirement");
    }

    if (pit_wait_for_ticks(PIT_TEST_TICKS) != PIT_STATUS_OK) {
        kernel_test_fail("PIT would not deliver before its retirement");
    }

    if (pit_retire() != PIT_STATUS_OK) {
        kernel_test_fail("PIT refused to retire");
    }

    /* A retired PIT is latched: running, restarting and re-retiring all fail. */
    if (!pit_is_retired() || pit_is_running()) {
        kernel_test_fail("PIT is not fully retired");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) !=
            PIT_STATUS_RETIRED ||
        pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC) !=
            PIT_STATUS_RETIRED ||
        pit_retire() != PIT_STATUS_RETIRED) {
        kernel_test_fail("retired PIT accepted a further mutation");
    }

    /* Both derived clocks must calibrate with no PIT to lean on. */
    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate without the PIT");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate without the PIT");
    }

    if (apic_timer_counts_per_second() == 0U || tsc_frequency() == 0U) {
        kernel_test_fail("a clock calibrated to an unusable rate");
    }

    if (apic_timer_start(PM_TIMER_TEST_FREQUENCY) != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not start");
    }

    start = pm_timer_read();
    tsc_start = tsc_read();

    if (apic_timer_wait_for_ticks(PM_TIMER_TEST_APIC_TICKS) !=
        APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer stopped delivering");
    }

    reference_ns = tsc_span_nanoseconds(tsc_start, tsc_read());

    if (apic_timer_stop() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not stop");
    }

    if (pm_timer_span(start, pm_timer_read(), &span) != PM_TIMER_STATUS_OK) {
        kernel_test_fail("ACPI PM timer span is not a duration");
    }

    measured_ns = pm_timer_ticks_to_nanoseconds(span);
    expected_ns = PM_TIMER_TEST_APIC_TICKS * UINT64_C(1000000000) /
        PM_TIMER_TEST_FREQUENCY;

    console_write("ST INFO pit-retired: PM ");
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
        kernel_test_fail("clocks disagree on an interval without the PIT");
    }

    if (!pm_timer_durations_agree(
            measured_ns,
            reference_ns,
            PM_TIMER_TOLERANCE_HALF
        )) {
        kernel_test_fail("PM timer and TSC disagree without the PIT");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written by timer callbacks inside the timer interrupt and read by the scenario
 * outside it, so the compiler must not keep either in a register across the halt
 * inside a sleep.
 */
static volatile uint32_t timers_fired[TIMERS_TEST_COUNT];
static volatile size_t timers_fired_count;

static void timers_record(uint64_t deadline_ns, void *context)
{
    (void)deadline_ns;

    if (timers_fired_count < TIMERS_TEST_COUNT) {
        timers_fired[timers_fired_count] = *(const uint32_t *)context;
        ++timers_fired_count;
    }
}

/*
 * Establish the monotonic clock and deadline timers, and prove the two things
 * that make them usable: the clock never steps backwards, and a deadline arrives
 * after the instant it named rather than before it.
 *
 * A sleep that returns early is the failure worth hunting. It would not look
 * like a failure - the call returns, the callback ran - and every wait built on
 * it would be silently short. So the scenario checks the elapsed time against
 * the clock rather than trusting that the callback fired.
 */
static void timers_scenario(void)
{
    static const uint32_t labels[TIMERS_TEST_COUNT] = {1U, 2U, 3U};
    size_t heap_live_before;
    uint64_t identifiers[TIMERS_TEST_COUNT] = {0U, 0U, 0U};
    uint64_t previous;
    uint64_t start;
    uint64_t slept_ns;
    uint64_t spare = 0U;
    uint64_t now;

    /* Before the clock has an origin it reports nothing rather than garbage. */
    if (clock_is_started() || clock_monotonic_ns() != 0U) {
        kernel_test_fail("monotonic clock was already started");
    }

    if (clock_start() != CLOCK_STATUS_NO_SOURCE) {
        kernel_test_fail("clock started without a calibrated counter");
    }

    if (tsc_calibrate() != TSC_STATUS_OK) {
        kernel_test_fail("TSC would not calibrate");
    }

    if (apic_timer_calibrate() != APIC_TIMER_STATUS_OK) {
        kernel_test_fail("local APIC timer would not calibrate");
    }

    if (clock_start() != CLOCK_STATUS_OK) {
        kernel_test_fail("monotonic clock would not start");
    }

    if (clock_start() != CLOCK_STATUS_ALREADY_STARTED) {
        kernel_test_fail("monotonic clock started twice");
    }

    /* A clock that steps backwards cannot order anything. */
    previous = clock_monotonic_ns();

    for (size_t index = 0; index < TSC_MONOTONIC_READS; ++index) {
        now = clock_monotonic_ns();

        if (now < previous) {
            kernel_test_fail("monotonic clock stepped backwards");
        }

        previous = now;
    }

    if (clock_get_state().backward_steps != 0U) {
        kernel_test_fail("monotonic clock had to repair a reading");
    }

    /* Deadlines need the clock, and refuse to run without it. */
    if (timer_arm(previous + TIMERS_TEST_STEP_NS, timers_record, NULL, &spare) !=
        TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline armed before the timers were started");
    }

    /*
     * The deadline table is a heap allocation now, not a static array, so
     * starting must take exactly one block and report the capacity it got.
     */
    heap_live_before = heap_get_state().live_allocations;

    if (timer_capacity() != 0U) {
        kernel_test_fail("deadline timers held a table before starting");
    }

    if (timer_start() != TIMER_STATUS_OK) {
        kernel_test_fail("deadline timers would not start");
    }

    if (timer_capacity() != TIMER_MAX_PENDING ||
        heap_get_state().live_allocations != heap_live_before + 1U) {
        kernel_test_fail("starting did not take one table from the heap");
    }

    if (timer_start() != TIMER_STATUS_ALREADY_STARTED) {
        kernel_test_fail("deadline timers started twice");
    }

    /* A deadline already gone, and one too near to program, are both refused. */
    now = clock_monotonic_ns();

    if (timer_arm(0U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL ||
        spare != 0U ||
        timer_arm(now + 1U, timers_record, NULL, &spare) !=
            TIMER_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a deadline in the past was accepted");
    }

    if (timer_cancel(0U) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_cancel(UINT64_MAX) != TIMER_STATUS_UNKNOWN_TIMER) {
        kernel_test_fail("cancelling an unknown deadline was accepted");
    }

    /* Three deadlines, armed out of order, must fire in time order. */
    timers_fired_count = 0U;
    start = clock_monotonic_ns();

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        const size_t reversed = TIMERS_TEST_COUNT - 1U - index;

        if (timer_arm(
                start + TIMERS_TEST_STEP_NS * (uint64_t)(reversed + 1U),
                timers_record,
                (void *)&labels[reversed],
                &identifiers[reversed]
            ) != TIMER_STATUS_OK ||
            identifiers[reversed] == 0U) {
            kernel_test_fail("a deadline would not arm");
        }
    }

    if (timer_pending_count() != TIMERS_TEST_COUNT) {
        kernel_test_fail("deadline timer table lost an entry");
    }

    /* Sleeping past all three collects them; the sleep is itself a deadline. */
    slept_ns = clock_monotonic_ns();

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) !=
        TIMER_STATUS_OK) {
        kernel_test_fail("sleep did not complete");
    }

    slept_ns = clock_monotonic_ns() - slept_ns;

    if (timers_fired_count != TIMERS_TEST_COUNT) {
        kernel_test_fail("not every deadline fired");
    }

    for (size_t index = 0; index < TIMERS_TEST_COUNT; ++index) {
        if (timers_fired[index] != labels[index]) {
            kernel_test_fail("deadlines fired out of order");
        }
    }

    if (slept_ns < TIMERS_TEST_STEP_NS * (TIMERS_TEST_COUNT + 1U)) {
        kernel_test_fail("sleep returned before its deadline");
    }

    if (timer_pending_count() != 0U || timer_expiry_count() == 0U) {
        kernel_test_fail("deadline timer table did not settle");
    }

    /* A cancelled deadline must not fire, and its identifier must go stale. */
    timers_fired_count = 0U;
    now = clock_monotonic_ns();

    if (timer_arm(
            now + TIMERS_TEST_STEP_NS,
            timers_record,
            (void *)&labels[0],
            &spare
        ) != TIMER_STATUS_OK) {
        kernel_test_fail("a deadline would not arm for cancellation");
    }

    if (timer_cancel(spare) != TIMER_STATUS_OK ||
        timer_cancel(spare) != TIMER_STATUS_UNKNOWN_TIMER ||
        timer_pending_count() != 0U) {
        kernel_test_fail("cancelling a deadline did not release it");
    }

    if (timer_sleep_ns(TIMERS_TEST_STEP_NS * 2U) != TIMER_STATUS_OK) {
        kernel_test_fail("sleep after a cancellation did not complete");
    }

    if (timers_fired_count != 0U) {
        kernel_test_fail("a cancelled deadline fired anyway");
    }

    if (timer_stop() != TIMER_STATUS_OK ||
        timer_is_started() ||
        timer_stop() != TIMER_STATUS_NOT_STARTED) {
        kernel_test_fail("deadline timers would not stop");
    }

    /*
     * And stopping gives it back. A subsystem that took heap memory once per
     * start and never returned it would look perfectly correct in every other
     * check here and exhaust the heap over a long-running kernel.
     */
    if (timer_capacity() != 0U ||
        heap_get_state().live_allocations != heap_live_before) {
        kernel_test_fail("stopping did not return the table to the heap");
    }

    if (heap_verify() != HEAP_STATUS_OK) {
        kernel_test_fail("the heap did not survive the deadline table");
    }

    if (apic_spurious_count() != 0U) {
        kernel_test_fail("local APIC raised a spurious interrupt");
    }
}

/*
 * Written through a volatile pointer inside the scenario and read back after a
 * permission change, so the compiler cannot cache either side of it or assume
 * it knows what a page it never mapped contains.
 */
static volatile uint8_t paging_scratch;

/* Larger than early boot should place on the 16 KiB kernel stack. */
static struct acpi_topology paging_probe_topology;

/*
 * Prove the permissions are enforced by the processor rather than merely
 * recorded in a table.
 *
 * `make verify` has always refused an RWX load segment, and until this
 * increment that assertion was the only thing standing behind Seneri's W^X
 * claim - and it inspects the ELF file, not the machine the kernel runs on.
 * Everything below the rejections is the part a file check can never do: a
 * fresh frame is mapped writable, written, narrowed to read-only, and written
 * again, and the scenario passes only if the processor refuses the second write
 * with the exact fault a supervisor write to a present read-only page produces.
 *
 * The probe returns if the store succeeds, so a permission that quietly failed
 * to take shows up as a scenario failure rather than as a timeout.
 */
static void paging_scenario(void)
{
    volatile uint8_t *probe =
        (volatile uint8_t *)(uintptr_t)PAGING_PROBE_ADDRESS;
    const uint64_t text = (uint64_t)(uintptr_t)(const void *)
        paging_probe_write_site;
    const uint64_t data = (uint64_t)(uintptr_t)(const void *)&paging_scratch;
    struct paging_translation translation;
    struct paging_state paging;
    struct paging_audit audit;
    size_t frames_before;
    size_t tables_before;
    uintptr_t frame;

    if (!paging_is_active()) {
        kernel_test_fail("kernel page tables are not installed");
    }

    paging = paging_get_state();

    if (!paging.no_execute_active || !paging.write_protect_active) {
        kernel_test_fail("W^X is not enforceable on this processor");
    }

    if (paging.root_physical_address == 0U || paging.table_frames == 0U ||
        paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("installed page tables do not match their intent");
    }

    /* The whole point of the increment, read back off the live hierarchy. */
    if (paging_audit_hierarchy(&audit) != PAGING_STATUS_OK ||
        audit.leaf_count == 0U || audit.executable_leaves == 0U ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        kernel_test_fail("a live mapping is writable and executable");
    }

    if (paging_translate(text, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_EXECUTE ||
        translation.physical_address != text) {
        kernel_test_fail("kernel text is not read-only and executable");
    }

    if (paging_translate(data, &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.physical_address != data) {
        kernel_test_fail("kernel data is not writable and non-executable");
    }

    /* The null page is absent, so a null dereference cannot read low memory. */
    if (paging_translate(0U, &translation) != PAGING_STATUS_NOT_MAPPED ||
        translation.level != 0U) {
        kernel_test_fail("the null page is mapped");
    }

    /* Every refusal, through the public interface, against the live tables. */
    if (paging_map(PAGING_PROBE_ADDRESS + 1U, 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_UNALIGNED_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, 0U, PAGING_WRITE) !=
            PAGING_STATUS_ZERO_LENGTH ||
        paging_map(UINT64_C(0x0000800000000000), 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_NONCANONICAL_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE | PAGING_EXECUTE) !=
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        kernel_test_fail("a malformed mapping request was accepted");
    }

    if (paging_map(text & ~(SENERI_PAGE_SIZE - 1U), 0U, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED ||
        paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("an impossible mapping change was accepted");
    }

    /*
     * The bulk identity map uses 2 MiB leaves, and splitting one is deferred,
     * so a 4 KiB change inside one is refused rather than silently applied to
     * the whole 2 MiB.
     */
    if (paging_protect(PAGING_TEST_HUGE_ADDRESS, SENERI_PAGE_SIZE,
            PAGING_READ) != PAGING_STATUS_HUGE_PAGE_PRESENT ||
        paging_unmap(PAGING_TEST_HUGE_ADDRESS, SENERI_PAGE_SIZE) !=
            PAGING_STATUS_HUGE_PAGE_PRESENT) {
        kernel_test_fail("a 2 MiB mapping accepted a 4 KiB change");
    }

    if (paging_initialize(&paging_probe_topology, NULL, NULL) !=
        PAGING_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("page tables accepted a second installation");
    }

    if (frame_allocate(&frame) != FRAME_STATUS_OK) {
        kernel_test_fail("no frame was available for the probe page");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED) {
        kernel_test_fail("the probe page would not map exactly once");
    }

    *probe = PAGING_TEST_PATTERN;

    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a writable mapping did not hold a write");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.physical_address != (uint64_t)frame ||
        translation.permissions != PAGING_WRITE ||
        translation.level != 1U) {
        kernel_test_fail("the probe page does not translate to its frame");
    }

    if (paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not narrow to read-only");
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != (uint64_t)frame) {
        kernel_test_fail("narrowing a mapping changed what it points at");
    }

    /* Reading is still permitted, and the contents survived the change. */
    if (*probe != PAGING_TEST_PATTERN) {
        kernel_test_fail("a read-only mapping lost the page contents");
    }

    /*
     * Map and undo the same page many times over. One leaked interior table
     * per cycle is invisible in a single pass and fatal over a long-running
     * kernel, so the check that matters is that the frame count is identical
     * after sixty-four cycles - and the paging state's own table count with it.
     */
    if (paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not unmap before the cycle");
    }

    frames_before = frame_allocator_get_stats().free_frames;
    tables_before = paging_get_state().table_frames;

    for (size_t cycle = 0; cycle < PAGING_TEST_CYCLES; ++cycle) {
        uintptr_t cycle_frame;

        if (frame_allocate(&cycle_frame) != FRAME_STATUS_OK ||
            paging_map(PAGING_PROBE_ADDRESS, cycle_frame, SENERI_PAGE_SIZE,
                PAGING_WRITE) != PAGING_STATUS_OK ||
            paging_unmap(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE) !=
                PAGING_STATUS_OK ||
            frame_release(cycle_frame) != FRAME_STATUS_OK) {
            kernel_test_fail("a map and unmap cycle did not complete");
        }
    }

    if (frame_allocator_get_stats().free_frames != frames_before) {
        kernel_test_fail("repeated mapping leaked a physical frame");
    }

    if (paging_get_state().table_frames != tables_before) {
        kernel_test_fail("repeated mapping leaked a page table");
    }

    if (paging_verify() != PAGING_STATUS_OK) {
        kernel_test_fail("the hierarchy did not survive repeated mapping");
    }

    /* Put the probe page back so the fault below has something to narrow. */
    if (frame_allocate(&frame) != FRAME_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, SENERI_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_protect(PAGING_PROBE_ADDRESS, SENERI_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not come back read-only");
    }

    console_write("ST INFO paging: read-only write to ");
    console_write_hex(PAGING_PROBE_ADDRESS);
    console_write(" expecting P=1 W=1 U=0\n");

    /*
     * The store that must fault. If the processor takes it, control returns
     * here and kernel_test_run reports the failure; if it faults,
     * kernel_test_handle_fatal_interrupt matches the vector, the error code,
     * CR2 and the faulting instruction, and passes.
     */
    paging_probe_write(probe, (uint8_t)~PAGING_TEST_PATTERN);
}

/*
 * Prove the heap hands out memory that is actually distinct, that it refuses
 * everything it should, and that its guard pages are enforced by the processor.
 *
 * The refusals matter more here than anywhere else in the kernel. An allocator
 * that accepts a pointer it never returned will happily mark a live block free
 * and hand the same bytes to two callers, and nothing downstream can detect
 * that. So every wrong pointer this scenario can construct - interior, below
 * the window, above the window, unaligned, already freed - is checked by name.
 */
static void heap_scenario(void)
{
    volatile uint8_t *bytes;
    struct paging_translation translation;
    struct heap_state heap;
    uint64_t committed_before;
    size_t pages_before;
    enum heap_status status;
    void *first = NULL;
    void *second = NULL;
    void *third = NULL;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("kernel heap is not online");
    }

    if (heap_initialize() != HEAP_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("heap accepted a second initialization");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /* Malformed requests, each refused by its own name. */
    if (heap_allocate(0U, &first) != HEAP_STATUS_ZERO_SIZE || first != NULL ||
        heap_allocate(HEAP_SIZE + 1U, &first) != HEAP_STATUS_TOO_LARGE ||
        first != NULL ||
        heap_allocate(16U, NULL) != HEAP_STATUS_NULL_ARGUMENT) {
        kernel_test_fail("a malformed allocation request was accepted");
    }

    if (heap_allocate(64U, &first) != HEAP_STATUS_OK || first == NULL ||
        heap_allocate(64U, &second) != HEAP_STATUS_OK || second == NULL ||
        first == second) {
        kernel_test_fail("the heap would not produce two distinct blocks");
    }

    if (((uint64_t)(uintptr_t)first & (HEAP_ALIGNMENT - 1U)) != 0U ||
        ((uint64_t)(uintptr_t)second & (HEAP_ALIGNMENT - 1U)) != 0U) {
        kernel_test_fail("the heap returned a misaligned allocation");
    }

    /* Both blocks must lie inside the window, between the guards. */
    if ((uint64_t)(uintptr_t)first < HEAP_BASE ||
        (uint64_t)(uintptr_t)second >= HEAP_GUARD_ABOVE) {
        kernel_test_fail("the heap returned a block outside its window");
    }

    /* Every wrong pointer this scenario can construct. */
    if (heap_free(NULL) != HEAP_STATUS_NULL_ARGUMENT ||
        heap_free((void *)((uintptr_t)first + 1U)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)((uintptr_t)first + HEAP_ALIGNMENT)) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_BELOW) !=
            HEAP_STATUS_BAD_POINTER ||
        heap_free((void *)(uintptr_t)HEAP_GUARD_ABOVE) !=
            HEAP_STATUS_BAD_POINTER) {
        kernel_test_fail("the heap accepted a pointer it never returned");
    }

    if (heap_free(first) != HEAP_STATUS_OK ||
        heap_free(first) != HEAP_STATUS_DOUBLE_FREE) {
        kernel_test_fail("the heap accepted a double free");
    }

    /*
     * The freed block is the best fit for the same size again, so a heap that
     * reuses its free space hands back the identical address. A heap that only
     * ever grew would return something new here and slowly exhaust the window.
     */
    if (heap_allocate(64U, &third) != HEAP_STATUS_OK || third != first) {
        kernel_test_fail("the heap did not reuse a freed block");
    }

    bytes = (volatile uint8_t *)third;

    for (uint64_t index = 0; index < 64U; ++index) {
        bytes[index] = HEAP_TEST_PATTERN;
    }

    for (uint64_t index = 0; index < 64U; ++index) {
        if (bytes[index] != HEAP_TEST_PATTERN) {
            kernel_test_fail("a heap block did not hold what was written");
        }
    }

    if (heap_free(third) != HEAP_STATUS_OK ||
        heap_free(second) != HEAP_STATUS_OK) {
        kernel_test_fail("the heap refused to release its own allocation");
    }

    heap = heap_get_state();

    if (heap.live_allocations != 0U || heap.allocated_bytes != 0U ||
        heap.block_count != 1U) {
        kernel_test_fail("the heap did not coalesce back to one free block");
    }

    status = heap_verify();

    if (status != HEAP_STATUS_OK) {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * Ask for the entire window in one allocation. Which way this goes depends
     * on the machine, and both ways are worth checking, so the scenario asks
     * what happened rather than assuming how much memory it has.
     *
     * With more free memory than the window, growth commits every page and the
     * next byte requested must be refused at the window bound. With less, the
     * growth runs out of frames part way through, which is the only path that
     * exercises rollback - and then what matters is that the heap is left
     * exactly as it was, with every page that had been mapped given back.
     */
    committed_before = heap.committed_bytes;
    pages_before = heap.mapped_pages;
    status = heap_allocate(HEAP_SIZE, &first);

    if (status == HEAP_STATUS_OUT_OF_MEMORY) {
        heap = heap_get_state();

        if (first != NULL || heap_verify() != HEAP_STATUS_OK ||
            heap.committed_bytes != committed_before ||
            heap.mapped_pages != pages_before ||
            heap.live_allocations != 0U) {
            kernel_test_fail("a failed heap growth did not roll back");
        }

        console_write("ST INFO heap: growth rolled back at the frame limit\n");
    } else if (status == HEAP_STATUS_OK) {
        heap = heap_get_state();

        if (first == NULL || heap.committed_bytes != HEAP_SIZE ||
            heap.mapped_pages != HEAP_SIZE / PAGING_PAGE_SIZE) {
            kernel_test_fail("committing the window did not map every page");
        }

        if (heap_allocate(HEAP_ALIGNMENT, &second) !=
                HEAP_STATUS_OUT_OF_MEMORY ||
            second != NULL) {
            kernel_test_fail("a full heap accepted another allocation");
        }

        /* The guards survive the window being fully committed against them. */
        if (heap_verify() != HEAP_STATUS_OK ||
            heap_free(first) != HEAP_STATUS_OK ||
            heap_verify() != HEAP_STATUS_OK) {
            kernel_test_fail("heap invariants do not hold at full commitment");
        }
    } else {
        kernel_test_fail(heap_status_string(status));
    }

    /*
     * The guard page above the window is absent and stays absent. Its address
     * is one past the last byte the heap can ever hand out, so this is exactly
     * the write a caller running off the end of its last allocation would make.
     */
    if (paging_translate(HEAP_GUARD_ABOVE, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("the upper heap guard page is mapped");
    }

    console_write("ST INFO heap: guard write to ");
    console_write_hex(HEAP_GUARD_ABOVE);
    console_write(" expecting P=0 W=1 U=0\n");

    paging_probe_write(
        (volatile uint8_t *)(uintptr_t)HEAP_GUARD_ABOVE,
        HEAP_TEST_PATTERN
    );
}


/*
 * Read every register of every recorded function twice through the ports and
 * require the two passes to agree. Configuration reads are the foundation
 * everything above them is decoded from, so a reader that returns a different
 * answer for the same question makes every claim above it meaningless. This is
 * the check that would catch an address port left latched by something else.
 */
static void pci_reads_repeat(void)
{
    for (size_t index = 0; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function == NULL) {
            kernel_test_fail("PCI returned no function for a live index");
        }

        for (uint16_t offset = 0U;
             offset <= PCI_CONFIG_SPACE_SIZE - 4U;
             offset = (uint16_t)(offset + 4U)) {
            uint32_t first = 0U;
            uint32_t second = 0U;

            if (pci_config_read_port(function->address, offset, &first) !=
                    PCI_STATUS_OK ||
                pci_config_read_port(function->address, offset, &second) !=
                    PCI_STATUS_OK) {
                kernel_test_fail("a configuration read was refused");
            }

            if (first != second) {
                kernel_test_fail("a configuration register did not read twice");
            }
        }
    }
}

/* Every refusal both readers owe, driven against the live machine. */
static void pci_refusals_are_named(void)
{
    struct pci_address address;
    uint32_t value = 0U;

    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_port(address, 2U, &value) != PCI_STATUS_BAD_OFFSET) {
        kernel_test_fail("an unaligned configuration read was accepted");
    }

    if (pci_config_read_port(address, PCI_CONFIG_SPACE_SIZE, &value) !=
        PCI_STATUS_BAD_OFFSET) {
        kernel_test_fail("a configuration read past the space was accepted");
    }

    address.device = PCI_DEVICES_PER_BUS;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        kernel_test_fail("a device number out of range was accepted");
    }

    address.device = 0U;
    address.segment = 1U;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        kernel_test_fail("the ports accepted a segment they cannot carry");
    }

    if (pci_initialize(NULL, false) != PCI_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("PCI accepted a second initialization");
    }
}

/*
 * At least one address on bus zero must have nothing at it, and must read all
 * ones. Absence is how enumeration decides a device is not there, so a machine
 * where absence read as anything else would produce devices that do not exist -
 * and this is the only way to check that the floating bus behaves as the
 * specification says it does.
 */
static void pci_absence_reads_all_ones(void)
{
    struct pci_address address;
    bool found_absent = false;

    address.segment = 0U;
    address.bus = 0U;
    address.function = 0U;

    for (uint8_t device = 0; device < PCI_DEVICES_PER_BUS; ++device) {
        uint32_t identity = 0U;

        address.device = device;

        if (pci_config_read_port(address, PCI_REGISTER_VENDOR_ID, &identity) !=
            PCI_STATUS_OK) {
            kernel_test_fail("a configuration read was refused");
        }

        if ((uint16_t)identity != PCI_VENDOR_ABSENT) {
            continue;
        }

        found_absent = true;

        /*
         * Not just the vendor register: an absent function floats the whole
         * bus high, so every register of it must read all ones. A machine that
         * answered zero for the rest would let a decoder invent a device with
         * class zero at every empty slot.
         */
        for (uint16_t offset = 0U;
             offset <= PCI_CONFIG_SPACE_SIZE - 4U;
             offset = (uint16_t)(offset + 4U)) {
            uint32_t value = 0U;

            if (pci_config_read_port(address, offset, &value) !=
                PCI_STATUS_OK) {
                kernel_test_fail("a configuration read was refused");
            }

            if (value != UINT32_C(0xFFFFFFFF)) {
                kernel_test_fail("an absent function did not read all ones");
            }
        }
    }

    if (!found_absent) {
        kernel_test_fail("bus zero has no empty slot to prove absence with");
    }
}

/*
 * Enumeration through the I/O ports alone, on a machine that declares no
 * configuration window. This is the path every x86 machine has, so it is the
 * one that must work without any of the rest, and the scenario asserts the
 * window really is absent rather than merely unused.
 */
static void pci_scenario(const struct acpi_mcfg *mcfg, bool mcfg_present)
{
    const struct pci_function *host_bridge;
    const struct pci_function *network;
    struct pci_state pci;
    struct pci_address address;
    enum pci_status status;
    uint32_t value = 0U;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("PCI enumeration ran without its lower layers");
    }

    if (pci_is_initialized()) {
        kernel_test_fail("PCI was initialized before its scenario");
    }

    status = pci_initialize(mcfg, mcfg_present);

    if (status != PCI_STATUS_OK) {
        kernel_test_fail(pci_status_string(status));
    }

    pci = pci_get_state();

    if (pci.ecam_active) {
        kernel_test_fail("a configuration window was mapped on this machine");
    }

    /*
     * With no window the memory reader has nothing to answer with, and says so
     * by name rather than reading whatever is at address zero.
     */
    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_ecam(address, 0U, &value) != PCI_STATUS_NO_ECAM) {
        kernel_test_fail("the window reader answered without a window");
    }

    if (pci.compared_dwords != 0U || pci.compared_functions != 0U) {
        kernel_test_fail("a comparison was reported without two mechanisms");
    }

    if (pci.bus_count == 0U || pci.function_count == 0U) {
        kernel_test_fail("enumeration through the ports found nothing");
    }

    host_bridge = pci_find_class(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST_BRIDGE);

    if (host_bridge == NULL) {
        kernel_test_fail("enumeration found no host bridge");
    }

    if (host_bridge->address.bus != 0U || host_bridge->address.device != 0U ||
        host_bridge->address.function != 0U) {
        kernel_test_fail("the host bridge is not at 00:00.0");
    }

    if (host_bridge->vendor_id == PCI_VENDOR_ABSENT ||
        host_bridge->vendor_id == 0U) {
        kernel_test_fail("the host bridge has no vendor");
    }

    /*
     * The host bridge is the first function on this machine, so a lookup that
     * ignored its arguments entirely would still return it and still satisfy
     * every check above. That was a negative control that passed, so two more
     * are asked: a class that is present but is not the first function, and a
     * class that is not assigned at all and must therefore be found nowhere.
     */
    network = pci_find_class(PCI_CLASS_NETWORK, 0U);

    if (network == NULL || network->class_code != PCI_CLASS_NETWORK ||
        network->subclass != 0U || network == host_bridge) {
        kernel_test_fail("the class lookup did not find the network device");
    }

    if (pci_find_class(UINT8_C(0xFE), UINT8_C(0xFE)) != NULL) {
        kernel_test_fail("the class lookup found an unassigned class");
    }

    pci_absence_reads_all_ones();
    pci_reads_repeat();
    pci_refusals_are_named();

    if (pci_verify() != PCI_STATUS_OK) {
        kernel_test_fail("PCI enumeration no longer matches the machine");
    }

    console_write("ST PCI ports functions ");
    console_write_u64(pci.function_count);
    console_write(" buses ");
    console_write_u64(pci.bus_count);
    console_putc('\n');

    if (pci_shutdown() != PCI_STATUS_OK ||
        pci_shutdown() != PCI_STATUS_NOT_INITIALIZED) {
        kernel_test_fail("PCI would not release its function table");
    }
}

/*
 * The same machine read two completely different ways.
 *
 * The port pair and the mapped window share no code below this file: one is two
 * I/O instructions, the other a load from uncacheable memory whose address is
 * computed from a firmware table. They have no reason to agree about anything
 * unless both are addressing the function the enumeration believes they are, so
 * requiring them to agree register for register is a check on the bus number
 * arithmetic, the window's base, the mapping's cacheability and the firmware
 * description all at once.
 *
 * The scenario also requires them to *disagree* about different functions,
 * because a window reader whose device and function bits went nowhere would
 * agree with the ports about 00:00.0 and answer 00:00.0 for everything else.
 */
static void pci_ecam_scenario(const struct acpi_mcfg *mcfg, bool mcfg_present)
{
    struct pci_state pci;
    struct pci_address address;
    enum pci_status status;
    size_t bridges = 0U;
    size_t message_signalled = 0U;
    size_t behind_a_bridge = 0U;
    size_t distinct_identities = 0U;
    uint32_t first_identity = 0U;
    uint32_t value = 0U;

    if (!mcfg_present) {
        kernel_test_fail("this machine declares no configuration window");
    }

    status = pci_initialize(mcfg, mcfg_present);

    if (status != PCI_STATUS_OK) {
        kernel_test_fail(pci_status_string(status));
    }

    pci = pci_get_state();

    if (!pci.ecam_active) {
        kernel_test_fail("the declared configuration window was not mapped");
    }

    if (pci.ecam_size != PAGING_ECAM_WINDOW_SIZE ||
        pci.ecam_base != paging_get_state().ecam_window_base) {
        kernel_test_fail("the window read is not the window that was mapped");
    }

    /*
     * Configuration space read through a cached mapping would be answered from
     * whatever the line held when it was last filled. QEMU under TCG models no
     * cache, so a cached window behaves identically here and no behavioural
     * test can tell the difference - the negative control for it passed with
     * the mapping made write-back. What can be checked is the mapping itself,
     * so it is: every page of the window must translate as uncacheable, at 4 KiB
     * granularity, to the physical address it claims. That is the same move
     * paging.c makes when it walks its own tables rather than trusting them.
     */
    for (uint64_t offset = 0U; offset < pci.ecam_size;
         offset += PAGING_PAGE_SIZE) {
        struct paging_translation window;

        if (paging_translate(pci.ecam_base + offset, &window) !=
            PAGING_STATUS_OK) {
            kernel_test_fail("a configuration window page is not mapped");
        }

        if (window.physical_address != pci.ecam_base + offset ||
            window.level != 1U ||
            window.permissions != (PAGING_WRITE | PAGING_UNCACHED)) {
            kernel_test_fail("the configuration window is not device memory");
        }
    }

    /*
     * Every function on this machine sits inside the mapped window, so every
     * one of them must have been compared. A comparison that quietly skipped
     * functions would still report agreement.
     */
    if (pci.compared_functions != pci.function_count ||
        pci.compared_dwords !=
            pci.function_count * (PCI_CONFIG_SPACE_SIZE / 4U) -
                pci.volatile_dwords) {
        kernel_test_fail("the two mechanisms were not compared everywhere");
    }

    for (size_t index = 0; index < pci.function_count; ++index) {
        const struct pci_function *function = pci_function_at(index);
        uint32_t identity = 0U;

        if (function == NULL) {
            kernel_test_fail("PCI returned no function for a live index");
        }

        if (function->header_type == PCI_HEADER_TYPE_BRIDGE) {
            ++bridges;
        }

        if (function->address.bus != 0U) {
            ++behind_a_bridge;
        }

        if (function->msi_x_offset != 0U) {
            ++message_signalled;

            /*
             * The capability the offset names must actually be there when the
             * window is asked, not only when the ports were. This is what turns
             * "a capability was recorded" into "the record points at it".
             */
            if (pci_config_read_ecam(
                    function->address,
                    function->msi_x_offset,
                    &value
                ) != PCI_STATUS_OK ||
                (uint8_t)value != PCI_CAPABILITY_MSI_X) {
                kernel_test_fail("a recorded MSI-X capability is not there");
            }
        }

        if (pci_config_read_ecam(
                function->address,
                PCI_REGISTER_VENDOR_ID,
                &identity
            ) != PCI_STATUS_OK) {
            kernel_test_fail("the window would not read a live function");
        }

        if (index == 0U) {
            first_identity = identity;
        } else if (identity != first_identity) {
            ++distinct_identities;
        }
    }

    /*
     * A window reader that ignored the device and function bits would answer
     * the same identity for every function and still agree with the ports about
     * the first one. Requiring the answers to differ is what closes that.
     */
    if (distinct_identities == 0U) {
        kernel_test_fail("the window answered one identity for every function");
    }

    if (bridges == 0U || behind_a_bridge == 0U || pci.bus_count < 2U) {
        kernel_test_fail("enumeration did not cross a bridge");
    }

    if (message_signalled == 0U) {
        kernel_test_fail("no function offered message-signalled interrupts");
    }

    /*
     * A bus past what Seneri mapped is refused rather than folded back into the
     * window, which is the failure that would read one bus as another.
     */
    address.segment = 0U;
    address.bus = (uint8_t)(pci.ecam_end_bus + 1U);
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_ecam(address, 0U, &value) !=
        PCI_STATUS_OUTSIDE_ECAM_WINDOW) {
        kernel_test_fail("the window answered about a bus it does not map");
    }

    pci_reads_repeat();

    if (pci_verify() != PCI_STATUS_OK) {
        kernel_test_fail("PCI enumeration no longer matches the machine");
    }

    console_write("ST PCI window agreed on ");
    console_write_u64(pci.compared_dwords);
    console_write(" registers of ");
    console_write_u64(pci.compared_functions);
    console_write(" functions across ");
    console_write_u64(pci.bus_count);
    console_write(" buses, ");
    console_write_u64(message_signalled);
    console_write(" with MSI-X\n");

    if (pci_shutdown() != PCI_STATUS_OK) {
        kernel_test_fail("PCI would not release its function table");
    }
}


/*
 * The guard page of the first thread this scenario creates. Slot zero belongs
 * to the boot thread, whose stack region is never mapped, so a created thread
 * is slot one and its guard is one stride above the region base. Written out
 * rather than computed so the Makefile can require this exact address in the
 * fault diagnostic - a guard that moved would otherwise still look like a pass.
 */
#define THREAD_GUARD_TEST_ADDRESS \
    (THREAD_STACK_REGION + THREAD_STACK_STRIDE)

/*
 * A supervisor write to an absent page is P=0 W=1 U=0. The heap scenario takes
 * the same error code at its own guard, and the two are told apart by CR2.
 */
#define THREAD_GUARD_TEST_ERROR_CODE UINT64_C(0x02)

_Static_assert(
    THREAD_GUARD_TEST_ADDRESS == UINT64_C(0x0000000800005000),
    "the thread guard page moved and the fault diagnostic no longer matches"
);

/* Written by every scenario worker, read by the boot thread after they exit. */
static volatile uint32_t thread_scenario_ran;
static volatile uint64_t thread_scenario_seen[THREAD_MAX];

static void scenario_worker(void *context)
{
    const uint64_t label = (uint64_t)(uintptr_t)context;

    for (unsigned int round = 0; round < 2U; ++round) {
        /*
         * Recorded from inside the thread, so this is also a check that
         * thread_current answers about the thread that is actually running
         * rather than about whoever asked last.
         */
        if (label < THREAD_MAX) {
            thread_scenario_seen[label] = thread_current();
        }

        thread_yield();
    }

    thread_scenario_ran += 1U;
}

/*
 * The parts of the thread layer normal boot does not reach: the capacity bound,
 * every refusal, and the teardown ordering. Normal boot proves three threads
 * rotate; this proves the layer says no in every way it has to.
 */
static void threads_scenario(void)
{
    struct frame_allocator_stats before;
    struct frame_allocator_stats after;
    struct thread_system_state threads;
    struct thread_state_report report;
    uint64_t identifiers[THREAD_MAX];
    uint64_t overflow = THREAD_ID_NONE;
    uint64_t boot_identifier;
    size_t created = 0U;
    enum thread_status status;

    if (!heap_is_active() || !paging_is_active()) {
        kernel_test_fail("threads ran without their lower layers");
    }

    if (thread_is_started()) {
        kernel_test_fail("threads were started before their scenario");
    }

    before = frame_allocator_get_stats();
    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    boot_identifier = thread_current();

    if (boot_identifier == THREAD_ID_NONE) {
        kernel_test_fail("the boot thread was adopted without an identifier");
    }

    if (thread_report(boot_identifier, &report) != THREAD_STATUS_OK ||
        !report.boot_thread || report.state != THREAD_STATE_RUNNING ||
        report.stack_top != 0U) {
        kernel_test_fail("the boot thread was not adopted as itself");
    }

    /*
     * Fill every slot the table has. Slot zero is the boot thread's and is
     * never handed out, so the capacity for created threads is one less than
     * the table's - and the refusal must arrive exactly there rather than one
     * either side of it.
     */
    for (size_t index = 0; index + 1U < THREAD_MAX; ++index) {
        status = thread_create(
            scenario_worker,
            (void *)(uintptr_t)(index + 1U),
            &identifiers[index]
        );

        if (status != THREAD_STATUS_OK) {
            kernel_test_fail(thread_status_string(status));
        }

        ++created;
    }

    if (created != THREAD_MAX - 1U) {
        kernel_test_fail("the thread table did not fill to its capacity");
    }

    if (thread_create(scenario_worker, NULL, &overflow) !=
            THREAD_STATUS_NO_CAPACITY ||
        overflow != THREAD_ID_NONE) {
        kernel_test_fail("a thread was created past the table's capacity");
    }

    /* Every identifier must be distinct, or joining names the wrong thread. */
    for (size_t index = 0; index < created; ++index) {
        for (size_t other = index + 1U; other < created; ++other) {
            if (identifiers[index] == identifiers[other]) {
                kernel_test_fail("two threads share an identifier");
            }
        }

        if (identifiers[index] == boot_identifier) {
            kernel_test_fail("a thread reused the boot thread's identifier");
        }
    }

    /* Refusals, each by its own name, with threads live. */
    if (thread_join(boot_identifier) != THREAD_STATUS_BAD_IDENTIFIER) {
        kernel_test_fail("a thread was allowed to wait for itself");
    }

    if (thread_join(UINT64_C(0xABCDEF)) != THREAD_STATUS_BAD_IDENTIFIER ||
        thread_join(THREAD_ID_NONE) != THREAD_STATUS_BAD_IDENTIFIER) {
        kernel_test_fail("a join accepted an identifier naming nothing");
    }

    /*
     * Tearing down while a thread is still runnable would unmap a stack that
     * still holds a suspended frame. It is refused, and refused before any of
     * it happens rather than part way through.
     */
    if (thread_stop() != THREAD_STATUS_THREADS_STILL_RUNNABLE) {
        kernel_test_fail("threads stopped with a thread still runnable");
    }

    threads = thread_get_state();

    if (threads.ready != created || threads.stack_frames !=
        created * THREAD_STACK_PAGES) {
        kernel_test_fail("the thread table does not account for its stacks");
    }

    if (thread_verify() != THREAD_STATUS_OK) {
        kernel_test_fail("thread table does not match the address space");
    }

    for (size_t index = 0; index < created; ++index) {
        status = thread_join(identifiers[index]);

        if (status != THREAD_STATUS_OK) {
            kernel_test_fail(thread_status_string(status));
        }

        if (thread_report(identifiers[index], &report) != THREAD_STATUS_OK ||
            report.state != THREAD_STATE_EXITED) {
            kernel_test_fail("a joined thread had not exited");
        }

        /* Joining something that has already exited returns at once. */
        if (thread_join(identifiers[index]) != THREAD_STATUS_OK) {
            kernel_test_fail("joining an exited thread was refused");
        }
    }

    if (thread_scenario_ran != created) {
        kernel_test_fail("not every thread reached the end of its function");
    }

    /*
     * Each worker recorded what thread_current answered while it was running.
     * A scheduler that switched stacks but not its idea of who is current would
     * pass every other check here.
     */
    for (size_t index = 0; index < created; ++index) {
        if (thread_scenario_seen[index + 1U] != identifiers[index]) {
            kernel_test_fail("a thread did not know which thread it was");
        }
    }

    threads = thread_get_state();

    if (threads.exited != created || threads.ready != 0U ||
        threads.switches == 0U) {
        kernel_test_fail("the thread table does not account for its exits");
    }

    status = thread_stop();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    if (thread_is_started() ||
        thread_stop() != THREAD_STATUS_NOT_STARTED ||
        thread_create(scenario_worker, NULL, &overflow) !=
            THREAD_STATUS_NOT_STARTED) {
        kernel_test_fail("threads accepted work after stopping");
    }

    after = frame_allocator_get_stats();

    if (after.free_frames != before.free_frames) {
        kernel_test_fail("stopping threads did not return every frame");
    }

    /*
     * The stacks are gone, so every guard and every stack page of every slot
     * must now be absent. Checked after teardown because a stop that unmapped
     * the guards but kept the stacks would leak silently.
     */
    for (size_t slot = 0; slot < THREAD_MAX; ++slot) {
        const uint64_t guard =
            THREAD_STACK_REGION + (uint64_t)slot * THREAD_STACK_STRIDE;
        struct paging_translation translation;

        for (uint64_t offset = 0U; offset < THREAD_STACK_STRIDE;
             offset += PAGING_PAGE_SIZE) {
            if (paging_translate(guard + offset, &translation) !=
                PAGING_STATUS_NOT_MAPPED) {
                kernel_test_fail("a thread stack outlived the scheduler");
            }
        }
    }

    console_write("ST THREADS created ");
    console_write_u64(created);
    console_write(" switches ");
    console_write_u64(threads.switches);
    console_write(" exited ");
    console_write_u64(threads.exited);
    console_putc('\n');
}

static void guard_worker(void *context)
{
    struct thread_state_report report;

    (void)context;

    if (thread_report(thread_current(), &report) != THREAD_STATUS_OK) {
        kernel_test_fail("a running thread cannot report itself");
    }

    if (report.guard_page != THREAD_GUARD_TEST_ADDRESS) {
        kernel_test_fail("the thread guard page is not where it should be");
    }

    if (report.stack_base != report.guard_page + PAGING_PAGE_SIZE) {
        kernel_test_fail("the guard page does not precede the stack");
    }

    /*
     * Written through the assembly probe for the same reason the paging and
     * heap scenarios use it: the scenario matches the faulting instruction
     * address exactly, and a compiler is free to move or delete an equivalent
     * C store to memory it can prove nothing reads.
     */
    paging_probe_write(
        (volatile uint8_t *)(uintptr_t)THREAD_GUARD_TEST_ADDRESS,
        UINT8_C(0x5E)
    );

    kernel_test_fail("a thread stack guard page accepted a write");
}

/*
 * Prove the guard by walking off the stack, exactly as the heap scenario proves
 * its window. The claim is that the page below a thread's stack is never
 * mapped, so a stack that runs past its own end meets a fault naming the guard
 * rather than the next thread's frame.
 *
 * What this deliberately does *not* prove is a true stack overflow, where RSP
 * itself has reached the guard. There the fault handler would need to push its
 * own frame onto a stack that has just run out, so the page fault escalates.
 * docs/THREADS.md records what was measured about that and what it needs.
 */
static void thread_guard_scenario(void)
{
    uint64_t identifier = THREAD_ID_NONE;
    struct paging_translation translation;
    enum thread_status status;

    status = thread_start();

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    status = thread_create(guard_worker, NULL, &identifier);

    if (status != THREAD_STATUS_OK) {
        kernel_test_fail(thread_status_string(status));
    }

    /*
     * The guard must be absent before the thread runs, or the fault the
     * scenario is about to take would prove nothing about the guard.
     */
    if (paging_translate(THREAD_GUARD_TEST_ADDRESS, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("the thread guard page is mapped");
    }

    if (paging_translate(THREAD_GUARD_TEST_ADDRESS + PAGING_PAGE_SIZE,
            &translation) != PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE) {
        kernel_test_fail("the thread stack above the guard is not writable");
    }

    console_write("ST THREAD guard ");
    console_write_hex(THREAD_GUARD_TEST_ADDRESS);
    console_putc('\n');

    /* Hands the processor to the worker, which does not come back. */
    thread_yield();
    kernel_test_fail("the guard thread returned from its fault");
}


/*
 * Sixteen coordinates chosen to hit every edge and a few interior points,
 * rather than a sweep. Normal boot already reads every pixel back through the
 * framebuffer's own addressing; what this scenario adds is a second, completely
 * independent addressing to compare it against, and that only needs to
 * disagree once.
 */
#define FRAMEBUFFER_TEST_PROBES 16U

/*
 * The same picture addressed two ways.
 *
 * Normal boot proves that what framebuffer_write_pixel writes,
 * framebuffer_read_pixel reads - which is true even if both agree on the wrong
 * address. This computes the physical address of a coordinate from the loader's
 * own numbers, reads it through a raw volatile pointer that shares no code with
 * framebuffer.c, and requires the two to agree. It is the same argument the two
 * PCI configuration mechanisms make, one layer up.
 */
static void framebuffer_scenario(const struct boot_framebuffer *framebuffer)
{
    struct framebuffer_state screen;
    uint32_t coordinates[FRAMEBUFFER_TEST_PROBES][2];
    uint32_t mask;
    size_t probes = 0U;
    enum framebuffer_status status;

    if (framebuffer == NULL || !framebuffer->present) {
        kernel_test_fail("the boot loader set no framebuffer");
    }

    if (!paging_is_active()) {
        kernel_test_fail("the framebuffer scenario ran before paging");
    }

    status = framebuffer_initialize(framebuffer);

    if (status != FRAMEBUFFER_STATUS_OK) {
        kernel_test_fail(framebuffer_status_string(status));
    }

    screen = framebuffer_get_state();
    mask = framebuffer_visible_mask();

    if (screen.width < 4U || screen.height < 4U) {
        kernel_test_fail("the framebuffer is too small to probe");
    }

    /* Four corners, four edge midpoints, and the rest spread through it. */
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = screen.width / 2U;
    coordinates[probes++][1] = 0U;
    coordinates[probes][0] = screen.width / 2U;
    coordinates[probes++][1] = screen.height - 1U;
    coordinates[probes][0] = 0U;
    coordinates[probes++][1] = screen.height / 2U;
    coordinates[probes][0] = screen.width - 1U;
    coordinates[probes++][1] = screen.height / 2U;

    while (probes < FRAMEBUFFER_TEST_PROBES) {
        coordinates[probes][0] =
            (uint32_t)(probes * 37U) % screen.width;
        coordinates[probes][1] =
            (uint32_t)(probes * 53U) % screen.height;
        ++probes;
    }

    /*
     * A distinct colour per probe, so a coordinate that aliases another shows
     * up as the wrong colour rather than as a coincidence.
     */
    for (size_t index = 0; index < probes; ++index) {
        const uint32_t colour = framebuffer_pack(
            (uint8_t)(index * 7U + 1U),
            (uint8_t)(index * 11U + 2U),
            (uint8_t)(index * 13U + 3U)
        );

        if (framebuffer_write_pixel(coordinates[index][0],
                coordinates[index][1], colour) != FRAMEBUFFER_STATUS_OK) {
            kernel_test_fail("the framebuffer refused a visible pixel");
        }
    }

    for (size_t index = 0; index < probes; ++index) {
        const uint32_t x = coordinates[index][0];
        const uint32_t y = coordinates[index][1];
        const uint32_t colour = framebuffer_pack(
            (uint8_t)(index * 7U + 1U),
            (uint8_t)(index * 11U + 2U),
            (uint8_t)(index * 13U + 3U)
        );
        /*
         * Computed here from the loader's own pitch, not from framebuffer.c.
         * If this file and that one disagree about where a pixel lives, this is
         * where it surfaces.
         */
        const volatile uint32_t *raw = (const volatile uint32_t *)(uintptr_t)(
            framebuffer->address +
            (uint64_t)y * framebuffer->pitch +
            (uint64_t)x * FRAMEBUFFER_BYTES_PER_PIXEL);
        uint32_t through_api = 0U;

        if (framebuffer_read_pixel(x, y, &through_api) !=
            FRAMEBUFFER_STATUS_OK) {
            kernel_test_fail("the framebuffer refused a visible pixel");
        }

        if ((through_api & mask) != (colour & mask)) {
            kernel_test_fail("a framebuffer pixel did not hold its colour");
        }

        if ((*raw & mask) != (colour & mask)) {
            kernel_test_fail("the framebuffer wrote a pixel somewhere else");
        }
    }

    /*
     * No two probes may share an address. A pitch read as a width collapses
     * rows onto each other, which every single-pixel check above would survive.
     */
    for (size_t left = 0; left < probes; ++left) {
        for (size_t right = left + 1U; right < probes; ++right) {
            const uint64_t first =
                (uint64_t)coordinates[left][1] * framebuffer->pitch +
                (uint64_t)coordinates[left][0] * FRAMEBUFFER_BYTES_PER_PIXEL;
            const uint64_t second =
                (uint64_t)coordinates[right][1] * framebuffer->pitch +
                (uint64_t)coordinates[right][0] * FRAMEBUFFER_BYTES_PER_PIXEL;

            if (coordinates[left][0] == coordinates[right][0] &&
                coordinates[left][1] == coordinates[right][1]) {
                continue;
            }

            if (first == second) {
                kernel_test_fail("two framebuffer coordinates share an address");
            }
        }
    }

    /* The last visible pixel must still be inside the mapped span. */
    if ((uint64_t)(screen.height - 1U) * screen.pitch +
            (uint64_t)(screen.width - 1U) * FRAMEBUFFER_BYTES_PER_PIXEL +
            FRAMEBUFFER_BYTES_PER_PIXEL > screen.size) {
        kernel_test_fail("the last pixel lies outside the framebuffer");
    }

    if (framebuffer_write_pixel(screen.width, 0U, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS ||
        framebuffer_write_pixel(0U, screen.height, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS ||
        framebuffer_write_pixel(UINT32_MAX, UINT32_MAX, 0U) !=
            FRAMEBUFFER_STATUS_OUT_OF_BOUNDS) {
        kernel_test_fail("the framebuffer accepted a pixel off the screen");
    }

    status = framebuffer_verify();

    if (status != FRAMEBUFFER_STATUS_OK) {
        kernel_test_fail(framebuffer_status_string(status));
    }

    console_write("ST FRAMEBUFFER ");
    console_write_u64(screen.width);
    console_putc('x');
    console_write_u64(screen.height);
    console_write(" probes ");
    console_write_u64(probes);
    console_write(" pitch ");
    console_write_u64(screen.pitch);
    console_putc('\n');
}

void kernel_test_run(
    enum kernel_test_scenario scenario,
    const struct acpi_mcfg *mcfg,
    bool mcfg_present,
    const struct boot_framebuffer *framebuffer
)
{
    enum pit_status pit_status;

    if (scenario == KERNEL_TEST_NONE) {
        return;
    }

    active_scenario = scenario;
    test_marker("BEGIN", scenario);

    if (!interrupt_frame_layout_self_test()) {
        kernel_test_fail("interrupt frame or descriptor validation failed");
    }

    switch (scenario) {
    case KERNEL_TEST_NORMAL:
        return;
    case KERNEL_TEST_BREAKPOINT:
        if (!interrupt_breakpoint_self_test()) {
            kernel_test_fail("breakpoint register restoration failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_INVALID_OPCODE:
        interrupt_trigger_invalid_opcode();
    case KERNEL_TEST_PAGE_FAULT:
        interrupt_trigger_page_fault();
    case KERNEL_TEST_IST:
        if (!interrupt_ist_self_test()) {
            kernel_test_fail("IST routing proof failed");
        }
        kernel_test_pass();
    case KERNEL_TEST_PIT:
        pit_status = pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_LEGACY_PIC);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        pit_status = pit_wait_for_ticks(PIT_TEST_TICKS);

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        if (pit_ticks() < PIT_TEST_TICKS) {
            kernel_test_fail("PIT delivered too few ticks");
        }

        pit_status = pit_stop();

        if (pit_status != PIT_STATUS_OK) {
            kernel_test_fail(pit_status_string(pit_status));
        }

        kernel_test_pass();
    case KERNEL_TEST_UNEXPECTED:
        interrupt_trigger_unexpected();
    case KERNEL_TEST_APIC:
        apic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_IOAPIC:
        ioapic_scenario();
        kernel_test_pass();
    case KERNEL_TEST_RETIRED:
        retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_APIC_TIMER:
        apic_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TSC:
        tsc_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PM_TIMER:
        pm_timer_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PIT_RETIRED:
        pit_retired_scenario();
        kernel_test_pass();
    case KERNEL_TEST_TIMERS:
        timers_scenario();
        kernel_test_pass();
    case KERNEL_TEST_PAGING:
        paging_scenario();
        kernel_test_fail("a read-only page accepted a supervisor write");
    case KERNEL_TEST_HEAP:
        heap_scenario();
        kernel_test_fail("a heap guard page accepted a supervisor write");
    case KERNEL_TEST_PCI:
        pci_scenario(mcfg, mcfg_present);
        kernel_test_pass();
    case KERNEL_TEST_PCI_ECAM:
        pci_ecam_scenario(mcfg, mcfg_present);
        kernel_test_pass();
    case KERNEL_TEST_THREADS:
        threads_scenario();
        kernel_test_pass();
    case KERNEL_TEST_THREAD_GUARD:
        thread_guard_scenario();
        kernel_test_fail("a thread stack guard page accepted a write");
    case KERNEL_TEST_FRAMEBUFFER:
        framebuffer_scenario(framebuffer);
        kernel_test_pass();
    case KERNEL_TEST_DOUBLE_FAULT:
        kernel_test_double_fault_armed = 1U;
        interrupt_test_set_gate_present(14U, false);
        interrupt_trigger_page_fault();
    case KERNEL_TEST_INVALID:
        kernel_test_fail("invalid or duplicate seneri.test argument");
    case KERNEL_TEST_NONE:
    default:
        kernel_test_fail("unreachable test scenario");
    }
}

_Noreturn void kernel_test_complete_normal(void)
{
    if (active_scenario != KERNEL_TEST_NORMAL) {
        kernel_test_fail("normal completion used outside the normal scenario");
    }

    kernel_test_pass();
}

bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame)
{
    bool matches = false;

    if (frame == NULL) {
        return false;
    }

    switch (active_scenario) {
    case KERNEL_TEST_INVALID_OPCODE:
        matches = frame->vector == 6U &&
            frame->error_code == 0U &&
            frame->rip == (uintptr_t)(const void *)interrupt_invalid_opcode_site;
        break;
    case KERNEL_TEST_PAGE_FAULT:
        matches = frame->vector == 14U &&
            frame->error_code == 0U &&
            frame->cr2 == PAGE_FAULT_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)interrupt_page_fault_site;
        break;
    case KERNEL_TEST_UNEXPECTED:
        matches = frame->vector == UINT64_C(0x80) && frame->error_code == 0U;
        break;
    case KERNEL_TEST_PAGING:
        matches = frame->vector == 14U &&
            frame->error_code == PAGING_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == PAGING_PROBE_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    case KERNEL_TEST_HEAP:
        matches = frame->vector == 14U &&
            frame->error_code == HEAP_TEST_FAULT_ERROR_CODE &&
            frame->cr2 == HEAP_GUARD_ABOVE &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    case KERNEL_TEST_THREAD_GUARD:
        /*
         * The fault is taken on the created thread's own stack, so this also
         * proves the fault path works at all on a stack this layer allocated
         * rather than only on the one boot.S set up.
         */
        matches = frame->vector == 14U &&
            frame->error_code == THREAD_GUARD_TEST_ERROR_CODE &&
            frame->cr2 == THREAD_GUARD_TEST_ADDRESS &&
            frame->rip == (uintptr_t)(const void *)paging_probe_write_site;
        break;
    default:
        return false;
    }

    if (!matches) {
        kernel_test_fail("fatal interrupt did not match its expectation");
    }

    kernel_test_pass();
}

const char *kernel_test_scenario_name(enum kernel_test_scenario scenario)
{
    switch (scenario) {
    case KERNEL_TEST_NONE:
        return "none";
    case KERNEL_TEST_NORMAL:
        return "normal";
    case KERNEL_TEST_BREAKPOINT:
        return "breakpoint";
    case KERNEL_TEST_INVALID_OPCODE:
        return "invalid-opcode";
    case KERNEL_TEST_PAGE_FAULT:
        return "page-fault";
    case KERNEL_TEST_IST:
        return "ist";
    case KERNEL_TEST_PIT:
        return "pit";
    case KERNEL_TEST_UNEXPECTED:
        return "unexpected";
    case KERNEL_TEST_DOUBLE_FAULT:
        return "double-fault";
    case KERNEL_TEST_APIC:
        return "apic";
    case KERNEL_TEST_IOAPIC:
        return "ioapic";
    case KERNEL_TEST_RETIRED:
        return "retired";
    case KERNEL_TEST_APIC_TIMER:
        return "apic-timer";
    case KERNEL_TEST_TSC:
        return "tsc";
    case KERNEL_TEST_PM_TIMER:
        return "pm-timer";
    case KERNEL_TEST_PIT_RETIRED:
        return "pit-retired";
    case KERNEL_TEST_TIMERS:
        return "timers";
    case KERNEL_TEST_PAGING:
        return "paging";
    case KERNEL_TEST_HEAP:
        return "heap";
    case KERNEL_TEST_PCI:
        return "pci";
    case KERNEL_TEST_PCI_ECAM:
        return "pci-ecam";
    case KERNEL_TEST_THREADS:
        return "threads";
    case KERNEL_TEST_THREAD_GUARD:
        return "thread-guard";
    case KERNEL_TEST_FRAMEBUFFER:
        return "framebuffer";
    case KERNEL_TEST_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

_Noreturn void kernel_test_fail(const char *reason)
{
    console_write("ST FAIL ");
    console_write(kernel_test_scenario_name(active_scenario));
    console_write(": ");
    console_write(reason);
    console_putc('\n');
    cpu_out32(QEMU_EXIT_PORT, QEMU_FAILURE_VALUE);
    console_halt();
}
