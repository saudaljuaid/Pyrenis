/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Boot has one policy here: validate the complete typed plan, execute it, then
 * verify the installed receipts before handing the machine to a test or shell.
 * Subsystem ordering lives in boot_plan.c as capability edges, not calls.
 */
#include <stdint.h>

#include <sapote/boot_ledger.h>
#include <sapote/boot_plan.h>
#include <sapote/console.h>
#include <sapote/shell.h>
#include <sapote/test.h>
#include <sapote/ui.h>

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information);

/*
 * Both objects must outlive kernel_main's early stack and the later shell.
 * Their ownership is singular and explicit: kernel_main initializes them,
 * boot-plan stages populate the context, and the published ledger is read-only.
 */
static struct boot_context installed_context;
static struct boot_ledger installed_ledger;

static void report_ledger_refusal(
    const struct boot_ledger *ledger,
    const struct boot_context *context
)
{
    console_write("Sapote: Boot Ledger refusal: ");
    console_write(boot_ledger_status_string(ledger->status));

    if (ledger->refusal_stage != BOOT_STAGE_INVALID) {
        console_write("; stage ");
        console_write(boot_stage_name(ledger->refusal_stage));
    }

    if (ledger->refusal_capability != BOOT_CAPABILITY_INVALID) {
        console_write("; capability ");
        console_write(boot_capability_string(ledger->refusal_capability));
    }

    if (context->stage_failure_detail != NULL) {
        console_write("; detail ");
        console_write(context->stage_failure_detail);
    }

    console_putc('\n');
}

_Noreturn void kernel_main(uint32_t magic, uintptr_t boot_information)
{
    enum boot_ledger_status status;

    /* Reversible bootstrap: named planner refusals need somewhere to speak. */
    console_initialize();
    boot_context_initialize(&installed_context, magic, boot_information);

    /* Pure and bounded; runs before PAT, WBINVD or CR3 replacement. */
    if (!boot_ledger_self_test()) {
        console_panic("Boot Ledger planner self-test failed");
    }

    status = boot_plan_build(&installed_ledger);
    if (status == BOOT_LEDGER_STATUS_OK) {
        status = boot_ledger_validate(&installed_ledger);
    }

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    status = boot_ledger_execute(&installed_ledger, &installed_context);

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    status = boot_ledger_verify_installed(&installed_ledger,
        &installed_context);

    if (status != BOOT_LEDGER_STATUS_OK) {
        report_ledger_refusal(&installed_ledger, &installed_context);
        console_panic(boot_ledger_status_string(status));
    }

    boot_ledger_publish(&installed_ledger);
    console_write("Sapote: Boot Ledger installed proof passed\n");
    if (ui_is_active() && ui_flush() != UI_STATUS_OK) {
        console_write("Sapote: First Light ledger status redraw failed\n");
    }

    if (installed_context.test_scenario == KERNEL_TEST_NORMAL) {
        kernel_test_complete_normal();
    }

    if (installed_context.test_scenario == KERNEL_TEST_BOOT_LEDGER) {
        kernel_test_complete_boot_ledger(&installed_context);
    }

    if (installed_context.test_scenario == KERNEL_TEST_FIRST_LIGHT) {
        kernel_test_complete_first_light();
    }

    if (installed_context.test_scenario == KERNEL_TEST_DEVICE_SUBSTRATE) {
        kernel_test_complete_device_substrate();
    }

    if (installed_context.test_scenario == KERNEL_TEST_XHCI) {
        kernel_test_complete_xhci();
    }

    if (installed_context.test_scenario == KERNEL_TEST_NVME) {
        kernel_test_complete_nvme();
    }

    if (installed_context.test_scenario == KERNEL_TEST_FILESYSTEM) {
        kernel_test_complete_filesystem();
    }

    if (installed_context.test_scenario == KERNEL_TEST_PROCESS) {
        kernel_test_complete_process();
    }

    if (installed_context.test_scenario == KERNEL_TEST_LINUX_ABI) {
        kernel_test_complete_linux_abi();
    }

    shell_run();
}
