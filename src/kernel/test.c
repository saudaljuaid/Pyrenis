/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/acpi.h>
#include <sapote/acpi_util.h>
#include <sapote/apic.h>
#include <sapote/apic_timer.h>
#include <sapote/boot_ledger.h>
#include <sapote/boot_plan.h>
#include <sapote/clock.h>
#include <sapote/console.h>
#include <sapote/cpu.h>
#include <sapote/device_substrate.h>
#include <sapote/framebuffer.h>
#include <sapote/filesystem.h>
#include <sapote/font.h>
#include <sapote/heap.h>
#include <sapote/interrupts.h>
#include <sapote/ioapic.h>
#include <sapote/memory.h>
#include <sapote/nvme.h>
#include <sapote/paging.h>
#include <sapote/pci.h>
#include <sapote/pic.h>
#include <sapote/pit.h>
#include <sapote/pointer.h>
#include <sapote/process.h>
#include <sapote/keyboard.h>
#include <sapote/linux_abi.h>
#include <sapote/screen.h>
#include <sapote/shell.h>
#include <sapote/pm_timer.h>
#include <sapote/surface.h>
#include <sapote/test.h>
#include <sapote/thread.h>
#include <sapote/timer.h>
#include <sapote/tsc.h>
#include <sapote/ui.h>
#include <sapote/ui_font.h>
#include <sapote/xhci.h>

#define QEMU_EXIT_PORT UINT16_C(0x00F4)
#define QEMU_FAILURE_VALUE UINT8_C(0x7F)
#define PAGE_FAULT_TEST_ADDRESS UINT64_C(0x0000000100000000)
#define PIT_TEST_FREQUENCY UINT32_C(100)
#define PIT_TEST_TICKS UINT64_C(8)
#define APIC_TIMER_TEST_FREQUENCY UINT32_C(100)
#define APIC_TIMER_TEST_TICKS UINT64_C(20)
#define TSC_MONOTONIC_READS 64U

/*
 * Eight level-triggered deliveries at 100 Hz. The failure this scenario exists
 * to catch is a pin that delivers once and stops, so one delivery would prove
 * nothing; eight of them cannot happen by accident. The bound is 25 times the
 * 80 ms they should take, so a line that dies is a named status rather than a
 * hang, and it stays well inside one wrap of the reference counter.
 */
#define IOAPIC_LEVEL_TEST_TICKS UINT64_C(8)
#define IOAPIC_LEVEL_TEST_BOUND_NS UINT64_C(2000000000)

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

    if (token_equals(value, length, "ioapic-level")) {
        return KERNEL_TEST_IOAPIC_LEVEL;
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

    if (token_equals(value, length, "shell")) {
        return KERNEL_TEST_SHELL;
    }

    if (token_equals(value, length, "keyboard")) {
        return KERNEL_TEST_KEYBOARD;
    }

    if (token_equals(value, length, "screen")) {
        return KERNEL_TEST_SCREEN;
    }

    if (token_equals(value, length, "framebuffer")) {
        return KERNEL_TEST_FRAMEBUFFER;
    }

    if (token_equals(value, length, "surface")) {
        return KERNEL_TEST_SURFACE;
    }

    if (token_equals(value, length, "write-combining")) {
        return KERNEL_TEST_WRITE_COMBINING;
    }

    if (token_equals(value, length, "device-windows")) {
        return KERNEL_TEST_DEVICE_WINDOWS;
    }

    if (token_equals(value, length, "boot-ledger")) {
        return KERNEL_TEST_BOOT_LEDGER;
    }

    if (token_equals(value, length, "first-light")) {
        return KERNEL_TEST_FIRST_LIGHT;
    }

    if (token_equals(value, length, "device-substrate")) {
        return KERNEL_TEST_DEVICE_SUBSTRATE;
    }

    if (token_equals(value, length, "xhci")) {
        return KERNEL_TEST_XHCI;
    }

    if (token_equals(value, length, "nvme")) {
        return KERNEL_TEST_NVME;
    }

    if (token_equals(value, length, "filesystem")) {
        return KERNEL_TEST_FILESYSTEM;
    }

    if (token_equals(value, length, "process")) {
        return KERNEL_TEST_PROCESS;
    }

    if (token_equals(value, length, "linux-abi")) {
        return KERNEL_TEST_LINUX_ABI;
    }

    return KERNEL_TEST_INVALID;
}

/*
 * The value each scenario hands to QEMU's debug exit device, which the Makefile
 * turns into the process status it requires. They are deliberately dense and
 * deliberately stable: a scenario that took another's value would pass as that
 * one.
 *
 * 0x22 belongs to ioapic-level. The scenarios added after it start at 0x23 so
 * every exit value remains stable across this integration.
 */
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
    case KERNEL_TEST_IOAPIC_LEVEL:
        return UINT8_C(0x22);
    case KERNEL_TEST_PCI:
        return UINT8_C(0x23);
    case KERNEL_TEST_PCI_ECAM:
        return UINT8_C(0x24);
    case KERNEL_TEST_THREADS:
        return UINT8_C(0x25);
    case KERNEL_TEST_THREAD_GUARD:
        return UINT8_C(0x26);
    case KERNEL_TEST_FRAMEBUFFER:
        return UINT8_C(0x27);
    case KERNEL_TEST_SCREEN:
        return UINT8_C(0x28);
    case KERNEL_TEST_KEYBOARD:
        return UINT8_C(0x29);
    case KERNEL_TEST_SHELL:
        return UINT8_C(0x2A);
    case KERNEL_TEST_SURFACE:
        return UINT8_C(0x2B);
    case KERNEL_TEST_WRITE_COMBINING:
        return UINT8_C(0x2C);
    case KERNEL_TEST_DEVICE_WINDOWS:
        return UINT8_C(0x2D);
    case KERNEL_TEST_BOOT_LEDGER:
        return UINT8_C(0x2E);
    case KERNEL_TEST_FIRST_LIGHT:
        return UINT8_C(0x2F);
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        return UINT8_C(0x30);
    case KERNEL_TEST_XHCI:
        return UINT8_C(0x31);
    case KERNEL_TEST_NVME:
        return UINT8_C(0x32);
    case KERNEL_TEST_FILESYSTEM:
        return UINT8_C(0x33);
    case KERNEL_TEST_PROCESS:
        return UINT8_C(0x34);
    case KERNEL_TEST_LINUX_ABI:
        return UINT8_C(0x36);
    default:
        return QEMU_FAILURE_VALUE;
    }
}

static bool device_substrate_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x30);
}

bool kernel_test_device_substrate_exit_self_test(void)
{
    return device_substrate_exit_contract(
            scenario_exit_value(KERNEL_TEST_DEVICE_SUBSTRATE)) &&
        !device_substrate_exit_contract(UINT8_C(0x32));
}

static bool xhci_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x31);
}

bool kernel_test_xhci_exit_self_test(void)
{
    return xhci_exit_contract(scenario_exit_value(KERNEL_TEST_XHCI)) &&
        !xhci_exit_contract(UINT8_C(0x32));
}

static bool nvme_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x32);
}

bool kernel_test_nvme_exit_self_test(void)
{
    return nvme_exit_contract(scenario_exit_value(KERNEL_TEST_NVME)) &&
        !nvme_exit_contract(UINT8_C(0x33));
}

static bool filesystem_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x33);
}

bool kernel_test_filesystem_exit_self_test(void)
{
    return filesystem_exit_contract(
            scenario_exit_value(KERNEL_TEST_FILESYSTEM)) &&
        !filesystem_exit_contract(UINT8_C(0x34));
}

static bool process_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x34);
}

bool kernel_test_process_exit_self_test(void)
{
    return process_exit_contract(scenario_exit_value(KERNEL_TEST_PROCESS)) &&
        !process_exit_contract(UINT8_C(0x33));
}

static bool linux_abi_exit_contract(uint8_t value)
{
    return value == UINT8_C(0x36);
}

bool kernel_test_linux_abi_exit_self_test(void)
{
    return linux_abi_exit_contract(
            scenario_exit_value(KERNEL_TEST_LINUX_ABI)) &&
        !linux_abi_exit_contract(UINT8_C(0x35)) &&
        !linux_abi_exit_contract(UINT8_C(0x34));
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

enum kernel_test_scenario kernel_test_select(
    const struct boot_information *context
)
{
    static const char prefix[] = "sapote.test=";
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
 * Prove a level-triggered redirection entry delivers more than once.
 *
 * Every other route in this kernel is edge triggered, and an edge needs no
 * acknowledgement at the I/O APIC: the pin is sampled on a transition and
 * nothing is latched. A level-triggered entry latches remote IRR when it
 * delivers and cannot deliver again until an end of interrupt directed at the
 * I/O APIC clears it, so the failure worth hunting is a line that fires exactly
 * once and then goes quiet. One delivery cannot tell that apart from success,
 * which is why this counts eight.
 *
 * The opposite failure is just as silent. Acknowledging a pin whose source is
 * still asserting re-delivers immediately, so a route that never quiets its
 * device counts its eight interrupts in microseconds and looks perfect. The
 * scenario therefore measures how long the eight took as well as that they
 * arrived, and holds it to the interval eight ticks of a 100 Hz timer take.
 */
static void ioapic_level_scenario(void)
{
    struct ioapic_redirection entry;
    struct ioapic_state before;
    struct ioapic_state after;
    uint64_t elapsed_ns = 0U;
    const uint64_t expected_ns = IOAPIC_LEVEL_TEST_TICKS * UINT64_C(1000000000) /
        PIT_TEST_FREQUENCY;

    if (!ioapic_is_initialized() || ioapic_get_state().count == 0U) {
        kernel_test_fail("I/O APIC is not initialized");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("a legacy PIC line was left unmasked");
    }

    /* Nothing is routed yet, so every acknowledgement is refused by name. */
    if (ioapic_send_eoi(INTERRUPT_IOAPIC_BASE) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED ||
        ioapic_send_eoi(INTERRUPT_IOAPIC_BASE - 1U) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_send_eoi(INTERRUPT_LOCAL_APIC_BASE) !=
            IOAPIC_STATUS_BAD_VECTOR ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, NULL) !=
            IOAPIC_STATUS_NULL_ARGUMENT ||
        ioapic_read_redirection(INTERRUPT_IOAPIC_BASE, &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("an unrouted vector was acknowledged");
    }

    /* And every malformed routing request, through the public interface. */
    if (ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, 0U,
            (enum ioapic_trigger)7) != IOAPIC_STATUS_BAD_TRIGGER ||
        ioapic_route_isa_irq_as(0U, INTERRUPT_IOAPIC_BASE, UINT8_MAX + 1U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_DESTINATION ||
        ioapic_route_isa_irq_as(UINT8_C(16), INTERRUPT_IOAPIC_BASE, 0U,
            IOAPIC_TRIGGER_FORCE_LEVEL) != IOAPIC_STATUS_BAD_IRQ) {
        kernel_test_fail("a malformed routing request was accepted");
    }

    /* A wait needs a running timer, a target and a bound the clock can hold. */
    if (pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS, NULL) !=
            PIT_STATUS_NULL_ARGUMENT ||
        pit_wait_for_ticks_bounded(1U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_NOT_RUNNING ||
        elapsed_ns != 0U) {
        kernel_test_fail("a bounded wait ran without a running timer");
    }

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC_LEVEL) !=
        PIT_STATUS_OK) {
        kernel_test_fail("the timer would not take the level-triggered route");
    }

    if (pit_active_route() != PIT_ROUTE_IO_APIC_LEVEL ||
        pit_wait_for_ticks_bounded(0U, IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns) != PIT_STATUS_BAD_INTERVAL ||
        pit_wait_for_ticks_bounded(1U, PIT_MAX_WAIT_NS + 1U, &elapsed_ns) !=
            PIT_STATUS_BAD_INTERVAL) {
        kernel_test_fail("a bounded wait accepted an interval it cannot hold");
    }

    /*
     * Read the entry off the hardware. An entry programmed edge triggered while
     * Sapote's records called it level triggered would deliver every interrupt
     * below and latch nothing, so this is the check that catches it.
     */
    if (ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_OK ||
        !entry.level_triggered || entry.masked || entry.active_low ||
        entry.vector != pit_active_vector() ||
        entry.global_interrupt != 2U) {
        kernel_test_fail("the level route did not read back level triggered");
    }

    if (!ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 1U) {
        kernel_test_fail("the level route was not recorded as level triggered");
    }

    if (pic_mask_snapshot() != UINT16_C(0xFFFF)) {
        kernel_test_fail("level routing unmasked a legacy PIC line");
    }

    /*
     * A vector names one pin. Pointing this one at IRQ4's pin as well would
     * leave the timer's entry unmasked and delivering a vector the dispatcher
     * would acknowledge on the wrong unit, so it is refused by name.
     */
    if (ioapic_route_isa_irq(4U, pit_active_vector(), 0U) !=
        IOAPIC_STATUS_VECTOR_IN_USE) {
        kernel_test_fail("one vector was pointed at two redirection entries");
    }

    before = ioapic_get_state();

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the level-triggered line stopped delivering");
    }

    after = ioapic_get_state();

    /*
     * Stop before printing: framebuffer-backed console output can exceed the
     * next one-shot period and queue a vector after the handler is removed.
     */
    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the level route would not stop");
    }

    console_write("ST INFO ioapic-level: ");
    console_write_u64(pit_ticks());
    console_write(" deliveries, remote IRR ");
    console_write_u64(after.remote_irr_observed);
    console_write(", directed EOI ");
    console_write_u64(after.directed_eoi_count);
    console_write(", mode ");
    console_write(after.directed_eoi_mode ? "directed" : "broadcast");
    console_write(", in ");
    console_write_u64(elapsed_ns);
    console_write(" ns\n");

    if (pit_ticks() < IOAPIC_LEVEL_TEST_TICKS) {
        kernel_test_fail("a level-triggered line delivered too few interrupts");
    }

    /*
     * And not far more than it was asked for. A pin acknowledged while its
     * source is still asserting re-delivers inside the acknowledgement, so it
     * counts thousands of interrupts in the time eight should take. The
     * interval check below would fail on that too, but only after describing
     * it as a timing problem; this names it for what it is.
     */
    if (pit_ticks() > IOAPIC_LEVEL_TEST_TICKS * 2U) {
        kernel_test_fail("a level-triggered line delivered without stopping");
    }

    /*
     * Every delivery latched remote IRR and every one was acknowledged at the
     * I/O APIC. The counts are what say the entry behaved as a level-triggered
     * entry rather than merely that interrupts arrived.
     */
    if (after.remote_irr_observed - before.remote_irr_observed <
            IOAPIC_LEVEL_TEST_TICKS ||
        after.remote_irr_missing != 0U ||
        (after.directed_eoi_mode &&
         (after.directed_eoi_count - before.directed_eoi_count <
              IOAPIC_LEVEL_TEST_TICKS ||
          !apic_get_state().eoi_broadcasts_suppressed)) ||
        (!after.directed_eoi_mode &&
         after.directed_eoi_count != before.directed_eoi_count)) {
        kernel_test_fail("a level-triggered delivery did not latch remote IRR");
    }

    /*
     * Refuse a source that re-delivers too quickly. The bounded wait supplies
     * the upper limit; a host pause may legitimately stretch emulated time.
     */
    if (elapsed_ns <
        expected_ns - expected_ns / PM_TIMER_TOLERANCE_QUARTER) {
        kernel_test_fail("level-triggered deliveries did not take a period");
    }

    /* Stopping unroutes the entry, so its vector is nothing's again. */
    if (ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_get_state().level_routes != 0U ||
        ioapic_read_redirection(pit_active_vector(), &entry) !=
            IOAPIC_STATUS_VECTOR_NOT_ROUTED) {
        kernel_test_fail("a stopped level route is still routed");
    }

    /*
     * The same pin, sampled as an edge again. An edge-triggered entry has no
     * remote IRR to latch and nothing to acknowledge, so both counters must
     * stand still across eight more deliveries. Without this, an
     * implementation that treated every route as level triggered would pass
     * everything above.
     */
    before = ioapic_get_state();

    if (pit_start(PIT_TEST_FREQUENCY, PIT_ROUTE_IO_APIC) != PIT_STATUS_OK ||
        ioapic_vector_is_level_triggered(pit_active_vector()) ||
        ioapic_send_eoi(pit_active_vector()) !=
            IOAPIC_STATUS_NOT_LEVEL_TRIGGERED) {
        kernel_test_fail("an edge route was treated as level triggered");
    }

    if (pit_wait_for_ticks_bounded(
            IOAPIC_LEVEL_TEST_TICKS,
            IOAPIC_LEVEL_TEST_BOUND_NS,
            &elapsed_ns
        ) != PIT_STATUS_OK) {
        kernel_test_fail("the edge-triggered line stopped delivering");
    }

    after = ioapic_get_state();

    if (after.remote_irr_observed != before.remote_irr_observed ||
        after.directed_eoi_count != before.directed_eoi_count ||
        after.level_routes != 0U) {
        kernel_test_fail("an edge-triggered delivery latched remote IRR");
    }

    if (pit_stop() != PIT_STATUS_OK) {
        kernel_test_fail("the edge route would not stop");
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

/*
 * Prove the permissions are enforced by the processor rather than merely
 * recorded in a table.
 *
 * `make verify` has always refused an RWX load segment, and until this
 * increment that assertion was the only thing standing behind Sapote's W^X
 * claim - and it inspects the ELF file, not the machine the kernel runs on.
 * Everything below the rejections is the part a file check can never do: a
 * fresh frame is mapped writable, written, narrowed to read-only, and written
 * again, and the scenario passes only if the processor refuses the second write
 * with the exact fault a supervisor write to a present read-only page produces.
 *
 * The probe returns if the store succeeds, so a permission that quietly failed
 * to take shows up as a scenario failure rather than as a timeout.
 */
static void paging_scenario(const struct paging_device_windows *device_windows)
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
    if (paging_map(PAGING_PROBE_ADDRESS + 1U, 0U, SAPOTE_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_UNALIGNED_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, 0U, PAGING_WRITE) !=
            PAGING_STATUS_ZERO_LENGTH ||
        paging_map(UINT64_C(0x0000800000000000), 0U, SAPOTE_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_NONCANONICAL_ADDRESS ||
        paging_map(PAGING_PROBE_ADDRESS, 0U, SAPOTE_PAGE_SIZE,
            PAGING_WRITE | PAGING_EXECUTE) !=
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        kernel_test_fail("a malformed mapping request was accepted");
    }

    if (paging_map(text & ~(SAPOTE_PAGE_SIZE - 1U), 0U, SAPOTE_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_ALREADY_MAPPED ||
        paging_unmap(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        paging_protect(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_NOT_MAPPED) {
        kernel_test_fail("an impossible mapping change was accepted");
    }

    /*
     * The bulk identity map uses 2 MiB leaves, and splitting one is deferred,
     * so a 4 KiB change inside one is refused rather than silently applied to
     * the whole 2 MiB.
     */
    if (paging_protect(PAGING_TEST_HUGE_ADDRESS, SAPOTE_PAGE_SIZE,
            PAGING_READ) != PAGING_STATUS_HUGE_PAGE_PRESENT ||
        paging_unmap(PAGING_TEST_HUGE_ADDRESS, SAPOTE_PAGE_SIZE) !=
            PAGING_STATUS_HUGE_PAGE_PRESENT) {
        kernel_test_fail("a 2 MiB mapping accepted a 4 KiB change");
    }

    if (paging_initialize(device_windows) !=
        PAGING_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("page tables accepted a second installation");
    }

    if (frame_allocate(&frame) != FRAME_STATUS_OK) {
        kernel_test_fail("no frame was available for the probe page");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, frame, SAPOTE_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_map(PAGING_PROBE_ADDRESS, frame, SAPOTE_PAGE_SIZE,
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

    if (paging_protect(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE, PAGING_READ) !=
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
    if (paging_unmap(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE) !=
        PAGING_STATUS_OK) {
        kernel_test_fail("the probe page would not unmap before the cycle");
    }

    frames_before = frame_allocator_get_stats().free_frames;
    tables_before = paging_get_state().table_frames;

    for (size_t cycle = 0; cycle < PAGING_TEST_CYCLES; ++cycle) {
        uintptr_t cycle_frame;

        if (frame_allocate(&cycle_frame) != FRAME_STATUS_OK ||
            paging_map(PAGING_PROBE_ADDRESS, cycle_frame, SAPOTE_PAGE_SIZE,
                PAGING_WRITE) != PAGING_STATUS_OK ||
            paging_unmap(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE) !=
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
        paging_map(PAGING_PROBE_ADDRESS, frame, SAPOTE_PAGE_SIZE,
            PAGING_WRITE) != PAGING_STATUS_OK ||
        paging_protect(PAGING_PROBE_ADDRESS, SAPOTE_PAGE_SIZE, PAGING_READ) !=
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

    /*
     * The heap's boundary proof must be able to fill its whole 16 MiB window.
     * The screen normally owns a long-lived 3 MiB client now, so this one
     * destructive scenario relinquishes it before testing the allocator in
     * isolation. The scenario ends by faulting on the upper guard and never
     * returns to code that needs the screen.
     */
    if (screen_is_active() && screen_release() != SCREEN_STATUS_OK) {
        kernel_test_fail("the screen did not release its heap surface");
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
static const struct paging_device_window *find_device_window(
    const struct paging_device_windows *windows,
    enum paging_device_window_kind kind
)
{
    if (windows == NULL) {
        return NULL;
    }

    for (size_t index = 0U; index < windows->count; ++index) {
        if (windows->entries[index].kind == kind) {
            return &windows->entries[index];
        }
    }

    return NULL;
}

static void pci_ecam_scenario(
    const struct acpi_mcfg *mcfg,
    bool mcfg_present,
    const struct paging_device_windows *device_windows
)
{
    const struct paging_device_window *ecam =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_PCI_ECAM);
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

    if (ecam == NULL || pci.ecam_size != PAGING_ECAM_WINDOW_SIZE ||
        pci.ecam_base != ecam->physical_base ||
        pci.ecam_size != ecam->length) {
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
     * A bus past what Sapote mapped is refused rather than folded back into the
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

    /*
     * The heap grows but deliberately never shrinks. With the surface already
     * occupying its first 3 MiB, the first thread table can commit one more
     * heap page that remains mapped after the table is freed. Warm that path
     * before taking the frame baseline so the comparison below measures only
     * the stack frames this scenario owns.
     */
    if (thread_start() != THREAD_STATUS_OK ||
        thread_stop() != THREAD_STATUS_OK) {
        kernel_test_fail("threads did not survive an empty start and stop");
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
/*
 * The screen console, checked where boot cannot check it.
 *
 * prove_screen_console in src/kernel/boot_proofs.c verifies that what was drawn
 * is on the glass. What it cannot do is take the console apart: boot needs the
 * console it is printing through, so it can never leave it in a broken state to
 * see what happens. This scenario can, because nothing after it needs a screen.
 *
 * Every character in the font is drawn and read back here, not a sample. The
 * boot proof draws nine letters; a glyph table with one bad row in the middle
 * of it would pass that and fail this.
 */
/*
 * The keyboard, taken apart where boot cannot.
 *
 * prove_keyboard injects five scancodes and checks three characters come out.
 * What it cannot do is fill the queue, because boot needs the keyboard working
 * afterwards, and it cannot toggle caps lock, because that would leave the
 * machine in a state the rest of boot did not ask for. Nothing after this
 * scenario needs a keyboard, so this can do both.
 */
/*
 * The shell, taken apart where boot cannot.
 *
 * prove_shell types one command and reads the answer off the glass. What it
 * cannot do is run every command, because several of them clear the screen or
 * print pages, and boot has to keep its transcript. Nothing after this scenario
 * needs a console, so this can run all of them.
 */
static void shell_scenario(void)
{
    static const char *const commands[] = {
        "help", "echo hello", "uptime", "mem", "pci", "keys", "threads",
        "ledger", "version", "clear"
    };

    struct shell_state before;
    struct shell_state after;

    if (!shell_is_active()) {
        kernel_test_fail("the shell scenario has no shell");
    }

    before = shell_get_state();

    /*
     * Every command this shell has, run for real. A command that faults or
     * hangs takes the scenario with it, which is the point: these read live
     * kernel state and any of them could be pointing at something that has
     * since moved.
     */
    for (size_t index = 0; index < sizeof(commands) / sizeof(commands[0]);
         ++index) {
        if (shell_execute(commands[index]) != SHELL_STATUS_OK) {
            kernel_test_fail("a built-in command refused to run");
        }
    }

    after = shell_get_state();

    if (after.commands - before.commands !=
        sizeof(commands) / sizeof(commands[0])) {
        kernel_test_fail("the shell did not count every command it ran");
    }

    if (after.unknown != before.unknown) {
        kernel_test_fail("the shell did not recognise one of its own commands");
    }

    /*
     * A prefix must not run a longer command, and a longer word must not run a
     * shorter one. Both directions, because a dispatcher that compares only as
     * far as its own name gets one of them wrong.
     */
    if (shell_execute("hel") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a prefix of a command ran that command");
    }

    if (shell_execute("helpful") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a longer word ran a shorter command");
    }

    if (shell_execute("echoes") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("a longer word ran echo");
    }

    /*
     * The line editor, driven the way a person drives it. Typed, corrected with
     * backspace, and submitted - and the correction has to have taken, which
     * only the command that runs can show.
     */
    before = shell_get_state();

    {
        static const char typed[] = "echa\bo corrected";

        for (size_t index = 0; typed[index] != '\0'; ++index) {
            if (shell_feed(typed[index]) != SHELL_STATUS_OK) {
                kernel_test_fail("the shell refused a character while typing");
            }
        }

        if (shell_feed('\n') != SHELL_STATUS_OK) {
            kernel_test_fail("the corrected line did not run");
        }
    }

    after = shell_get_state();

    if (after.unknown != before.unknown) {
        kernel_test_fail("backspace did not correct the command");
    }

    if (after.length != 0U) {
        kernel_test_fail("the shell kept the line after running it");
    }

    /*
     * An erase has to reach the glass, not only the buffer.
     *
     * This exists because a control found it missing. Deleting the space and
     * the second backspace from the shell's erase sequence - so the cursor
     * moves but the character stays on screen - passed every check here, and
     * chasing that found a real bug underneath: the screen console did not
     * handle backspace at all, so a correction drew the font's replacement
     * character instead of stepping back.
     */
    if (screen_is_active()) {
        if (screen_clear() != SCREEN_STATUS_OK) {
            kernel_test_fail("the shell scenario could not clear the screen");
        }

        if (shell_feed('a') != SHELL_STATUS_OK ||
            shell_feed('b') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a character before an erase");
        }

        if (screen_verify_cell(1U, 0U, 'b') != SCREEN_STATUS_OK) {
            kernel_test_fail("a typed character did not reach the screen");
        }

        if (shell_feed('\b') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a backspace");
        }

        /* The erased cell is blank, and the one before it is untouched. */
        if (screen_verify_cell(1U, 0U, ' ') != SCREEN_STATUS_OK) {
            kernel_test_fail("backspace did not erase the character on screen");
        }

        if (screen_verify_cell(0U, 0U, 'a') != SCREEN_STATUS_OK) {
            kernel_test_fail("backspace erased more than one character");
        }

        /* And the next character lands where the erased one was. */
        if (shell_feed('c') != SHELL_STATUS_OK) {
            kernel_test_fail("the shell refused a character after an erase");
        }

        if (screen_verify_cell(1U, 0U, 'c') != SCREEN_STATUS_OK) {
            kernel_test_fail("the cursor did not return to the erased cell");
        }

        while (shell_get_state().length > 0U) {
            if (shell_feed('\b') != SHELL_STATUS_OK) {
                kernel_test_fail("the shell would not clear its line");
            }
        }
    }

    /* And an unknown command is reported without stopping anything. */
    before = shell_get_state();

    if (shell_execute("definitelynotacommand") != SHELL_STATUS_UNKNOWN_COMMAND) {
        kernel_test_fail("an unknown command was not reported");
    }

    if (shell_get_state().unknown != before.unknown + 1U) {
        kernel_test_fail("an unknown command was not counted");
    }

    if (shell_execute("help") != SHELL_STATUS_OK) {
        kernel_test_fail("the shell stopped working after an unknown command");
    }

    after = shell_get_state();
    console_write("Sapote: shell scenario ran ");
    console_write_u64(after.commands);
    console_write(" commands and refused ");
    console_write_u64(after.unknown);
    console_write(" unknown\n");
}

static void keyboard_scenario(void)
{
    struct keyboard_state before;
    struct keyboard_state after;
    struct keyboard_event event;
    size_t drained = 0U;

    if (!keyboard_is_initialized()) {
        kernel_test_fail("the keyboard scenario has no keyboard");
    }

    /* Bringing it up twice is refused. Boot only ever does it once. */
    if (keyboard_initialize() != KEYBOARD_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("the keyboard was brought up twice");
    }

    /* Drain whatever boot's proof left, so the counts below start clean. */
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        drained += 1U;

        if (drained > 4096U) {
            kernel_test_fail("the keyboard queue would not drain");
        }
    }

    if (keyboard_read(&event) != KEYBOARD_STATUS_EMPTY) {
        kernel_test_fail("an empty keyboard queue did not say so");
    }

    if (keyboard_read(NULL) == KEYBOARD_STATUS_OK) {
        kernel_test_fail("the keyboard wrote an event through a null pointer");
    }

    /*
     * Caps lock, which boot deliberately does not touch. It is a toggle on
     * press only, it applies to letters and not to digits, and a second press
     * undoes it.
     */
    before = keyboard_get_state();

    if (before.caps_lock) {
        kernel_test_fail("caps lock was already latched");
    }

    cpu_interrupt_enable();

    if (keyboard_inject_scancode(0x3AU) != KEYBOARD_STATUS_OK ||
        keyboard_inject_scancode(0x1EU) != KEYBOARD_STATUS_OK) {
        cpu_interrupt_disable();
        kernel_test_fail("the controller refused an injected scancode");
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        if (keyboard_get_state().events >= before.events + 2U) {
            break;
        }
    }

    cpu_interrupt_disable();

    if (!keyboard_get_state().caps_lock) {
        kernel_test_fail("caps lock did not latch on press");
    }

    /* The 'a' after it must have arrived capitalised. */
    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        if (event.character == '\0') {
            continue;
        }

        if (event.character != 'A') {
            kernel_test_fail("caps lock did not capitalise the next letter");
        }
    }

    if (keyboard_character_for(0x02U, false, true) != '1') {
        kernel_test_fail("caps lock changed a digit");
    }

    cpu_interrupt_enable();

    if (keyboard_inject_scancode(0x3AU) != KEYBOARD_STATUS_OK) {
        cpu_interrupt_disable();
        kernel_test_fail("the controller refused the second caps lock");
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        if (!keyboard_get_state().caps_lock) {
            break;
        }
    }

    cpu_interrupt_disable();

    if (keyboard_get_state().caps_lock) {
        kernel_test_fail("caps lock did not release on a second press");
    }

    while (keyboard_read(&event) == KEYBOARD_STATUS_OK) {
        /* discard */
    }

    /*
     * Overflow. The queue holds sixty-four minus one; more than that must be
     * counted as dropped rather than silently overwriting what is waiting.
     * Interrupts stay off so nothing is consumed while it fills.
     */
    before = keyboard_get_state();

    if (before.dropped != 0U) {
        kernel_test_fail("the keyboard had already dropped an event");
    }

    /*
     * Interrupts stay on for the whole flood. An earlier version toggled them
     * around each injection, which delivered nothing at all: sti does not take
     * effect until after the instruction following it, so sti immediately
     * followed by cli leaves a window of exactly zero instructions. The
     * controller holds one byte, so each injection has to be taken by the
     * handler before the next will land, and the bounded wait inside
     * keyboard_inject_scancode is what gives it the chance.
     */
    cpu_interrupt_enable();

    for (uint32_t index = 0; index < 200U; ++index) {
        if (keyboard_inject_scancode(0x1EU) != KEYBOARD_STATUS_OK) {
            cpu_interrupt_disable();
            kernel_test_fail("the controller refused a flood of scancodes");
        }
    }

    for (uint64_t spins = 0; spins < UINT64_C(200000000); ++spins) {
        const struct keyboard_state now = keyboard_get_state();

        if (now.events + now.dropped >= before.events + 200U) {
            break;
        }
    }

    cpu_interrupt_disable();
    after = keyboard_get_state();

    if (after.queued >= KEYBOARD_QUEUE_SIZE) {
        kernel_test_fail("the keyboard queue grew past its bound");
    }

    if (after.dropped == 0U) {
        kernel_test_fail("a flooded keyboard queue dropped nothing");
    }

    if (after.events + after.dropped < before.events + 200U) {
        kernel_test_fail("the keyboard lost events it never accounted for");
    }

    console_write("Sapote: keyboard scenario queued ");
    console_write_u64((uint64_t)after.queued);
    console_write(" and dropped ");
    console_write_u64(after.dropped - before.dropped);
    console_write(" of a 200 event flood\n");
}

static void screen_scenario(void)
{
    struct screen_state before;
    struct screen_state after;
    uint32_t width = 0U;
    uint32_t height = 0U;
    uint32_t first = 0U;
    uint32_t count = 0U;

    if (!screen_is_active()) {
        kernel_test_fail("the screen scenario has no console");
    }

    if (sapote_font_geometry(&width, &height, &first, &count) !=
        FONT_STATUS_OK) {
        kernel_test_fail("the font table would not describe itself");
    }

    before = screen_get_state();

    if (before.cell_width != width || before.cell_height != height) {
        kernel_test_fail("the console and the font disagree about the cell");
    }

    /*
     * Bringing the console up twice must be refused. Boot cannot test this,
     * because boot only ever does it once.
     */
    if (screen_initialize() != SCREEN_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail("the console adopted the screen twice");
    }

    /*
     * Every glyph the table covers, drawn and read back. A cell is checked
     * immediately after it is written so a later character cannot repair an
     * earlier one by overlapping it.
     */
    if (screen_clear() != SCREEN_STATUS_OK) {
        kernel_test_fail("the console would not clear");
    }

    for (uint32_t code = first; code < first + count; ++code) {
        const char character = (char)(unsigned char)code;
        const struct screen_state cursor = screen_get_state();

        if (screen_putc(character) != SCREEN_STATUS_OK) {
            kernel_test_fail("the console refused a character its font covers");
        }

        if (screen_verify_cell(cursor.column, cursor.row, character) !=
            SCREEN_STATUS_OK) {
            kernel_test_fail("a glyph did not reach the screen intact");
        }
    }

    after = screen_get_state();

    if (after.characters - before.characters != (uint64_t)count) {
        kernel_test_fail("the console lost a character it said it drew");
    }

    /*
     * A cell outside the grid is refused rather than clamped, and the refusal
     * is the console's own rather than the framebuffer's bounds check catching
     * it afterwards.
     */
    if (screen_verify_cell(after.columns, 0U, 'x') != SCREEN_STATUS_NO_ROOM) {
        kernel_test_fail("the console read a cell past its last column");
    }

    if (screen_verify_cell(0U, after.rows, 'x') != SCREEN_STATUS_NO_ROOM) {
        kernel_test_fail("the console read a cell past its last row");
    }

    /*
     * A scroll that actually moves rows, checked by content.
     *
     * This exists because a control found it missing. Scrolling by more than
     * the screen and scrolling by zero both take early exits - one is a fill,
     * one is a no-op - so neither reaches the copy loop, and reversing that
     * loop's direction left every check above still passing. A copy whose
     * destination is above its source must walk forwards or it reads rows it
     * has already overwritten, and only content one cell tall can tell.
     */
    if (screen_clear() != SCREEN_STATUS_OK) {
        kernel_test_fail("the console would not clear before the scroll check");
    }

    if (screen_write("top\nsecond") != SCREEN_STATUS_OK) {
        kernel_test_fail("the console refused the scroll fixture");
    }

    while (screen_get_state().row + 1U < before.rows) {
        if (screen_putc('\n') != SCREEN_STATUS_OK) {
            kernel_test_fail("the console refused to reach its last row");
        }
    }

    if (screen_putc('\n') != SCREEN_STATUS_OK) {
        kernel_test_fail("the console refused to scroll one line");
    }

    /* The second line must now be the first. */
    for (uint32_t column = 0U; column < 6U; ++column) {
        if (screen_verify_cell(column, 0U, "second"[column]) !=
            SCREEN_STATUS_OK) {
            kernel_test_fail("a scroll did not move the rows it copied");
        }
    }

    console_write("Sapote: screen scenario drew ");
    console_write_u64((uint64_t)count);
    console_write(" glyphs and read every one back\n");
}

static uint32_t surface_test_colour(uint32_t value)
{
    return framebuffer_pack(
        (uint8_t)(value * 3U + 1U),
        (uint8_t)(value * 5U + 2U),
        (uint8_t)(value * 7U + 3U)
    );
}

static void require_framebuffer_pixel(
    uint32_t x,
    uint32_t y,
    uint32_t expected,
    const char *reason
)
{
    uint32_t pixel = 0U;
    const uint32_t mask = framebuffer_visible_mask();

    if (framebuffer_read_pixel(x, y, &pixel) != FRAMEBUFFER_STATUS_OK ||
        (pixel & mask) != (expected & mask)) {
        kernel_test_fail(reason);
    }
}

/*
 * The self-test proves the primitives over guarded synthetic rows. This proves
 * the other half: heap allocation, damage presented to device memory, and the
 * framebuffer pitch all agree on where the same pixels live.
 */
static void surface_scenario(void)
{
    uint32_t source[4U * 5U];
    const struct framebuffer_state framebuffer = framebuffer_get_state();
    const uint32_t base = surface_test_colour(1U);
    const uint32_t changed = surface_test_colour(2U);
    const uint32_t clipped_colour = surface_test_colour(3U);
    struct surface surface = { 0 };
    struct surface_rect rectangle;
    uint32_t origin_x;
    uint32_t origin_y;

    if (!framebuffer_is_active()) {
        kernel_test_fail("the surface scenario has no framebuffer");
    }

    if (framebuffer.width < 16U || framebuffer.height < 32U) {
        kernel_test_fail("the framebuffer is too small for surface fixtures");
    }

    if (surface_initialize(&surface, framebuffer.width, framebuffer.height) !=
        SURFACE_STATUS_OK) {
        kernel_test_fail("the surface scenario could not allocate its buffer");
    }

    rectangle.x = 0U;
    rectangle.y = 0U;
    rectangle.width = surface.width;
    rectangle.height = surface.height;

    if (surface_fill_rect(&surface, rectangle, base) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels !=
            (uint64_t)surface.width * surface.height ||
        surface.damage.pending) {
        kernel_test_fail("a full surface present copied the wrong damage");
    }

    require_framebuffer_pixel(surface.width - 1U, surface.height - 1U, base,
        "a full surface present missed its last pixel");

    if (surface_pixel(&surface, surface.width - 2U, surface.height - 2U,
            changed) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 1U) {
        kernel_test_fail("one damaged surface pixel copied more than itself");
    }

    require_framebuffer_pixel(surface.width - 2U, surface.height - 2U,
        changed, "a damaged surface pixel was presented on the wrong row");

    rectangle.x = 0U;
    rectangle.y = surface.height / 2U;
    rectangle.width = surface.width;
    rectangle.height = 16U;

    if (surface_fill_rect(&surface, rectangle, changed) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != (uint64_t)surface.width * 16U) {
        kernel_test_fail("one text line copied more than one line");
    }

    rectangle.x = surface.width - 2U;
    rectangle.y = surface.height - 2U;
    rectangle.width = 4U;
    rectangle.height = 4U;

    if (surface_fill_rect(&surface, rectangle, clipped_colour) !=
            SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 4U) {
        kernel_test_fail("a clipped fill crossed the surface edge");
    }

    require_framebuffer_pixel(surface.width - 1U, surface.height - 1U,
        clipped_colour, "a clipped fill missed its visible corner");

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            source[y * 5U + x] = surface_test_colour(y * 16U + x);
        }

        source[y * 5U + 4U] = UINT32_C(0xDEADBEEF);
    }

    origin_x = 8U;
    origin_y = 8U;

    if (surface_blit(&surface, origin_x, origin_y, source, 4U, 4U,
            5U * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 16U) {
        kernel_test_fail("a padded source did not blit as four rows");
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            require_framebuffer_pixel(origin_x + x, origin_y + y,
                surface_test_colour(y * 16U + x),
                "surface blit used the destination pitch for its source");
        }
    }

    rectangle.x = origin_x;
    rectangle.y = origin_y;
    rectangle.width = 4U;
    rectangle.height = 4U;

    if (surface_copy_rect(&surface, rectangle, origin_x + 1U,
            origin_y + 1U) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("a downward overlapping copy was refused");
    }

    for (uint32_t y = 0U; y < 4U; ++y) {
        for (uint32_t x = 0U; x < 4U; ++x) {
            require_framebuffer_pixel(origin_x + x + 1U,
                origin_y + y + 1U, surface_test_colour(y * 16U + x),
                "a downward overlapping copy read overwritten pixels");
        }
    }

    if (surface_blit(&surface, origin_x, origin_y, source, 4U, 4U,
            5U * SURFACE_BYTES_PER_PIXEL) != SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("the overlap fixture could not be restored");
    }

    rectangle.x = origin_x + 1U;
    rectangle.y = origin_y + 1U;
    rectangle.width = 3U;
    rectangle.height = 3U;

    if (surface_copy_rect(&surface, rectangle, origin_x, origin_y) !=
            SURFACE_STATUS_OK ||
        surface_present(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("an upward overlapping copy was refused");
    }

    for (uint32_t y = 0U; y < 3U; ++y) {
        for (uint32_t x = 0U; x < 3U; ++x) {
            require_framebuffer_pixel(origin_x + x, origin_y + y,
                surface_test_colour((y + 1U) * 16U + x + 1U),
                "an upward overlapping copy read overwritten pixels");
        }
    }

    if (surface_pixel(&surface, 1U, 2U, changed) != SURFACE_STATUS_OK ||
        surface_pixel(&surface, 4U, 6U, changed) != SURFACE_STATUS_OK ||
        !surface.damage.pending || surface.damage.rectangle.x != 1U ||
        surface.damage.rectangle.y != 2U ||
        surface.damage.rectangle.width != 4U ||
        surface.damage.rectangle.height != 5U ||
        surface_present(&surface) != SURFACE_STATUS_OK ||
        surface.last_present_pixels != 20U) {
        kernel_test_fail("surface damage did not form one bounding rectangle");
    }

    if (surface_release(&surface) != SURFACE_STATUS_OK) {
        kernel_test_fail("the surface scenario leaked its buffer");
    }

    console_write("ST SURFACE full ");
    console_write_u64((uint64_t)framebuffer.width * framebuffer.height);
    console_write(" line ");
    console_write_u64((uint64_t)framebuffer.width * 16U);
    console_write(" clipped 4 overlap both damage 20\n");
}

static void write_combining_scenario(
    const struct boot_framebuffer *framebuffer,
    const struct paging_device_windows *device_windows
)
{
    const struct paging_state paging = paging_get_state();
    const struct paging_device_window *framebuffer_window =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_FRAMEBUFFER);
    const struct paging_device_window *ecam =
        find_device_window(device_windows, PAGING_DEVICE_WINDOW_PCI_ECAM);
    struct paging_translation translation;
    struct surface cached = {0};

    if (framebuffer == NULL || !framebuffer->present ||
        framebuffer_window == NULL) {
        kernel_test_fail("the write-combining scenario has no framebuffer");
    }

    if (paging_verify() != PAGING_STATUS_OK ||
        paging.write_combining_pat_entry != 1U ||
        ((paging.pat_after >> 8U) & UINT64_C(0xFF)) != 1U) {
        kernel_test_fail("IA32_PAT does not select write-combining entry 1");
    }

    for (unsigned int index = 0U; index < 8U; ++index) {
        if (index != paging.write_combining_pat_entry &&
            ((paging.pat_before >> (index * 8U)) & UINT64_C(0xFF)) !=
                ((paging.pat_after >> (index * 8U)) & UINT64_C(0xFF))) {
            kernel_test_fail("IA32_PAT changed an entry it did not own");
        }
    }

    for (uint64_t offset = 0U; offset < framebuffer_window->length;
         offset += PAGING_PAGE_SIZE) {
        const uint64_t address = framebuffer_window->physical_base + offset;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
            translation.physical_address != address ||
            translation.level != 1U ||
            translation.permissions !=
                (PAGING_WRITE | PAGING_WRITE_COMBINING) ||
            translation.memory_type != PAGING_MEMORY_WRITE_COMBINING) {
            kernel_test_fail("a framebuffer page is not write-combining");
        }
    }

    for (uint64_t offset = 0U; ecam != NULL && offset < ecam->length;
         offset += PAGING_PAGE_SIZE) {
        const uint64_t address = ecam->physical_base + offset;

        if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
            translation.permissions != (PAGING_WRITE | PAGING_UNCACHED) ||
            translation.memory_type != PAGING_MEMORY_UNCACHEABLE) {
            kernel_test_fail("a PCI ECAM page is not uncacheable");
        }
    }

    if (paging_translate((uint64_t)(uintptr_t)&active_scenario, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
        kernel_test_fail("ordinary kernel RAM is not write-back");
    }

    if (paging_map(PAGING_PROBE_ADDRESS, 0U, PAGING_PAGE_SIZE,
            PAGING_UNCACHED | PAGING_WRITE_COMBINING) !=
        PAGING_STATUS_CONFLICTING_MEMORY_TYPES) {
        kernel_test_fail("paging accepted incompatible memory types");
    }

    if (surface_initialize(&cached, framebuffer->width, framebuffer->height) !=
            SURFACE_STATUS_OK ||
        surface_verify(&cached) != SURFACE_STATUS_OK) {
        kernel_test_fail("the cached surface is not write-back");
    }

    if (surface_release(&cached) != SURFACE_STATUS_OK ||
        framebuffer_verify() != FRAMEBUFFER_STATUS_OK) {
        kernel_test_fail("write-combining state did not survive verification");
    }

    console_write("ST WRITE-COMBINING PAT ");
    console_write_hex(paging.pat_after);
    console_write(" ENTRY ");
    console_write_u64(paging.write_combining_pat_entry);
    console_write(" FRAMEBUFFER ");
    console_write_u64(framebuffer_window->length / PAGING_PAGE_SIZE);
    console_write(" PAGES\n");
}

static void device_windows_scenario(
    const struct paging_device_windows *expected
)
{
    const struct paging_device_windows *installed =
        paging_get_device_windows();
    struct paging_translation translation;
    struct paging_audit audit;
    size_t failed_window = PAGING_DEVICE_WINDOW_NONE;
    size_t page_count = 0U;
    size_t io_apic_count = 0U;
    bool found_vga = false;
    bool found_local_apic = false;
    bool found_ecam = false;
    bool found_framebuffer = false;

    if (paging_verify_device_windows(expected, &failed_window) !=
            PAGING_STATUS_OK ||
        paging_audit_hierarchy(&audit) != PAGING_STATUS_OK ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        kernel_test_fail("the installed device-window registry is invalid");
    }

    for (size_t index = 0U; index < installed->count; ++index) {
        const struct paging_device_window *window = &installed->entries[index];
        enum paging_memory_type required_type = PAGING_MEMORY_UNCACHEABLE;

        switch (window->kind) {
        case PAGING_DEVICE_WINDOW_VGA_TEXT:
            found_vga = true;
            break;
        case PAGING_DEVICE_WINDOW_LOCAL_APIC:
            found_local_apic = true;
            break;
        case PAGING_DEVICE_WINDOW_IO_APIC:
            ++io_apic_count;
            break;
        case PAGING_DEVICE_WINDOW_PCI_ECAM:
            found_ecam = true;
            break;
        case PAGING_DEVICE_WINDOW_FRAMEBUFFER:
            found_framebuffer = true;
            required_type = PAGING_MEMORY_WRITE_COMBINING;
            break;
        case PAGING_DEVICE_WINDOW_KIND_COUNT:
        default:
            kernel_test_fail("the installed registry has an unknown kind");
        }

        if (window->memory_type != required_type ||
            window->permissions != PAGING_DEVICE_WINDOW_WRITE) {
            kernel_test_fail("a device window has the wrong policy");
        }

        for (uint64_t offset = 0U; offset < window->length;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = window->physical_base + offset;
            const uint32_t permissions = PAGING_WRITE |
                (required_type == PAGING_MEMORY_WRITE_COMBINING
                    ? PAGING_WRITE_COMBINING
                    : PAGING_UNCACHED);

            if (paging_translate(address, &translation) != PAGING_STATUS_OK ||
                translation.physical_address != address ||
                translation.permissions != permissions ||
                translation.memory_type != required_type ||
                translation.level != 1U) {
                kernel_test_fail("a complete device window did not translate");
            }

            ++page_count;
        }
    }

    if (!found_vga || !found_local_apic || io_apic_count == 0U) {
        kernel_test_fail("the installed registry lacks a required window");
    }

    if (paging_translate((uint64_t)(uintptr_t)&active_scenario, &translation) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_WRITE ||
        translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
        kernel_test_fail("ordinary RAM is not write-back");
    }

    console_write("ST DEVICE-WINDOWS WINDOWS ");
    console_write_u64(installed->count);
    console_write(" PAGES ");
    console_write_u64(page_count);
    console_write(" VGA 1 LOCAL-APIC 1 IO-APICS ");
    console_write_u64(io_apic_count);
    console_write(" ECAM ");
    console_write_u64(found_ecam ? 1U : 0U);
    console_write(" FRAMEBUFFER ");
    console_write_u64(found_framebuffer ? 1U : 0U);
    console_putc('\n');
}

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

    /*
     * Boot adopts the framebuffer before the scenarios run, because the screen
     * console has to come up early enough to show the rest of the boot. So the
     * framebuffer being already initialized is the expected state here and not
     * a failure; what this scenario needs is a framebuffer that is up, not one
     * that it personally brought up.
     *
     * Adopting it a second time must still be refused, and that refusal is the
     * one this branch is asserting.
     */
    status = framebuffer_initialize(framebuffer);

    if (status != FRAMEBUFFER_STATUS_OK &&
        status != FRAMEBUFFER_STATUS_ALREADY_INITIALIZED) {
        kernel_test_fail(framebuffer_status_string(status));
    }

    if (!framebuffer_is_active()) {
        kernel_test_fail("the framebuffer scenario has no framebuffer");
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

    cpu_store_fence();

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
    const struct kernel_test_context *context
)
{
    enum pit_status pit_status;

    if (scenario == KERNEL_TEST_NONE) {
        return;
    }

    active_scenario = scenario;
    test_marker("BEGIN", scenario);

    if (context == NULL || context->framebuffer == NULL ||
        context->device_windows == NULL ||
        (context->mcfg_present && context->mcfg == NULL)) {
        kernel_test_fail("the test context is incomplete");
    }

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
    case KERNEL_TEST_IOAPIC_LEVEL:
        ioapic_level_scenario();
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
        paging_scenario(context->device_windows);
        kernel_test_fail("a read-only page accepted a supervisor write");
    case KERNEL_TEST_HEAP:
        heap_scenario();
        kernel_test_fail("a heap guard page accepted a supervisor write");
    case KERNEL_TEST_PCI:
        pci_scenario(context->mcfg, context->mcfg_present);
        kernel_test_pass();
    case KERNEL_TEST_PCI_ECAM:
        pci_ecam_scenario(context->mcfg, context->mcfg_present,
            context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_THREADS:
        threads_scenario();
        kernel_test_pass();
    case KERNEL_TEST_THREAD_GUARD:
        thread_guard_scenario();
        kernel_test_fail("a thread stack guard page accepted a write");
    case KERNEL_TEST_FRAMEBUFFER:
        framebuffer_scenario(context->framebuffer);
        kernel_test_pass();
    case KERNEL_TEST_SCREEN:
        screen_scenario();
        kernel_test_pass();
    case KERNEL_TEST_KEYBOARD:
        keyboard_scenario();
        kernel_test_pass();
    case KERNEL_TEST_SHELL:
        shell_scenario();
        kernel_test_pass();
    case KERNEL_TEST_SURFACE:
        surface_scenario();
        kernel_test_pass();
    case KERNEL_TEST_WRITE_COMBINING:
        write_combining_scenario(context->framebuffer,
            context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_DEVICE_WINDOWS:
        device_windows_scenario(context->device_windows);
        kernel_test_pass();
    case KERNEL_TEST_BOOT_LEDGER:
        /* Deferred until kernel_main publishes the fully verified receipts. */
        return;
    case KERNEL_TEST_FIRST_LIGHT:
        /* Deferred until the ledger and UI are both installed and published. */
        return;
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_XHCI:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_NVME:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_FILESYSTEM:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_PROCESS:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_LINUX_ABI:
        /* Deferred until the proof receipt is installed and published. */
        return;
    case KERNEL_TEST_DOUBLE_FAULT:
        kernel_test_double_fault_armed = 1U;
        interrupt_test_set_gate_present(14U, false);
        interrupt_trigger_page_fault();
    case KERNEL_TEST_INVALID:
        kernel_test_fail("invalid or duplicate sapote.test argument");
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

static uint32_t boot_ledger_stage_sequence(
    const struct boot_ledger *ledger,
    enum boot_stage_id stage
)
{
    const struct boot_stage_receipt *receipt =
        boot_ledger_receipt_for(ledger, stage);

    return receipt == NULL ? 0U : receipt->sequence;
}

static uint32_t boot_ledger_capability_sequence(
    const struct boot_ledger *ledger,
    enum boot_capability capability
)
{
    for (size_t receipt_index = 0U;
         receipt_index < ledger->receipt_count;
         ++receipt_index) {
        const struct boot_stage_receipt *receipt =
            boot_ledger_receipt_at(ledger, receipt_index);

        if (receipt == NULL) {
            kernel_test_fail("Boot Ledger receipt lookup inconsistent");
        }

        for (size_t capability_index = 0U;
             capability_index < receipt->provided_capability_count;
             ++capability_index) {
            if (receipt->provided_capabilities[capability_index] ==
                capability) {
                return receipt->sequence;
            }
        }
    }

    return 0U;
}

_Noreturn void kernel_test_complete_boot_ledger(
    const struct boot_context *context
)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *framebuffer_wc;
    const struct boot_stage_receipt *framebuffer_output;
    uint32_t device_windows;
    uint32_t paging_install;
    uint32_t paging_proofs;
    uint32_t interrupts;

    if (active_scenario != KERNEL_TEST_BOOT_LEDGER) {
        kernel_test_fail("Boot Ledger completion used outside its scenario");
    }

    if (context == NULL || ledger == NULL || !ledger->validated ||
        !ledger->executed || ledger->status != BOOT_LEDGER_STATUS_OK) {
        kernel_test_fail("the installed Boot Ledger is incomplete");
    }

    device_windows = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_DEVICE_WINDOWS);
    paging_install = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_PAGING_INSTALL);
    paging_proofs = boot_ledger_stage_sequence(ledger,
        BOOT_STAGE_PAGING_PROOFS);
    interrupts = boot_ledger_capability_sequence(ledger,
        BOOT_CAPABILITY_INTERRUPTS_ENABLED);

    for (size_t index = 0U; index < ledger->planned_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            boot_ledger_planned_stage_at(ledger, index);
        const struct boot_stage_receipt *receipt =
            boot_ledger_receipt_at(ledger, index);

        if (descriptor == NULL || receipt == NULL ||
            descriptor->id != receipt->stage_id ||
            receipt->sequence != index + 1U) {
            kernel_test_fail("validated plan and receipt order differ");
        }

        if (descriptor->required &&
            (receipt->result != BOOT_RECEIPT_RAN ||
             receipt->status != BOOT_LEDGER_STATUS_OK)) {
            kernel_test_fail("a mandatory Boot Ledger stage has no receipt");
        }

        if (receipt->result == BOOT_RECEIPT_RAN) {
            for (size_t requirement = 0U;
                 requirement < descriptor->required_capability_count;
                 ++requirement) {
                const uint32_t provider = boot_ledger_capability_sequence(
                    ledger, descriptor->required_capabilities[requirement]);

                if (provider == 0U || provider >= receipt->sequence) {
                    kernel_test_fail(
                        "a Boot Ledger dependency was ordered after its consumer"
                    );
                }
            }
        }
    }

    if (device_windows == 0U || paging_install <= device_windows ||
        paging_proofs <= paging_install ||
        boot_ledger_capability_sequence(ledger,
            BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED) != paging_proofs ||
        boot_ledger_capability_sequence(ledger,
            BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED) !=
                paging_proofs) {
        kernel_test_fail("paging receipts violate device-window proof order");
    }

    if (interrupts <= boot_ledger_stage_sequence(ledger,
            BOOT_STAGE_INTERRUPT_FOUNDATION) ||
        interrupts <= boot_ledger_stage_sequence(ledger,
            BOOT_STAGE_INTERRUPT_CONTROLLERS)) {
        kernel_test_fail("interrupt enable preceded its foundation");
    }

    framebuffer_wc = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FRAMEBUFFER_WC);
    framebuffer_output = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FRAMEBUFFER_OUTPUT);

    if (context->information.framebuffer.present) {
        if (framebuffer_wc == NULL || framebuffer_output == NULL ||
            framebuffer_wc->result != BOOT_RECEIPT_RAN ||
            framebuffer_output->result != BOOT_RECEIPT_RAN ||
            framebuffer_output->sequence <= framebuffer_wc->sequence) {
            kernel_test_fail("framebuffer output preceded independent WC proof");
        }
    } else if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED) ||
        framebuffer_output == NULL ||
        framebuffer_output->result == BOOT_RECEIPT_RAN) {
        kernel_test_fail("absent framebuffer leaked a success capability");
    }

    if (!boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE) ||
        !boot_ledger_fingerprint_valid(ledger)) {
        kernel_test_fail("installed Boot Ledger fingerprint is invalid");
    }

    console_write("ST LEDGER stages ");
    console_write_u64(ledger->planned_count);
    console_write(" receipts ");
    console_write_u64(ledger->receipt_count);
    console_write(" capabilities ");
    console_write_u64(ledger->established_capability_count);
    console_write(" skips ");
    console_write_u64(ledger->optional_skip_count);
    console_write(" fingerprint ");
    console_write_hex(ledger->fingerprint);
    console_putc('\n');
    kernel_test_pass();
}

static uint32_t first_light_pixel(uint32_t x, uint32_t y)
{
    uint32_t pixel = 0U;
    struct surface *surface = screen_surface();

    if (surface == NULL ||
        surface_read_pixel(surface, x, y, &pixel) != SURFACE_STATUS_OK) {
        kernel_test_fail("First Light cached-surface pixel read failed");
    }
    return pixel;
}

static void first_light_expect_text_pixel(
    char character,
    uint32_t x,
    uint32_t baseline,
    uint32_t expected,
    const char *failure
)
{
    const struct ui_font_metrics metrics = ui_font_get_metrics();
    uint8_t glyph[UI_FONT_MAX_HEIGHT * UI_FONT_MAX_ROW_BYTES];

    if (sapote_ui_font_glyph((uint32_t)(unsigned char)character, glyph,
            sizeof(glyph)) != UI_FONT_STATUS_OK ||
        baseline < metrics.ascent) {
        kernel_test_fail("First Light stable text probe has no glyph");
    }
    for (uint32_t row = 0U; row < metrics.height; ++row) {
        for (uint32_t column = 0U; column < metrics.width; ++column) {
            const uint8_t byte = glyph[
                row * metrics.row_bytes + column / 8U
            ];

            if ((byte & (uint8_t)(0x80U >> (column & 7U))) != 0U) {
                if (first_light_pixel(x + column,
                        baseline - metrics.ascent + row) != expected) {
                    kernel_test_fail(failure);
                }
                return;
            }
        }
    }
    kernel_test_fail("First Light stable text probe glyph is empty");
}

static void first_light_process_ui(const char *failure)
{
    enum ui_status status = ui_process_events();

    if (status == UI_STATUS_OK) {
        status = ui_flush();
    }
    if (status != UI_STATUS_OK) {
        kernel_test_fail(failure);
    }
}

static void first_light_inject_pointer(
    uint8_t flags,
    int32_t delta_x,
    int32_t delta_y,
    const char *failure
)
{
    const int8_t device_x = (int8_t)delta_x;
    const int8_t device_y = (int8_t)-delta_y;
    uint8_t packet_flags = flags;

    if (device_x < 0) {
        packet_flags |= UINT8_C(0x10);
    }
    if (device_y < 0) {
        packet_flags |= UINT8_C(0x20);
    }
    cpu_interrupt_enable();
    const enum pointer_status status = pointer_inject_packet(packet_flags,
        (uint8_t)device_x, (uint8_t)device_y);
    cpu_interrupt_disable();
    if (status != POINTER_STATUS_OK) {
        kernel_test_fail(failure);
    }
    first_light_process_ui(failure);
}

static void first_light_move_pointer(
    uint32_t target_x,
    uint32_t target_y,
    const char *failure
)
{
    for (uint32_t packets = 0U; packets < 64U; ++packets) {
        const struct pointer_state pointer = pointer_get_state();
        int32_t delta_x;
        int32_t delta_y;

        if (pointer.x == target_x && pointer.y == target_y) {
            return;
        }
        delta_x = (int32_t)target_x - (int32_t)pointer.x;
        delta_y = (int32_t)target_y - (int32_t)pointer.y;
        if (delta_x > 127) {
            delta_x = 127;
        } else if (delta_x < -127) {
            delta_x = -127;
        }
        if (delta_y > 127) {
            delta_y = 127;
        } else if (delta_y < -127) {
            delta_y = -127;
        }
        first_light_inject_pointer(0U, delta_x, delta_y, failure);
    }
    kernel_test_fail("First Light cursor did not reach its dock target");
}

static void first_light_click_dock_item(
    const struct ui_dock_item *item,
    enum ui_panel_id expected_panel,
    char title_initial
)
{
    const uint32_t target_x = item->bounds.x + item->bounds.width / 2U;
    const uint32_t target_y = item->bounds.y + item->bounds.height / 2U;
    const uint32_t state_x = item->bounds.x + 4U;
    const uint32_t state_y = item->bounds.y + 4U;
    const struct ui_state *ui;
    uint32_t title_width;

    first_light_move_pointer(target_x, target_y,
        "First Light real pointer movement failed");
    ui = ui_get_state();
    if (ui->hover != item->id ||
        first_light_pixel(state_x, state_y) != ui->theme.white) {
        kernel_test_fail("First Light dock hover state pixel is incorrect");
    }

    first_light_inject_pointer(UINT8_C(0x01), 0, 0,
        "First Light pointer press failed");
    ui = ui_get_state();
    if (ui->pressed != item->id ||
        first_light_pixel(state_x, state_y) != ui->theme.shadow) {
        kernel_test_fail("First Light dock pressed state pixel is incorrect");
    }

    first_light_inject_pointer(0U, 0, 0,
        "First Light pointer release failed");
    ui = ui_get_state();
    if (ui->pressed != UI_ELEMENT_NONE ||
        ui->active_panel != expected_panel ||
        first_light_pixel(state_x, state_y) != ui->theme.title_active) {
        kernel_test_fail("First Light dock active state pixel is incorrect");
    }
    if (first_light_pixel(ui->layout.panel.x + 4U,
            ui->layout.panel.y + 4U) != ui->theme.title_inactive) {
        kernel_test_fail("First Light panel title surface pixel is incorrect");
    }
    if (ui_font_text_width(ui_panel_name(expected_panel), &title_width) !=
            UI_FONT_STATUS_OK) {
        kernel_test_fail("First Light panel title width is unavailable");
    }
    first_light_expect_text_pixel(title_initial,
        ui->layout.panel.x + 4U +
            (ui->layout.panel.width - 8U - title_width) / 2U,
        ui->layout.panel_title_baseline, ui->theme.white,
        "First Light panel title glyph pixel is incorrect");
}

_Noreturn void kernel_test_complete_first_light(void)
{
    static const enum ui_element_id ids[UI_DOCK_ITEM_COUNT] = {
        UI_ELEMENT_DOCK_TERMINAL, UI_ELEMENT_DOCK_LEDGER,
        UI_ELEMENT_DOCK_SYSTEM, UI_ELEMENT_DOCK_ABOUT
    };
    static const enum ui_action actions[UI_DOCK_ITEM_COUNT] = {
        UI_ACTION_TOGGLE_TERMINAL, UI_ACTION_TOGGLE_LEDGER,
        UI_ACTION_TOGGLE_SYSTEM, UI_ACTION_TOGGLE_ABOUT
    };
    static const enum ui_panel_id panels[UI_DOCK_ITEM_COUNT] = {
        UI_PANEL_TERMINAL, UI_PANEL_LEDGER, UI_PANEL_SYSTEM, UI_PANEL_ABOUT
    };
    static const char initials[UI_DOCK_ITEM_COUNT] = { 'T', 'L', 'S', 'A' };
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *font;
    const struct boot_stage_receipt *layout;
    const struct boot_stage_receipt *construction;
    const struct boot_stage_receipt *activation;
    const struct boot_stage_receipt *proof_receipt;
    const struct boot_stage_receipt *wc;
    const struct ui_state *ui = ui_get_state();
    const struct ui_point initial_pointer = ui->pointer;
    const struct ui_render_counters initial_renders = ui->renders;
    struct ui_proof proof;
    enum ui_status proof_status;

    if (active_scenario != KERNEL_TEST_FIRST_LIGHT) {
        kernel_test_fail("First Light completion used outside its scenario");
    }
    if (ledger == NULL || !ledger->validated || !ledger->executed ||
        ledger->status != BOOT_LEDGER_STATUS_OK || ledger->degraded ||
        !boot_ledger_fingerprint_valid(ledger)) {
        kernel_test_fail("First Light installed ledger is invalid");
    }
    font = boot_ledger_receipt_for(ledger, BOOT_STAGE_UI_FONT);
    layout = boot_ledger_receipt_for(ledger, BOOT_STAGE_UI_LAYOUT);
    construction = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DESKTOP_CONSTRUCTION);
    activation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DESKTOP_ACTIVATION);
    proof_receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FIRST_LIGHT_PROOF);
    wc = boot_ledger_receipt_for(ledger, BOOT_STAGE_FRAMEBUFFER_WC);
    if (font == NULL || layout == NULL || construction == NULL ||
        activation == NULL || proof_receipt == NULL || wc == NULL ||
        font->result != BOOT_RECEIPT_RAN ||
        layout->result != BOOT_RECEIPT_RAN ||
        construction->result != BOOT_RECEIPT_RAN ||
        activation->result != BOOT_RECEIPT_RAN ||
        proof_receipt->result != BOOT_RECEIPT_RAN ||
        wc->result != BOOT_RECEIPT_RAN) {
        kernel_test_fail("First Light required stage receipt is missing");
    }
    if (wc->sequence >= construction->sequence ||
        wc->sequence >= activation->sequence ||
        construction->sequence >= activation->sequence ||
        activation->sequence >= proof_receipt->sequence) {
        kernel_test_fail("First Light desktop present preceded its WC proof");
    }
    if (!boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_UI_FONT_VERIFIED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_UI_LAYOUT_VALIDATED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FIRST_LIGHT_INSTALLED_PROOF_COMPLETE)) {
        kernel_test_fail("First Light installed capability is missing");
    }
    if (!ui->active || !ui->pointer_present || !ui->ledger_pass ||
        !pointer_is_present() || ui->layout.surface.width != 1024U ||
        ui->layout.surface.height != 768U) {
        kernel_test_fail("First Light installed UI state is incomplete");
    }
    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        const struct ui_dock_item *item = &ui->layout.dock_items[index];

        if (item->id != ids[index] || item->action != actions[index] ||
            item->panel != panels[index]) {
            kernel_test_fail("First Light dock typed action is incorrect");
        }
    }

    if (first_light_pixel(ui->layout.menu_bar.x,
            ui->layout.menu_bar.y) != ui->theme.white ||
        first_light_pixel(ui->layout.workspace_bar.x + 4U,
            ui->layout.workspace_bar.y) != ui->theme.desktop_light ||
        first_light_pixel(ui->layout.workspace_bar.x + 4U,
            ui->layout.workspace_bar.y + 2U) != ui->theme.desktop_dark ||
        first_light_pixel(ui->layout.hero_window.x,
            ui->layout.hero_window.y) != ui->theme.ink) {
        kernel_test_fail("First Light Workbench chrome stable pixel is incorrect");
    }
    if (first_light_pixel(ui->layout.logo.x,
            ui->layout.logo.y) != ui->theme.window_face) {
        kernel_test_fail("First Light logo field is not integrated");
    }
    if (first_light_pixel(ui->layout.logo.x + 140U,
            ui->layout.logo.y + 140U) != ui->theme.ink) {
        kernel_test_fail("First Light logo stable pixel is incorrect");
    }
    first_light_expect_text_pixel('S', ui->layout.wordmark.x,
        ui->layout.title_baseline, ui->theme.ink,
        "First Light wordmark stable pixel is incorrect");
    if (first_light_pixel(ui->layout.dock.x, ui->layout.dock.y) !=
            ui->theme.ink ||
        first_light_pixel(ui->layout.ledger_status.x + 4U,
            ui->layout.ledger_status.y + 4U) != ui->theme.white) {
        kernel_test_fail("First Light dock or ledger stable pixel is incorrect");
    }

    for (size_t index = 0U; index < UI_DOCK_ITEM_COUNT; ++index) {
        first_light_click_dock_item(&ui->layout.dock_items[index],
            panels[index], initials[index]);
        ui = ui_get_state();
    }
    if (initial_pointer.x < 0 || initial_pointer.y < 0 ||
        first_light_pixel((uint32_t)initial_pointer.x,
            (uint32_t)initial_pointer.y) != ui->theme.window_face ||
        first_light_pixel((uint32_t)ui->pointer.x,
            (uint32_t)ui->pointer.y) != ui->theme.ink ||
        ui->renders.cursor_moves <= initial_renders.cursor_moves ||
        ui->renders.damage_rectangles <= initial_renders.damage_rectangles) {
        kernel_test_fail("First Light cursor damage left a trail");
    }

    struct keyboard_event keyboard = {
        .scancode = 0x01U, .pressed = true, .shift = false, .character = '\0'
    };
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("First Light keyboard panel close failed");
    }
    first_light_process_ui("First Light keyboard panel close draw failed");
    keyboard.scancode = 0x0FU;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("First Light keyboard focus-next failed");
    }
    first_light_process_ui("First Light keyboard focus-next draw failed");
    if (ui_get_state()->focus != UI_ELEMENT_DOCK_LEDGER) {
        kernel_test_fail("First Light keyboard focus-next chose wrong item");
    }
    keyboard.shift = true;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("First Light keyboard focus-previous failed");
    }
    first_light_process_ui("First Light keyboard focus-previous draw failed");
    if (ui_get_state()->focus != UI_ELEMENT_DOCK_TERMINAL) {
        kernel_test_fail("First Light keyboard focus-previous chose wrong item");
    }
    keyboard.scancode = 0x1CU;
    keyboard.shift = false;
    if (ui_handle_keyboard(&keyboard) != UI_STATUS_OK) {
        kernel_test_fail("First Light keyboard activation failed");
    }
    first_light_process_ui("First Light keyboard activation draw failed");
    if (ui_get_state()->active_panel != UI_PANEL_TERMINAL) {
        kernel_test_fail("First Light keyboard activation chose wrong panel");
    }

    if (!boot_plan_pointer_absence_self_test()) {
        kernel_test_fail("First Light pointer-absence synthetic plan failed");
    }
    proof_status = ui_verify_installed(&proof);
    if (proof_status != UI_STATUS_OK) {
        kernel_test_fail("First Light final installed redraw proof failed");
    }
    if (proof.width != 1024U || proof.height != 768U ||
        proof.dock_items != UI_DOCK_ITEM_COUNT ||
        proof.ledger_fingerprint != ledger->fingerprint ||
        proof.render_hash == 0U) {
        kernel_test_fail("First Light final installed shape is inconsistent");
    }
    if (proof.events == 0U || proof.panels < 5U ||
        proof.cursor_moves == 0U || proof.damage_rectangles == 0U ||
        proof.glyphs == 0U) {
        kernel_test_fail("First Light final interaction counters are incomplete");
    }

    console_write("ST FIRST_LIGHT geometry ");
    console_write_u64(proof.width);
    console_putc('x');
    console_write_u64(proof.height);
    console_write(" dock ");
    console_write_u64(proof.dock_items);
    console_write(" events ");
    console_write_u64(proof.events);
    console_write(" panels ");
    console_write_u64(proof.panels);
    console_write(" cursor ");
    console_write_u64(proof.cursor_moves);
    console_write(" damage ");
    console_write_u64(proof.damage_rectangles);
    console_write(" glyphs ");
    console_write_u64(proof.glyphs);
    console_write(" fingerprint ");
    console_write_hex(proof.ledger_fingerprint);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_device_substrate(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *receipt;
    const struct device_substrate_proof proof = device_substrate_get_proof();
    const size_t negative_controls = 4U + 4U + 2U + 2U +
        proof.negative_controls;

    if (active_scenario != KERNEL_TEST_DEVICE_SUBSTRATE) {
        kernel_test_fail("device-substrate completion used outside its scenario");
    }
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_DEVICE_SUBSTRATE_PROOF);
    if (ledger == NULL || receipt == NULL ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != 1U ||
        receipt->proof_counters[1] != DEVICE_SUBSTRATE_DMA_BYTES ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_FIXTURE_ABSENT)) {
        kernel_test_fail("device-substrate installed receipt is invalid");
    }
    if (proof.queue_size == 0U || proof.used_before != 0U ||
        proof.used_after != 1U ||
        proof.used_length != DEVICE_SUBSTRATE_DMA_BYTES ||
        proof.interrupt_count != 1U ||
        proof.random_bytes != DEVICE_SUBSTRATE_DMA_BYTES ||
        proof.nonzero_bytes == 0U ||
        !proof.dma_device_written || !proof.msix_delivered ||
        !proof.ownership_round_trip || !proof.teardown_complete ||
        proof.negative_controls != 2U || negative_controls != 14U) {
        kernel_test_fail("device-substrate installed proof is inconsistent");
    }

    console_write("ST DEVICE_SUBSTRATE dma ");
    console_write_u64(proof.random_bytes);
    console_write(" msix ");
    console_write_u64(proof.interrupt_count);
    console_write(" used 0->1 ownership CPU-DEVICE-CPU teardown clean ");
    console_write("negatives ");
    console_write_u64(negative_controls);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_xhci(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct xhci_descriptor_proof proof = xhci_get_descriptor_proof();

    if (active_scenario != KERNEL_TEST_XHCI) {
        kernel_test_fail("xHCI completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_XHCI_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_XHCI_DESCRIPTOR_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_XHCI_FIXTURE_ABSENT)) {
        kernel_test_fail("xHCI installed receipt is invalid");
    }
    if (proof.descriptor_bytes != XHCI_DEVICE_DESCRIPTOR_BYTES ||
        proof.msix_completion_count != 1U ||
        proof.robustness_tests != XHCI_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.controller_ready || !proof.descriptor_valid ||
        !proof.sentinel_changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("xHCI installed proof is inconsistent");
    }

    console_write("ST XHCI descriptor ");
    console_write_u64(proof.descriptor_bytes);
    console_write(" msix ");
    console_write_u64(proof.msix_completion_count);
    console_write(" ownership CPU-CONTROLLER-CPU teardown clean robustness ");
    console_write_u64(proof.robustness_tests);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_nvme(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct nvme_read_proof proof = nvme_get_read_proof();

    if (active_scenario != KERNEL_TEST_NVME) {
        kernel_test_fail("NVMe completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_NVME_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_NVME_READ_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != NVME_BLOCK_BYTES ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_NVME_FIXTURE_ABSENT)) {
        kernel_test_fail("NVMe installed receipt is invalid");
    }
    if (proof.block_bytes != NVME_BLOCK_BYTES ||
        proof.msix_completion_count != 1U ||
        proof.ignored_completions != 0U ||
        proof.robustness_tests != NVME_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.controller_ready || !proof.namespace_ready ||
        !proof.contents_valid || !proof.sentinel_valid ||
        !proof.changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("NVMe installed proof is inconsistent");
    }

    console_write("ST NVME read ");
    console_write_u64(proof.block_bytes);
    console_write(" msix ");
    console_write_u64(proof.msix_completion_count);
    console_write(" ownership CPU-CONTROLLER-CPU teardown clean robustness ");
    console_write_u64(proof.robustness_tests);
    console_putc('\n');
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_filesystem(void)
{
    static const uint8_t expected_name[FAT16_CANONICAL_NAME_BYTES] =
        {'S', 'A', 'P', 'O', 'T', 'E', ' ', ' ', 'B', 'I', 'N'};
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *foundation;
    const struct boot_stage_receipt *receipt;
    const struct filesystem_file_proof proof = filesystem_get_file_proof();

    if (active_scenario != KERNEL_TEST_FILESYSTEM) {
        kernel_test_fail("filesystem completion used outside its scenario");
    }
    foundation = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FAT16_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_FILESYSTEM_FILE_PROOF);
    if (ledger == NULL || foundation == NULL || receipt == NULL ||
        foundation->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != FAT16_FILE_BYTES ||
        receipt->proof_counters[1] != 4U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FAT16_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FILESYSTEM_FIXTURE_ABSENT)) {
        kernel_test_fail("filesystem installed receipt is invalid");
    }
    for (size_t index = 0U; index < sizeof(expected_name); ++index) {
        if (proof.canonical_name[index] != expected_name[index]) {
            kernel_test_fail("filesystem canonical name is invalid");
        }
    }
    if (proof.file_bytes != FAT16_FILE_BYTES || proof.read_count != 4U ||
        proof.msix_completion_count != 4U ||
        proof.ignored_completions != 0U ||
        proof.robustness_tests != FILESYSTEM_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.fat16_ready || !proof.file_located || !proof.contents_valid ||
        !proof.sentinel_valid || !proof.changed_while_controller_owned ||
        !proof.ownership_complete || !proof.teardown_complete) {
        kernel_test_fail("filesystem installed proof is inconsistent");
    }

    kernel_test_pass();
}

_Noreturn void kernel_test_complete_process(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *address_space;
    const struct boot_stage_receipt *elf64;
    const struct boot_stage_receipt *receipt;
    const struct process_proof_result proof = process_get_proof_result();

    if (active_scenario != KERNEL_TEST_PROCESS) {
        kernel_test_fail("process completion used outside its scenario");
    }
    address_space = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_PROCESS_ADDRESS_SPACE_FOUNDATION);
    elf64 = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_ELF64_LOADER_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_PROCESS_INSTALLED_PROOF);
    if (ledger == NULL || address_space == NULL || elf64 == NULL ||
        receipt == NULL || address_space->result != BOOT_RECEIPT_RAN ||
        elf64->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != 128U ||
        receipt->proof_counters[1] != 1U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_ADDRESS_SPACE_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_ELF64_LOADER_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT)) {
        kernel_test_fail("process installed receipt is invalid");
    }
    if (proof.file_bytes != 128U || proof.segment_count != 1U ||
        proof.result != UINT32_C(0x53415037) ||
        proof.robustness_tests != PROCESS_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.ring_three || !proof.private_address_space ||
        !proof.image_read_execute || !proof.stack_read_write_no_execute ||
        !proof.guard_unmapped || !proof.interrupt_authenticated ||
        !proof.normal_exit || !proof.teardown_complete ||
        !proof.resource_census_equal || !process_resources_released()) {
        kernel_test_fail("process installed proof is inconsistent");
    }
    kernel_test_pass();
}

_Noreturn void kernel_test_complete_linux_abi(void)
{
    const struct boot_ledger *ledger = boot_ledger_installed();
    const struct boot_stage_receipt *syscall_cpu;
    const struct boot_stage_receipt *image_stack;
    const struct boot_stage_receipt *receipt;
    const struct linux_abi_proof_result proof =
        linux_abi_get_proof_result();

    if (active_scenario != KERNEL_TEST_LINUX_ABI) {
        kernel_test_fail("Linux ABI completion used outside its scenario");
    }
    syscall_cpu = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_SYSCALL_CPU_FOUNDATION);
    image_stack = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_IMAGE_STACK_FOUNDATION);
    receipt = boot_ledger_receipt_for(ledger,
        BOOT_STAGE_LINUX_INSTALLED_PROOF);
    if (ledger == NULL || syscall_cpu == NULL || image_stack == NULL ||
        receipt == NULL || syscall_cpu->result != BOOT_RECEIPT_RAN ||
        image_stack->result != BOOT_RECEIPT_RAN ||
        receipt->result != BOOT_RECEIPT_RAN ||
        receipt->proof_counter_count != 2U ||
        receipt->proof_counters[0] != 33584U ||
        receipt->proof_counters[1] != 9U ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_IMAGE_STACK_FOUNDATION_AVAILABLE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_LINUX_FIXTURE_ABSENT)) {
        kernel_test_fail("Linux ABI installed receipt is invalid");
    }
    if (proof.file_bytes != 33584U || proof.program_headers != 5U ||
        proof.load_segments != 4U || proof.file_clusters != 9U ||
        proof.stdout_bytes != 7U || proof.syscall_count != 9U ||
        proof.distinct_syscalls != 7U || proof.exit_status != 0U ||
        proof.robustness_tests != LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS ||
        !proof.ring_three || !proof.private_address_space ||
        !proof.real_syscall_instruction || !proof.stdout_valid ||
        !proof.exit_zero || !proof.unknown_enosys ||
        !proof.write_xor_execute || !proof.kernel_cr3_restored ||
        !proof.teardown_complete || !proof.resource_census_equal ||
        !linux_abi_resources_released()) {
        kernel_test_fail("Linux ABI installed proof is inconsistent");
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
    case KERNEL_TEST_IOAPIC_LEVEL:
        return "ioapic-level";
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
    case KERNEL_TEST_SCREEN:
        return "screen";
    case KERNEL_TEST_KEYBOARD:
        return "keyboard";
    case KERNEL_TEST_SHELL:
        return "shell";
    case KERNEL_TEST_SURFACE:
        return "surface";
    case KERNEL_TEST_WRITE_COMBINING:
        return "write-combining";
    case KERNEL_TEST_DEVICE_WINDOWS:
        return "device-windows";
    case KERNEL_TEST_BOOT_LEDGER:
        return "boot-ledger";
    case KERNEL_TEST_FIRST_LIGHT:
        return "first-light";
    case KERNEL_TEST_DEVICE_SUBSTRATE:
        return "device-substrate";
    case KERNEL_TEST_XHCI:
        return "xhci";
    case KERNEL_TEST_NVME:
        return "nvme";
    case KERNEL_TEST_FILESYSTEM:
        return "filesystem";
    case KERNEL_TEST_PROCESS:
        return "process";
    case KERNEL_TEST_LINUX_ABI:
        return "linux-abi";
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
