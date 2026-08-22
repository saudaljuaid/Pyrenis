/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_TEST_H
#define SAPOTE_TEST_H

#include <stdbool.h>
#include <stdint.h>

#include <sapote/acpi.h>
#include <sapote/boot.h>
#include <sapote/interrupts.h>
#include <sapote/paging.h>

enum kernel_test_scenario {
    KERNEL_TEST_NONE = 0,
    KERNEL_TEST_NORMAL,
    KERNEL_TEST_BREAKPOINT,
    KERNEL_TEST_INVALID_OPCODE,
    KERNEL_TEST_PAGE_FAULT,
    KERNEL_TEST_IST,
    KERNEL_TEST_PIT,
    KERNEL_TEST_UNEXPECTED,
    KERNEL_TEST_DOUBLE_FAULT,
    KERNEL_TEST_APIC,
    KERNEL_TEST_IOAPIC,
    KERNEL_TEST_IOAPIC_LEVEL,
    KERNEL_TEST_RETIRED,
    KERNEL_TEST_APIC_TIMER,
    KERNEL_TEST_TSC,
    KERNEL_TEST_PM_TIMER,
    KERNEL_TEST_PIT_RETIRED,
    KERNEL_TEST_TIMERS,
    KERNEL_TEST_PAGING,
    KERNEL_TEST_HEAP,
    KERNEL_TEST_PCI,
    KERNEL_TEST_PCI_ECAM,
    KERNEL_TEST_THREADS,
    KERNEL_TEST_THREAD_GUARD,
    KERNEL_TEST_FRAMEBUFFER,
    KERNEL_TEST_SCREEN,
    KERNEL_TEST_KEYBOARD,
    KERNEL_TEST_SHELL,
    KERNEL_TEST_SURFACE,
    KERNEL_TEST_WRITE_COMBINING,
    KERNEL_TEST_DEVICE_WINDOWS,
    KERNEL_TEST_BOOT_LEDGER,
    KERNEL_TEST_FIRST_LIGHT,
    KERNEL_TEST_DEVICE_SUBSTRATE,
    KERNEL_TEST_XHCI,
    KERNEL_TEST_NVME,
    KERNEL_TEST_FILESYSTEM,
    KERNEL_TEST_PROCESS,
    KERNEL_TEST_LINUX_ABI,
    KERNEL_TEST_INVALID
};

/*
 * The bounded environment a scenario is allowed to inspect. A new discovered
 * window extends the registry, not kernel_test_run's signature, and every read
 * remains explicit rather than reaching into kernel.c's file scope.
 */
struct kernel_test_context {
    const struct acpi_mcfg *mcfg;
    const struct boot_framebuffer *framebuffer;
    const struct paging_device_windows *device_windows;
    bool mcfg_present;
};

enum kernel_test_scenario kernel_test_select(
    const struct boot_information *information
);
void kernel_test_run(
    enum kernel_test_scenario scenario,
    const struct kernel_test_context *context
);
_Noreturn void kernel_test_complete_normal(void);
struct boot_context;
_Noreturn void kernel_test_complete_boot_ledger(
    const struct boot_context *context
);
_Noreturn void kernel_test_complete_first_light(void);
_Noreturn void kernel_test_complete_device_substrate(void);
bool kernel_test_device_substrate_exit_self_test(void);
_Noreturn void kernel_test_complete_xhci(void);
bool kernel_test_xhci_exit_self_test(void);
_Noreturn void kernel_test_complete_nvme(void);
bool kernel_test_nvme_exit_self_test(void);
_Noreturn void kernel_test_complete_filesystem(void);
bool kernel_test_filesystem_exit_self_test(void);
_Noreturn void kernel_test_complete_process(void);
bool kernel_test_process_exit_self_test(void);
_Noreturn void kernel_test_complete_linux_abi(void);
bool kernel_test_linux_abi_exit_self_test(void);
bool kernel_test_handle_fatal_interrupt(const struct interrupt_frame *frame);
const char *kernel_test_scenario_name(enum kernel_test_scenario scenario);
_Noreturn void kernel_test_fail(const char *reason);

extern volatile uint8_t kernel_test_double_fault_armed;

#endif
