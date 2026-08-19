/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_BOOT_STAGES_H
#define SENERI_BOOT_STAGES_H

#include <stdbool.h>

#include <seneri/acpi.h>
#include <seneri/apic.h>
#include <seneri/boot.h>
#include <seneri/ioapic.h>
#include <seneri/memory.h>
#include <seneri/pit.h>
#include <seneri/pm_timer.h>

/*
 * The stages kernel_main runs, in the two files it runs them from.
 *
 * This header exists because kernel.c had grown to 2,211 lines and had stopped
 * having a single responsibility: every increment added a proof and every proof
 * landed there. docs/DEBT.md wrote that down at 1,990 lines and the next
 * increment added 221 more, so the split is the debt being paid rather than
 * described.
 *
 * Nothing declared here is a general-purpose interface. Every one of these is
 * called exactly once, from kernel_main, in the order boot happens. They are
 * declared rather than static only because they now live beside each other
 * instead of beside their caller.
 *
 * The division is by what a function is allowed to do:
 *
 *   src/kernel/boot_report.c   describes what was found and never decides
 *   src/kernel/boot_proofs.c   decides, and panics when the answer is wrong
 *
 * A function that panics does not belong in the first group, and a function
 * that only writes to the console does not belong in the second.
 */

/*
 * Reporting. Each of these turns one discovered structure into the transcript
 * lines the Makefile asserts. None of them panic, and none of them read state
 * the caller has not handed them.
 */
void report_boot_context(const struct boot_context *context);
void report_allocator(const struct frame_allocator_stats *stats);
void report_acpi_root(const struct acpi_root *root);
void report_acpi_madt(const struct acpi_madt *madt);
void report_acpi_fadt(const struct acpi_fadt *fadt);
void report_acpi_mcfg(const struct acpi_mcfg *mcfg, bool present);
void report_acpi_topology(const struct acpi_topology *topology);
void report_pm_timer(const struct pm_timer_state *pm_timer);
void report_apic(const struct apic_state *apic);
void report_ioapic(const struct ioapic_state *ioapic);

/*
 * Bring-up and proof. Every one of these either establishes a layer or
 * demonstrates that an established layer does what its document claims, and
 * every one of them panics rather than returning a status, because a boot that
 * cannot prove its own foundation has nothing useful to return it to.
 */
void prove_frame_lifecycle(void);

void install_page_tables(
    const struct acpi_topology *topology,
    const struct acpi_mcfg *mcfg,
    const struct boot_framebuffer *framebuffer
);
void prove_paging_lifecycle(void);

void bring_up_heap(void);
void prove_heap_lifecycle(void);

void prove_timer_route(enum pit_route route);
void retire_legacy_interrupt_path(void);

void prove_pm_timer(void);
void prove_apic_timer(void);
void prove_tsc(void);
void retire_pit(void);
void prove_clocks_without_pit(void);
void prove_monotonic_time(void);

/*
 * The configuration window is passed in rather than read from a file-scope
 * static, which is what it did while this lived in kernel.c. A hidden read of
 * another translation unit's state is not something this split should preserve.
 */
void bring_up_pci(const struct acpi_mcfg *mcfg, bool present);

void prove_threads(void);
void prove_preemption(void);

void prove_framebuffer(const struct boot_framebuffer *framebuffer);
void prove_surface(void);
void draw_logo(void);

/*
 * Runs after the logo rather than before it. The logo is a splash and the
 * console replaces it, so the last thing on the screen at the end of boot is
 * the boot log rather than a picture.
 */
void prove_screen_console(void);

/*
 * The first device here that a person operates. Boot cannot wait for a
 * keystroke, so it injects one through the controller's own 0xD2 command and
 * proves the whole path - routing, vector, handler, decode, queue - with only
 * the finger simulated.
 */
void prove_keyboard(void);

/*
 * The first proof that runs the whole chain at once: scancodes injected through
 * the controller, decoded, fed to the shell, executed, drawn, and read back out
 * of the framebuffer. Nothing in it is simulated except the finger.
 */
void prove_shell(void);

#endif
