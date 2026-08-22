/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_BOOT_LEDGER_H
#define SAPOTE_BOOT_LEDGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/acpi.h>
#include <sapote/boot.h>
#include <sapote/paging.h>
#include <sapote/test.h>

/*
 * Sapote's boot graph and its evidence are deliberately bounded. Neither
 * plan construction nor validation may depend on the heap they are meant to
 * authorize.
 */
#define BOOT_LEDGER_STAGE_CAPACITY 46U
#define BOOT_LEDGER_RECEIPT_CAPACITY 46U
#define BOOT_STAGE_CAPABILITY_CAPACITY 21U
#define BOOT_STAGE_PROOF_COUNTER_CAPACITY 2U

enum boot_stage_id {
    BOOT_STAGE_INVALID = 0,
    BOOT_STAGE_EARLY_SERIAL,
    BOOT_STAGE_INTERRUPT_FOUNDATION,
    BOOT_STAGE_PURE_SELF_TESTS,
    BOOT_STAGE_BOOT_INFORMATION,
    BOOT_STAGE_FIRMWARE_DISCOVERY,
    BOOT_STAGE_DEVICE_WINDOWS,
    BOOT_STAGE_INTERRUPT_CONTROLLERS,
    BOOT_STAGE_FRAME_ALLOCATOR,
    BOOT_STAGE_PAGING_INSTALL,
    BOOT_STAGE_PAGING_PROOFS,
    BOOT_STAGE_FRAMEBUFFER_WC,
    BOOT_STAGE_MEMORY_RUNTIME,
    BOOT_STAGE_FRAMEBUFFER_OUTPUT,
    BOOT_STAGE_KEYBOARD,
    BOOT_STAGE_SHELL,
    BOOT_STAGE_EARLY_SCENARIO,
    BOOT_STAGE_INTERRUPT_PROOFS,
    BOOT_STAGE_TIMER_ROUTING,
    BOOT_STAGE_TIMER_CALIBRATION,
    BOOT_STAGE_PCI,
    BOOT_STAGE_THREADING,
    BOOT_STAGE_SCHEDULER,
    BOOT_STAGE_CLOSING_PROOFS,
    BOOT_STAGE_UI_FONT,
    BOOT_STAGE_POINTER_DECISION,
    BOOT_STAGE_POINTER_OUTCOME,
    BOOT_STAGE_UI_LAYOUT,
    BOOT_STAGE_DESKTOP_CONSTRUCTION,
    BOOT_STAGE_DESKTOP_ACTIVATION,
    BOOT_STAGE_FIRST_LIGHT_PROOF,
    BOOT_STAGE_PCI_RESOURCE_FOUNDATION,
    BOOT_STAGE_DYNAMIC_VECTOR_FOUNDATION,
    BOOT_STAGE_DMA_FOUNDATION,
    BOOT_STAGE_DEVICE_SUBSTRATE_PROOF,
    BOOT_STAGE_XHCI_FOUNDATION,
    BOOT_STAGE_XHCI_DESCRIPTOR_PROOF,
    BOOT_STAGE_NVME_FOUNDATION,
    BOOT_STAGE_NVME_READ_PROOF,
    BOOT_STAGE_FAT16_FOUNDATION,
    BOOT_STAGE_FILESYSTEM_FILE_PROOF,
    BOOT_STAGE_PROCESS_ADDRESS_SPACE_FOUNDATION,
    BOOT_STAGE_ELF64_LOADER_FOUNDATION,
    BOOT_STAGE_PROCESS_INSTALLED_PROOF,
    BOOT_STAGE_LINUX_SYSCALL_CPU_FOUNDATION,
    BOOT_STAGE_LINUX_IMAGE_STACK_FOUNDATION,
    BOOT_STAGE_LINUX_INSTALLED_PROOF,
    BOOT_STAGE_COUNT
};

enum boot_capability {
    BOOT_CAPABILITY_INVALID = 0,
    BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE,
    BOOT_CAPABILITY_BOOT_SELF_TESTS_COMPLETE,
    BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED,
    BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE,
    BOOT_CAPABILITY_ACPI_ROOT_VALIDATED,
    BOOT_CAPABILITY_INTERRUPT_TOPOLOGY_DISCOVERED,
    BOOT_CAPABILITY_CLOCKS_DISCOVERED,
    BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED,
    BOOT_CAPABILITY_PAGE_TABLES_INSTALLED,
    BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED,
    BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED,
    BOOT_CAPABILITY_HEAP_AVAILABLE,
    BOOT_CAPABILITY_IDT_INSTALLED,
    BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED,
    BOOT_CAPABILITY_INTERRUPTS_ENABLED,
    BOOT_CAPABILITY_INTERRUPT_ROUTING_PROVED,
    BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE,
    BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE,
    BOOT_CAPABILITY_FRAMEBUFFER_AVAILABILITY_DECIDED,
    BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED,
    BOOT_CAPABILITY_FRAMEBUFFER_SERIAL_FALLBACK,
    BOOT_CAPABILITY_SURFACE_AVAILABLE,
    BOOT_CAPABILITY_THREADING_AVAILABLE,
    BOOT_CAPABILITY_SCHEDULER_AVAILABLE,
    BOOT_CAPABILITY_SHELL_AVAILABLE,
    BOOT_CAPABILITY_BOOT_PROOFS_COMPLETE,
    BOOT_CAPABILITY_FRAMEBUFFER_OUTPUT_INSTALLED,
    BOOT_CAPABILITY_KEYBOARD_AVAILABLE,
    BOOT_CAPABILITY_UI_FONT_VERIFIED,
    BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED,
    BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE,
    BOOT_CAPABILITY_POINTER_INPUT_ABSENT,
    BOOT_CAPABILITY_UI_LAYOUT_VALIDATED,
    BOOT_CAPABILITY_DESKTOP_SHELL_AVAILABLE,
    BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED,
    BOOT_CAPABILITY_FIRST_LIGHT_INSTALLED_PROOF_COMPLETE,
    BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE,
    BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE,
    BOOT_CAPABILITY_DEVICE_SUBSTRATE_FIXTURE_ABSENT,
    BOOT_CAPABILITY_XHCI_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE,
    BOOT_CAPABILITY_XHCI_FIXTURE_ABSENT,
    BOOT_CAPABILITY_NVME_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE,
    BOOT_CAPABILITY_NVME_FIXTURE_ABSENT,
    BOOT_CAPABILITY_FAT16_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE,
    BOOT_CAPABILITY_FILESYSTEM_FIXTURE_ABSENT,
    BOOT_CAPABILITY_PRIVATE_ONE_FILE_READ_AVAILABLE,
    BOOT_CAPABILITY_PROCESS_ADDRESS_SPACE_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_ELF64_LOADER_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE,
    BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT,
    BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED,
    BOOT_CAPABILITY_LINUX_SYSCALL_CPU_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_LINUX_IMAGE_STACK_FOUNDATION_AVAILABLE,
    BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE,
    BOOT_CAPABILITY_LINUX_FIXTURE_ABSENT,
    BOOT_CAPABILITY_LINUX_OUTCOME_DECIDED,
    BOOT_CAPABILITY_COUNT
};

enum boot_phase {
    BOOT_PHASE_FOUNDATION = 0,
    BOOT_PHASE_DISCOVERY,
    BOOT_PHASE_CONTROLLERS,
    BOOT_PHASE_MEMORY_TRANSITION,
    BOOT_PHASE_RUNTIME,
    BOOT_PHASE_TIMERS,
    BOOT_PHASE_SERVICES,
    BOOT_PHASE_PROOFS,
    BOOT_PHASE_COUNT
};

enum boot_irreversible_class {
    BOOT_IRREVERSIBLE_NONE = 0,
    BOOT_IRREVERSIBLE_PAT_CR3,
    BOOT_IRREVERSIBLE_INTERRUPT_ENABLE,
    BOOT_IRREVERSIBLE_FRAMEBUFFER_OUTPUT,
    BOOT_IRREVERSIBLE_APIC_TIMER,
    BOOT_IRREVERSIBLE_SCHEDULER,
    BOOT_IRREVERSIBLE_COUNT
};

enum boot_ledger_status {
    BOOT_LEDGER_STATUS_OK = 0,
    BOOT_LEDGER_STATUS_NULL_ARGUMENT,
    BOOT_LEDGER_STATUS_EMPTY_PLAN,
    BOOT_LEDGER_STATUS_NOT_VALIDATED,
    BOOT_LEDGER_STATUS_ALREADY_EXECUTED,
    BOOT_LEDGER_STATUS_UNKNOWN_STAGE_IDENTIFIER,
    BOOT_LEDGER_STATUS_UNKNOWN_CAPABILITY,
    BOOT_LEDGER_STATUS_TOO_MANY_STAGES,
    BOOT_LEDGER_STATUS_TOO_MANY_RECEIPTS,
    BOOT_LEDGER_STATUS_TOO_MANY_STAGE_CAPABILITIES,
    BOOT_LEDGER_STATUS_INVALID_NEUTRAL_SKIP,
    BOOT_LEDGER_STATUS_DUPLICATE_STAGE,
    BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_REQUIREMENT,
    BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER,
    BOOT_LEDGER_STATUS_MISSING_CAPABILITY_PROVIDER,
    BOOT_LEDGER_STATUS_CAPABILITY_DEPENDENCY_CYCLE,
    BOOT_LEDGER_STATUS_INVALID_PHASE_TRANSITION,
    BOOT_LEDGER_STATUS_IRREVERSIBLE_STAGE_ORDERED_TOO_EARLY,
    BOOT_LEDGER_STATUS_REQUIRED_STAGE_SKIPPED,
    BOOT_LEDGER_STATUS_UNDECLARED_CAPABILITY_PROVIDED,
    BOOT_LEDGER_STATUS_STAGE_EXECUTED_BEFORE_REQUIREMENTS,
    BOOT_LEDGER_STATUS_REQUIRED_STAGE_FAILED,
    BOOT_LEDGER_STATUS_OPTIONAL_STAGE_FAILED,
    BOOT_LEDGER_STATUS_OPTIONAL_STAGE_SKIPPED,
    BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
    BOOT_LEDGER_STATUS_PLAN_FINGERPRINT_MISMATCH,
    BOOT_LEDGER_STATUS_INSTALLED_PLAN_DIFFERS_VALIDATED_PLAN,
    BOOT_LEDGER_STATUS_COUNT
};

enum boot_receipt_result {
    BOOT_RECEIPT_RAN = 0,
    BOOT_RECEIPT_SKIPPED,
    BOOT_RECEIPT_FAILED,
    BOOT_RECEIPT_RESULT_COUNT
};

struct boot_context;
struct boot_stage_descriptor;

struct boot_stage_result {
    enum boot_receipt_result result;
    enum boot_capability provided[BOOT_STAGE_CAPABILITY_CAPACITY];
    size_t provided_count;
    uint64_t proof_counters[BOOT_STAGE_PROOF_COUNTER_CAPACITY];
    size_t proof_counter_count;
};

typedef void (*boot_stage_execute_t)(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
);

struct boot_stage_descriptor {
    enum boot_stage_id id;
    const char *name;
    enum boot_capability required_capabilities[
        BOOT_STAGE_CAPABILITY_CAPACITY
    ];
    size_t required_capability_count;
    enum boot_capability provided_capabilities[
        BOOT_STAGE_CAPABILITY_CAPACITY
    ];
    size_t provided_capability_count;
    enum boot_capability skipped_capabilities[
        BOOT_STAGE_CAPABILITY_CAPACITY
    ];
    size_t skipped_capability_count;
    bool required;
    /* A declared neutral branch, such as valid pointer absence. */
    bool skip_preserves_health;
    enum boot_phase phase;
    enum boot_irreversible_class irreversible_class;
    boot_stage_execute_t execute;
};

struct boot_stage_receipt {
    enum boot_stage_id stage_id;
    uint32_t sequence;
    enum boot_capability provided_capabilities[
        BOOT_STAGE_CAPABILITY_CAPACITY
    ];
    size_t provided_capability_count;
    enum boot_ledger_status status;
    enum boot_receipt_result result;
    uint64_t proof_counters[BOOT_STAGE_PROOF_COUNTER_CAPACITY];
    size_t proof_counter_count;
};

struct boot_ledger {
    struct boot_stage_descriptor descriptors[BOOT_LEDGER_STAGE_CAPACITY];
    size_t descriptor_count;
    size_t canonical_order[BOOT_LEDGER_STAGE_CAPACITY];
    enum boot_stage_id validated_stage_ids[BOOT_LEDGER_STAGE_CAPACITY];
    size_t planned_count;
    struct boot_stage_receipt receipts[BOOT_LEDGER_RECEIPT_CAPACITY];
    size_t receipt_count;
    enum boot_capability established_capabilities[BOOT_CAPABILITY_COUNT];
    size_t established_capability_count;
    enum boot_ledger_status status;
    enum boot_stage_id refusal_stage;
    enum boot_capability refusal_capability;
    uint64_t validated_plan_fingerprint;
    uint64_t fingerprint;
    size_t executed_count;
    size_t optional_skip_count;
    bool validated;
    bool executed;
    bool degraded;
};

/*
 * One statically owned context spans kernel_main and the interactive shell.
 * Stages populate fields in dependency order; consumers reach them only after
 * the corresponding receipt exists. The context owns values, not arbitrary
 * pointers, and it never outlives the kernel image that owns its storage.
 */
struct boot_context {
    uint32_t multiboot_magic;
    uintptr_t multiboot_information_address;
    struct boot_information information;
    struct acpi_root acpi_root;
    struct acpi_madt acpi_madt;
    struct acpi_fadt acpi_fadt;
    struct acpi_mcfg acpi_mcfg;
    struct acpi_topology topology;
    struct paging_device_windows device_windows;
    struct kernel_test_context test_context;
    enum kernel_test_scenario test_scenario;
    const char *stage_failure_detail;
    bool mcfg_present;
};

void boot_ledger_reset(struct boot_ledger *ledger);
enum boot_ledger_status boot_ledger_add_stage(
    struct boot_ledger *ledger,
    const struct boot_stage_descriptor *descriptor
);
enum boot_ledger_status boot_ledger_validate(struct boot_ledger *ledger);
enum boot_ledger_status boot_ledger_execute(
    struct boot_ledger *ledger,
    struct boot_context *context
);
enum boot_ledger_status boot_ledger_verify_installed(
    struct boot_ledger *ledger,
    const struct boot_context *context
);
void boot_ledger_publish(const struct boot_ledger *ledger);
const struct boot_ledger *boot_ledger_installed(void);

void boot_stage_result_succeed(
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
);
void boot_stage_result_skip(
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
);
void boot_stage_result_fail(struct boot_stage_result *result);
bool boot_stage_result_provide(
    struct boot_stage_result *result,
    enum boot_capability capability
);

bool boot_ledger_has_capability(
    const struct boot_ledger *ledger,
    enum boot_capability capability
);
bool boot_ledger_fingerprint_valid(const struct boot_ledger *ledger);
const struct boot_stage_descriptor *boot_ledger_planned_stage_at(
    const struct boot_ledger *ledger,
    size_t index
);
const struct boot_stage_receipt *boot_ledger_receipt_at(
    const struct boot_ledger *ledger,
    size_t index
);
const struct boot_stage_receipt *boot_ledger_receipt_for(
    const struct boot_ledger *ledger,
    enum boot_stage_id stage
);

bool boot_ledger_self_test(void);
const char *boot_stage_name(enum boot_stage_id stage);
const char *boot_capability_string(enum boot_capability capability);
const char *boot_ledger_status_string(enum boot_ledger_status status);
const char *boot_receipt_result_string(enum boot_receipt_result result);

#endif
