/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * The order boot happens in.
 *
 * This file is deliberately close to a list. Describing the machine and proving
 * it live in src/kernel/boot_report.c and src/kernel/boot_proofs.c; what is left
 * here is the sequence, because the sequence is the part that has to be read top
 * to bottom to be understood at all.
 *
 * Order is the argument in several places and the comments say so where it is:
 * page tables are installed only once frames can be allocated, the 8254 is
 * retired only once something independent of it can time an interval, and the
 * closing verification block runs after the last thing that writes through each
 * mapping rather than before it.
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
#include <seneri/heap.h>
#include <seneri/interrupts.h>
#include <seneri/font.h>
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

    if (!screen_self_test()) {
        console_panic("screen console grid self-test failed");
    }

    if (!keyboard_self_test()) {
        console_panic("keyboard translation self-test failed");
    }

    if (!shell_self_test()) {
        console_panic("shell line and dispatch self-test failed");
    }

    /*
     * The first self-test in this kernel that is not C. It runs beside the
     * others because a caller should not have to care which language answered.
     */
    if (seneri_logo_self_test() != 1) {
        console_panic("logo decoder self-test failed");
    }

    if (seneri_font_self_test() != 1) {
        console_panic("font reader self-test failed");
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

    /*
     * As early as the address space allows, which is here: the framebuffer is a
     * device window paging carved out above, so this cannot run before
     * install_page_tables, and everything after this line appears on the screen
     * as well as on the serial port.
     *
     * The logo is drawn first and the console then replaces it. That ordering
     * is the whole argument for putting this here rather than at the end of
     * boot: a splash nobody can read is worth less than the log, and a log that
     * only starts once boot has finished has missed the part somebody watching
     * a machine that will not start actually needs.
     */
    prove_framebuffer(&context.framebuffer);

    if (framebuffer_is_active()) {
        draw_logo();
        prove_screen_console();
    }

    /*
     * Input beside output, and before the scenarios, so a scenario that wants
     * a keyboard finds one. It needs only the I/O APIC, which came up long
     * before this, so there is nothing else holding it back.
     */
    prove_keyboard();
    prove_shell();

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
    bring_up_pci(boot_mcfg_present ? &boot_mcfg : NULL, boot_mcfg_present);
    prove_threads();
    prove_preemption();

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

    /*
     * The framebuffer is re-checked here and not only inside its own proof,
     * because the logo is blitted after that proof runs: roughly 800,000 more
     * stores through the same mapping. Verifying before the last thing that
     * writes through a mapping is verifying the wrong moment, which is exactly
     * what paging_verify and heap_verify are placed here to avoid.
     *
     * Threads are deliberately absent from this block. thread_stop has already
     * run by now, so there is no table left to check; docs/THREADS.md records
     * that thread_verify's moment is while threads exist rather than at the end
     * of boot.
     */
    if (framebuffer_is_active()) {
        const enum framebuffer_status framebuffer_status =
            framebuffer_verify();

        if (framebuffer_status != FRAMEBUFFER_STATUS_OK) {
            console_panic(framebuffer_status_string(framebuffer_status));
        }
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
    console_write("Seneri OS: preemption passed\n");
    console_write("Seneri OS: framebuffer passed\n");
    console_write("Seneri OS: logo passed\n");
    console_write("Seneri OS: screen console passed\n");
    console_write("Seneri OS: keyboard passed\n");
    console_write("Seneri OS: shell passed\n");
    console_write("Seneri OS: never triple fault milestone passed\n");

    if (test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    /*
     * With no scenario selected there is nobody waiting for this machine to
     * stop, so it does not: boot ends by handing the console to whoever is in
     * front of it. Every scenario, including normal, has already left above -
     * a scenario has to finish and a shell does not.
     */
    shell_run();
}
