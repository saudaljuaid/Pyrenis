/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/boot_ledger.h>
#include <sapote/device_substrate.h>
#include <sapote/dma.h>
#include <sapote/elf64.h>
#include <sapote/framebuffer.h>
#include <sapote/filesystem.h>
#include <sapote/interrupt_vector.h>
#include <sapote/linux_abi.h>
#include <sapote/msix.h>
#include <sapote/nvme.h>
#include <sapote/paging.h>
#include <sapote/pci_resource.h>
#include <sapote/pointer.h>
#include <sapote/process.h>
#include <sapote/ui.h>
#include <sapote/ui_font.h>
#include <sapote/xhci.h>

#define BOOT_FINGERPRINT_OFFSET UINT64_C(14695981039346656037)
#define BOOT_FINGERPRINT_PRIME UINT64_C(1099511628211)
#define BOOT_FINGERPRINT_VERSION UINT64_C(1)
#define BOOT_NO_PROVIDER BOOT_LEDGER_STAGE_CAPACITY

_Static_assert(BOOT_CAPABILITY_COUNT <= 64U,
    "every boot capability must fit the bounded private planner set");
_Static_assert(BOOT_LEDGER_RECEIPT_CAPACITY >= BOOT_LEDGER_STAGE_CAPACITY,
    "every planned stage needs room for one receipt");
_Static_assert(BOOT_LEDGER_STAGE_CAPACITY + 1U == BOOT_STAGE_COUNT,
    "the bounded ledger has exactly one slot for every non-invalid stage");

static const char *const stage_names[] = {
    "invalid stage",
    "early serial",
    "interrupt foundation",
    "pure boot self-tests",
    "boot information",
    "firmware discovery",
    "device-window registry",
    "interrupt controllers",
    "physical frame allocator",
    "PAT and page-table installation",
    "installed paging proofs",
    "independent framebuffer WC proof",
    "heap and paging runtime",
    "framebuffer output",
    "keyboard interrupt path",
    "interactive shell",
    "early scenario gate",
    "interrupt proofs",
    "interrupt routing",
    "timer calibration",
    "PCI access",
    "threading",
    "scheduler",
    "closing boot proofs",
    "First Light UI font",
    "pointer availability decision",
    "pointer availability outcome",
    "First Light layout",
    "desktop construction",
    "desktop activation",
    "First Light installed proof",
    "PCI resource ownership",
    "dynamic interrupt vectors",
    "DMA foundation",
    "installed device-substrate proof",
    "xHCI host-controller foundation",
    "installed xHCI descriptor proof",
    "NVMe block-controller foundation",
    "installed NVMe read proof",
    "bounded read-only FAT16 foundation",
    "installed FAT16 file-read proof",
    "private process address-space foundation",
    "bounded ELF64 loader foundation",
    "installed Ring 3 process proof",
    "Linux x86-64 syscall CPU foundation",
    "static BusyBox image and initial-stack foundation",
    "installed static BusyBox proof"
};

static const char *const capability_names[] = {
    "invalid capability",
    "early serial available",
    "boot self-tests complete",
    "boot information validated",
    "physical frame allocator available",
    "ACPI root validated",
    "interrupt topology discovered",
    "clocks discovered",
    "device-window registry validated",
    "page tables installed",
    "W^X proved",
    "installed device windows proved",
    "heap available",
    "IDT installed",
    "interrupt controllers configured",
    "interrupts enabled",
    "interrupt routing proved",
    "timer calibration complete",
    "PCI access available",
    "framebuffer availability decided",
    "framebuffer WC independently proved",
    "serial framebuffer fallback",
    "surface available",
    "threading available",
    "scheduler available",
    "shell available",
    "boot proofs complete",
    "framebuffer output installed",
    "keyboard available",
    "UI font verified",
    "pointer availability decided",
    "pointer input available",
    "pointer input absent",
    "UI layout validated",
    "desktop shell available",
    "desktop shell activated",
    "First Light installed proof complete",
    "PCI resource ownership available",
    "dynamic vector foundation available",
    "DMA foundation available",
    "device-substrate installed proof complete",
    "device-substrate fixture absent",
    "xHCI foundation available",
    "xHCI descriptor proof complete",
    "xHCI fixture absent",
    "NVMe foundation available",
    "NVMe read proof complete",
    "NVMe fixture absent",
    "FAT16 foundation available",
    "filesystem file proof complete",
    "filesystem fixture absent",
    "private one-file read available",
    "process address-space foundation available",
    "ELF64 loader foundation available",
    "process installed proof complete",
    "process fixture absent",
    "process outcome decided",
    "Linux syscall CPU foundation available",
    "Linux image and stack foundation available",
    "Linux installed proof complete",
    "Linux fixture absent",
    "Linux outcome decided"
};

static const char *const status_names[] = {
    "ok",
    "null argument",
    "empty boot plan",
    "boot plan not validated",
    "boot plan already executed",
    "unknown stage identifier",
    "unknown capability",
    "too many stages",
    "too many receipts",
    "too many stage capabilities",
    "neutral skip policy is invalid",
    "duplicate stage",
    "duplicate capability requirement",
    "duplicate capability provider",
    "missing capability provider",
    "capability dependency cycle",
    "invalid phase transition",
    "irreversible stage ordered too early",
    "required stage skipped",
    "undeclared capability provided",
    "stage executed before its requirements",
    "required stage failed",
    "optional stage failed",
    "optional stage skipped",
    "receipt mismatch",
    "plan fingerprint mismatch",
    "installed plan differs from validated plan"
};

static const char *const result_names[] = {
    "ran",
    "skipped",
    "failed"
};

_Static_assert(sizeof(stage_names) / sizeof(stage_names[0]) ==
    BOOT_STAGE_COUNT, "boot stage string table is incomplete");
_Static_assert(sizeof(capability_names) / sizeof(capability_names[0]) ==
    BOOT_CAPABILITY_COUNT, "boot capability string table is incomplete");
_Static_assert(sizeof(status_names) / sizeof(status_names[0]) ==
    BOOT_LEDGER_STATUS_COUNT, "boot ledger status string table is incomplete");
_Static_assert(sizeof(result_names) / sizeof(result_names[0]) ==
    BOOT_RECEIPT_RESULT_COUNT, "receipt result string table is incomplete");

static const struct boot_ledger *published_ledger;

static bool stage_is_known(enum boot_stage_id stage)
{
    return stage > BOOT_STAGE_INVALID && stage < BOOT_STAGE_COUNT;
}

static bool capability_is_known(enum boot_capability capability)
{
    return capability > BOOT_CAPABILITY_INVALID &&
        capability < BOOT_CAPABILITY_COUNT;
}

static bool descriptor_has_capability(
    const enum boot_capability *capabilities,
    size_t count,
    enum boot_capability capability
)
{
    for (size_t index = 0U; index < count; ++index) {
        if (capabilities[index] == capability) {
            return true;
        }
    }

    return false;
}

static bool optional_outcome_valid(
    bool stage_planned,
    bool complete,
    bool absent
)
{
    return !stage_planned || complete != absent;
}

static bool descriptor_requires(
    const struct boot_stage_descriptor *descriptor,
    enum boot_capability capability
)
{
    return descriptor_has_capability(descriptor->required_capabilities,
        descriptor->required_capability_count, capability);
}

static const struct boot_stage_descriptor *descriptor_for_stage(
    const struct boot_ledger *ledger,
    enum boot_stage_id stage
)
{
    if (ledger == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
        if (ledger->descriptors[index].id == stage) {
            return &ledger->descriptors[index];
        }
    }
    return NULL;
}

static void set_refusal(
    struct boot_ledger *ledger,
    enum boot_ledger_status status,
    enum boot_stage_id stage,
    enum boot_capability capability
)
{
    ledger->status = status;
    ledger->refusal_stage = stage;
    ledger->refusal_capability = capability;
}

static uint64_t fingerprint_u64(uint64_t fingerprint, uint64_t value)
{
    for (size_t byte = 0U; byte < sizeof(value); ++byte) {
        fingerprint ^= value & UINT64_C(0xFF);
        fingerprint *= BOOT_FINGERPRINT_PRIME;
        value >>= 8U;
    }

    return fingerprint;
}

static uint64_t fingerprint_capability_set(
    uint64_t fingerprint,
    const enum boot_capability *capabilities,
    size_t count,
    uint64_t tag
)
{
    fingerprint = fingerprint_u64(fingerprint, tag);
    fingerprint = fingerprint_u64(fingerprint, count);

    for (enum boot_capability capability = BOOT_CAPABILITY_INVALID + 1;
         capability < BOOT_CAPABILITY_COUNT;
         capability = (enum boot_capability)(capability + 1)) {
        if (descriptor_has_capability(capabilities, count, capability)) {
            fingerprint = fingerprint_u64(fingerprint, capability);
        }
    }

    return fingerprint;
}

static uint64_t fingerprint_plan(const struct boot_ledger *ledger)
{
    uint64_t fingerprint = BOOT_FINGERPRINT_OFFSET;

    fingerprint = fingerprint_u64(fingerprint, BOOT_FINGERPRINT_VERSION);
    fingerprint = fingerprint_u64(fingerprint, ledger->planned_count);

    for (size_t plan_index = 0U;
         plan_index < ledger->planned_count;
         ++plan_index) {
        const size_t descriptor_index = ledger->canonical_order[plan_index];
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[descriptor_index];

        fingerprint = fingerprint_u64(fingerprint, descriptor->id);
        fingerprint = fingerprint_u64(fingerprint, descriptor->required);
        fingerprint = fingerprint_u64(fingerprint,
            descriptor->skip_preserves_health);
        fingerprint = fingerprint_u64(fingerprint, descriptor->phase);
        fingerprint = fingerprint_u64(fingerprint,
            descriptor->irreversible_class);
        fingerprint = fingerprint_capability_set(fingerprint,
            descriptor->required_capabilities,
            descriptor->required_capability_count, UINT64_C(0xA1));
        fingerprint = fingerprint_capability_set(fingerprint,
            descriptor->provided_capabilities,
            descriptor->provided_capability_count, UINT64_C(0xA2));
        fingerprint = fingerprint_capability_set(fingerprint,
            descriptor->skipped_capabilities,
            descriptor->skipped_capability_count, UINT64_C(0xA3));
    }

    return fingerprint;
}

static uint64_t fingerprint_receipts(const struct boot_ledger *ledger)
{
    uint64_t fingerprint = ledger->validated_plan_fingerprint;

    fingerprint = fingerprint_u64(fingerprint, UINT64_C(0xB001ED6E));
    fingerprint = fingerprint_u64(fingerprint, ledger->receipt_count);

    for (size_t index = 0U; index < ledger->receipt_count; ++index) {
        const struct boot_stage_receipt *receipt = &ledger->receipts[index];

        fingerprint = fingerprint_u64(fingerprint, receipt->stage_id);
        fingerprint = fingerprint_u64(fingerprint, receipt->sequence);
        fingerprint = fingerprint_u64(fingerprint, receipt->result);
        fingerprint = fingerprint_u64(fingerprint, receipt->status);
        fingerprint = fingerprint_capability_set(fingerprint,
            receipt->provided_capabilities,
            receipt->provided_capability_count, UINT64_C(0xB1));
    }

    return fingerprint;
}

static bool irreversible_requirement_at(
    enum boot_irreversible_class irreversible_class,
    size_t index,
    enum boot_capability *capability
)
{
    static const enum boot_capability pat_cr3[] = {
        BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED,
        BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE
    };
    static const enum boot_capability interrupt_enable[] = {
        BOOT_CAPABILITY_IDT_INSTALLED,
        BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED
    };
    static const enum boot_capability framebuffer_output[] = {
        BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED
    };
    static const enum boot_capability apic_timer[] = {
        BOOT_CAPABILITY_IDT_INSTALLED,
        BOOT_CAPABILITY_INTERRUPT_CONTROLLERS_CONFIGURED,
        BOOT_CAPABILITY_INTERRUPTS_ENABLED,
        BOOT_CAPABILITY_CLOCKS_DISCOVERED
    };
    static const enum boot_capability scheduler[] = {
        BOOT_CAPABILITY_HEAP_AVAILABLE,
        BOOT_CAPABILITY_THREADING_AVAILABLE,
        BOOT_CAPABILITY_TIMER_CALIBRATION_COMPLETE,
        BOOT_CAPABILITY_INTERRUPTS_ENABLED
    };
    const enum boot_capability *requirements = NULL;
    size_t count = 0U;

    switch (irreversible_class) {
    case BOOT_IRREVERSIBLE_NONE:
        return false;
    case BOOT_IRREVERSIBLE_PAT_CR3:
        requirements = pat_cr3;
        count = sizeof(pat_cr3) / sizeof(pat_cr3[0]);
        break;
    case BOOT_IRREVERSIBLE_INTERRUPT_ENABLE:
        requirements = interrupt_enable;
        count = sizeof(interrupt_enable) / sizeof(interrupt_enable[0]);
        break;
    case BOOT_IRREVERSIBLE_FRAMEBUFFER_OUTPUT:
        requirements = framebuffer_output;
        count = sizeof(framebuffer_output) / sizeof(framebuffer_output[0]);
        break;
    case BOOT_IRREVERSIBLE_APIC_TIMER:
        requirements = apic_timer;
        count = sizeof(apic_timer) / sizeof(apic_timer[0]);
        break;
    case BOOT_IRREVERSIBLE_SCHEDULER:
        requirements = scheduler;
        count = sizeof(scheduler) / sizeof(scheduler[0]);
        break;
    case BOOT_IRREVERSIBLE_COUNT:
    default:
        return false;
    }

    if (index >= count) {
        return false;
    }

    *capability = requirements[index];
    return true;
}

void boot_ledger_reset(struct boot_ledger *ledger)
{
    if (ledger == NULL) {
        return;
    }

    for (size_t byte = 0U; byte < sizeof(*ledger); ++byte) {
        ((uint8_t *)ledger)[byte] = 0U;
    }

    ledger->status = BOOT_LEDGER_STATUS_NOT_VALIDATED;
    ledger->refusal_stage = BOOT_STAGE_INVALID;
    ledger->refusal_capability = BOOT_CAPABILITY_INVALID;
}

enum boot_ledger_status boot_ledger_add_stage(
    struct boot_ledger *ledger,
    const struct boot_stage_descriptor *descriptor
)
{
    if (ledger == NULL || descriptor == NULL) {
        return BOOT_LEDGER_STATUS_NULL_ARGUMENT;
    }

    if (ledger->descriptor_count >= BOOT_LEDGER_STAGE_CAPACITY) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_TOO_MANY_STAGES,
            descriptor->id, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    ledger->descriptors[ledger->descriptor_count] = *descriptor;
    ledger->descriptor_count += 1U;
    ledger->validated = false;
    ledger->executed = false;
    ledger->status = BOOT_LEDGER_STATUS_NOT_VALIDATED;
    return BOOT_LEDGER_STATUS_OK;
}

enum boot_ledger_status boot_ledger_validate(struct boot_ledger *ledger)
{
    size_t providers[BOOT_CAPABILITY_COUNT];
    bool planned[BOOT_LEDGER_STAGE_CAPACITY] = {false};

    if (ledger == NULL) {
        return BOOT_LEDGER_STATUS_NULL_ARGUMENT;
    }

    ledger->validated = false;
    ledger->executed = false;
    ledger->planned_count = 0U;
    ledger->receipt_count = 0U;
    ledger->refusal_stage = BOOT_STAGE_INVALID;
    ledger->refusal_capability = BOOT_CAPABILITY_INVALID;

    if (ledger->descriptor_count == 0U) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_EMPTY_PLAN,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (ledger->descriptor_count > BOOT_LEDGER_STAGE_CAPACITY) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_TOO_MANY_STAGES,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    for (size_t capability = 0U;
         capability < BOOT_CAPABILITY_COUNT;
         ++capability) {
        providers[capability] = BOOT_NO_PROVIDER;
    }

    for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[index];

        if (!stage_is_known(descriptor->id)) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_UNKNOWN_STAGE_IDENTIFIER,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        if (descriptor->phase >= BOOT_PHASE_COUNT ||
            descriptor->irreversible_class >= BOOT_IRREVERSIBLE_COUNT ||
            descriptor->name == NULL || descriptor->execute == NULL) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_INVALID_PHASE_TRANSITION,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        if (descriptor->required_capability_count >
                BOOT_STAGE_CAPABILITY_CAPACITY ||
            descriptor->provided_capability_count >
                BOOT_STAGE_CAPABILITY_CAPACITY ||
            descriptor->skipped_capability_count >
                BOOT_STAGE_CAPABILITY_CAPACITY) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_TOO_MANY_STAGE_CAPABILITIES,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        if (descriptor->skip_preserves_health &&
            (descriptor->required ||
             descriptor->skipped_capability_count == 0U)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_INVALID_NEUTRAL_SKIP,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        for (size_t other = 0U; other < index; ++other) {
            if (ledger->descriptors[other].id == descriptor->id) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_DUPLICATE_STAGE,
                    descriptor->id, BOOT_CAPABILITY_INVALID);
                return ledger->status;
            }
        }

        for (size_t requirement = 0U;
             requirement < descriptor->required_capability_count;
             ++requirement) {
            const enum boot_capability capability =
                descriptor->required_capabilities[requirement];

            if (!capability_is_known(capability)) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_UNKNOWN_CAPABILITY,
                    descriptor->id, capability);
                return ledger->status;
            }

            for (size_t earlier = 0U; earlier < requirement; ++earlier) {
                if (descriptor->required_capabilities[earlier] == capability) {
                    set_refusal(ledger,
                        BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_REQUIREMENT,
                        descriptor->id, capability);
                    return ledger->status;
                }
            }
        }

        for (size_t capability_index = 0U;
             capability_index < descriptor->provided_capability_count +
                descriptor->skipped_capability_count;
             ++capability_index) {
            const bool skipped = capability_index >=
                descriptor->provided_capability_count;
            const size_t source_index = skipped ?
                capability_index - descriptor->provided_capability_count :
                capability_index;
            const enum boot_capability capability = skipped ?
                descriptor->skipped_capabilities[source_index] :
                descriptor->provided_capabilities[source_index];

            if (!capability_is_known(capability)) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_UNKNOWN_CAPABILITY,
                    descriptor->id, capability);
                return ledger->status;
            }

            const bool common_outcome = skipped &&
                providers[capability] == index &&
                descriptor_has_capability(
                    descriptor->provided_capabilities,
                    descriptor->provided_capability_count, capability) &&
                !descriptor_has_capability(
                    descriptor->skipped_capabilities, source_index,
                    capability);

            if (providers[capability] != BOOT_NO_PROVIDER &&
                !common_outcome) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER,
                    descriptor->id, capability);
                return ledger->status;
            }
            if (providers[capability] == BOOT_NO_PROVIDER) {
                providers[capability] = index;
            }
        }
    }

    /* Missing providers are a more precise refusal than an ordering class. */
    for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[index];

        for (size_t requirement = 0U;
             requirement < descriptor->required_capability_count;
             ++requirement) {
            const enum boot_capability capability =
                descriptor->required_capabilities[requirement];

            if (providers[capability] == BOOT_NO_PROVIDER) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_MISSING_CAPABILITY_PROVIDER,
                    descriptor->id, capability);
                return ledger->status;
            }
        }
    }

    /* Irreversible classes carry non-negotiable semantic prerequisites. */
    for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[index];

        for (size_t requirement = 0U;; ++requirement) {
            enum boot_capability capability;

            if (!irreversible_requirement_at(descriptor->irreversible_class,
                    requirement, &capability)) {
                break;
            }

            if (!descriptor_requires(descriptor, capability) ||
                providers[capability] == BOOT_NO_PROVIDER ||
                ledger->descriptors[providers[capability]].phase >
                    descriptor->phase) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_IRREVERSIBLE_STAGE_ORDERED_TOO_EARLY,
                    descriptor->id, capability);
                return ledger->status;
            }
        }
    }

    while (ledger->planned_count < ledger->descriptor_count) {
        size_t selected = BOOT_NO_PROVIDER;

        for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
            const struct boot_stage_descriptor *descriptor =
                &ledger->descriptors[index];
            bool ready = !planned[index];

            if (!ready) {
                continue;
            }

            for (size_t requirement = 0U;
                 ready && requirement < descriptor->required_capability_count;
                 ++requirement) {
                const size_t provider = providers[
                    descriptor->required_capabilities[requirement]
                ];

                ready = provider != BOOT_NO_PROVIDER && planned[provider];
            }

            if (!ready) {
                continue;
            }

            if (selected == BOOT_NO_PROVIDER ||
                descriptor->phase < ledger->descriptors[selected].phase ||
                (descriptor->phase == ledger->descriptors[selected].phase &&
                 descriptor->id < ledger->descriptors[selected].id)) {
                selected = index;
            }
        }

        if (selected == BOOT_NO_PROVIDER) {
            enum boot_stage_id cycle_stage = BOOT_STAGE_INVALID;

            for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
                if (!planned[index] &&
                    (cycle_stage == BOOT_STAGE_INVALID ||
                     ledger->descriptors[index].id < cycle_stage)) {
                    cycle_stage = ledger->descriptors[index].id;
                }
            }

            set_refusal(ledger,
                BOOT_LEDGER_STATUS_CAPABILITY_DEPENDENCY_CYCLE,
                cycle_stage, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        ledger->canonical_order[ledger->planned_count] = selected;
        ledger->validated_stage_ids[ledger->planned_count] =
            ledger->descriptors[selected].id;
        ledger->planned_count += 1U;
        planned[selected] = true;
    }

    /* Cycles win over phase errors because they explain the real deadlock. */
    for (size_t index = 0U; index < ledger->descriptor_count; ++index) {
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[index];

        for (size_t requirement = 0U;
             requirement < descriptor->required_capability_count;
             ++requirement) {
            const enum boot_capability capability =
                descriptor->required_capabilities[requirement];

            if (ledger->descriptors[providers[capability]].phase >
                descriptor->phase) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_INVALID_PHASE_TRANSITION,
                    descriptor->id, capability);
                return ledger->status;
            }
        }
    }

    ledger->validated_plan_fingerprint = fingerprint_plan(ledger);
    ledger->fingerprint = 0U;
    ledger->validated = true;
    ledger->status = BOOT_LEDGER_STATUS_OK;
    return ledger->status;
}

void boot_stage_result_succeed(
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    if (descriptor == NULL || result == NULL) {
        return;
    }

    for (size_t byte = 0U; byte < sizeof(*result); ++byte) {
        ((uint8_t *)result)[byte] = 0U;
    }

    result->result = BOOT_RECEIPT_RAN;
    result->provided_count = descriptor->provided_capability_count;

    for (size_t index = 0U; index < result->provided_count; ++index) {
        result->provided[index] = descriptor->provided_capabilities[index];
    }
}

void boot_stage_result_skip(
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    if (descriptor == NULL || result == NULL) {
        return;
    }

    for (size_t byte = 0U; byte < sizeof(*result); ++byte) {
        ((uint8_t *)result)[byte] = 0U;
    }

    result->result = BOOT_RECEIPT_SKIPPED;
    result->provided_count = descriptor->skipped_capability_count;

    for (size_t index = 0U; index < result->provided_count; ++index) {
        result->provided[index] = descriptor->skipped_capabilities[index];
    }
}

void boot_stage_result_fail(struct boot_stage_result *result)
{
    if (result == NULL) {
        return;
    }

    for (size_t byte = 0U; byte < sizeof(*result); ++byte) {
        ((uint8_t *)result)[byte] = 0U;
    }

    result->result = BOOT_RECEIPT_FAILED;
}

bool boot_stage_result_provide(
    struct boot_stage_result *result,
    enum boot_capability capability
)
{
    if (result == NULL ||
        result->provided_count >= BOOT_STAGE_CAPABILITY_CAPACITY) {
        return false;
    }

    result->provided[result->provided_count] = capability;
    result->provided_count += 1U;
    return true;
}

bool boot_ledger_has_capability(
    const struct boot_ledger *ledger,
    enum boot_capability capability
)
{
    if (ledger == NULL || !capability_is_known(capability)) {
        return false;
    }

    return descriptor_has_capability(ledger->established_capabilities,
        ledger->established_capability_count, capability);
}

bool boot_ledger_fingerprint_valid(const struct boot_ledger *ledger)
{
    return ledger != NULL && ledger->validated && ledger->executed &&
        fingerprint_plan(ledger) == ledger->validated_plan_fingerprint &&
        fingerprint_receipts(ledger) == ledger->fingerprint;
}

static bool requirements_established(
    const struct boot_ledger *ledger,
    const struct boot_stage_descriptor *descriptor,
    enum boot_capability *missing
)
{
    for (size_t index = 0U;
         index < descriptor->required_capability_count;
         ++index) {
        if (!boot_ledger_has_capability(ledger,
                descriptor->required_capabilities[index])) {
            *missing = descriptor->required_capabilities[index];
            return false;
        }
    }

    return true;
}

static enum boot_ledger_status validate_result(
    const struct boot_stage_descriptor *descriptor,
    const struct boot_stage_result *result,
    enum boot_capability *bad_capability
)
{
    const enum boot_capability *declared = NULL;
    size_t declared_count = 0U;

    if (result->result == BOOT_RECEIPT_RAN) {
        declared = descriptor->provided_capabilities;
        declared_count = descriptor->provided_capability_count;
    } else if (result->result == BOOT_RECEIPT_SKIPPED) {
        declared = descriptor->skipped_capabilities;
        declared_count = descriptor->skipped_capability_count;
    } else if (result->result != BOOT_RECEIPT_FAILED) {
        return BOOT_LEDGER_STATUS_RECEIPT_MISMATCH;
    }

    if (result->provided_count > BOOT_STAGE_CAPABILITY_CAPACITY ||
        result->proof_counter_count > BOOT_STAGE_PROOF_COUNTER_CAPACITY) {
        return BOOT_LEDGER_STATUS_RECEIPT_MISMATCH;
    }

    for (size_t index = 0U; index < result->provided_count; ++index) {
        if (!descriptor_has_capability(declared, declared_count,
                result->provided[index])) {
            *bad_capability = result->provided[index];
            return BOOT_LEDGER_STATUS_UNDECLARED_CAPABILITY_PROVIDED;
        }
    }

    if (result->provided_count != declared_count) {
        return BOOT_LEDGER_STATUS_RECEIPT_MISMATCH;
    }

    for (size_t index = 0U; index < declared_count; ++index) {
        if (!descriptor_has_capability(result->provided,
                result->provided_count, declared[index])) {
            *bad_capability = declared[index];
            return BOOT_LEDGER_STATUS_RECEIPT_MISMATCH;
        }
    }

    return BOOT_LEDGER_STATUS_OK;
}

static enum boot_ledger_status append_receipt(
    struct boot_ledger *ledger,
    const struct boot_stage_descriptor *descriptor,
    const struct boot_stage_result *result,
    enum boot_ledger_status receipt_status
)
{
    struct boot_stage_receipt *receipt;

    if (ledger->receipt_count >= BOOT_LEDGER_RECEIPT_CAPACITY) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_TOO_MANY_RECEIPTS,
            descriptor->id, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    receipt = &ledger->receipts[ledger->receipt_count];

    for (size_t byte = 0U; byte < sizeof(*receipt); ++byte) {
        ((uint8_t *)receipt)[byte] = 0U;
    }

    receipt->stage_id = descriptor->id;
    receipt->sequence = (uint32_t)(ledger->receipt_count + 1U);
    receipt->provided_capability_count = result->provided_count;
    receipt->status = receipt_status;
    receipt->result = result->result;
    receipt->proof_counter_count = result->proof_counter_count;

    for (size_t index = 0U; index < result->provided_count; ++index) {
        receipt->provided_capabilities[index] = result->provided[index];

        if (!boot_ledger_has_capability(ledger, result->provided[index])) {
            if (ledger->established_capability_count >=
                BOOT_CAPABILITY_COUNT) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                    descriptor->id, result->provided[index]);
                return ledger->status;
            }

            ledger->established_capabilities[
                ledger->established_capability_count
            ] = result->provided[index];
            ledger->established_capability_count += 1U;
        }
    }

    for (size_t index = 0U; index < result->proof_counter_count; ++index) {
        receipt->proof_counters[index] = result->proof_counters[index];
    }

    ledger->receipt_count += 1U;

    if (result->result != BOOT_RECEIPT_SKIPPED) {
        ledger->executed_count += 1U;
    }

    if (result->result != BOOT_RECEIPT_RAN) {
        if (result->result != BOOT_RECEIPT_SKIPPED ||
            !descriptor->skip_preserves_health) {
            ledger->degraded = true;
        }

        if (result->result == BOOT_RECEIPT_SKIPPED) {
            ledger->optional_skip_count += 1U;
        }
    }

    return BOOT_LEDGER_STATUS_OK;
}

static enum boot_ledger_status execute_stage(
    struct boot_ledger *ledger,
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor
)
{
    struct boot_stage_result result;
    enum boot_capability missing = BOOT_CAPABILITY_INVALID;
    enum boot_capability bad_capability = BOOT_CAPABILITY_INVALID;
    enum boot_ledger_status status;

    context->stage_failure_detail = NULL;

    if (!requirements_established(ledger, descriptor, &missing)) {
        if (descriptor->required) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_STAGE_EXECUTED_BEFORE_REQUIREMENTS,
                descriptor->id, missing);
            return ledger->status;
        }

        boot_stage_result_skip(descriptor, &result);
        return append_receipt(ledger, descriptor, &result,
            BOOT_LEDGER_STATUS_OPTIONAL_STAGE_SKIPPED);
    }

    boot_stage_result_fail(&result);
    descriptor->execute(context, descriptor, &result);
    status = validate_result(descriptor, &result, &bad_capability);

    if (status != BOOT_LEDGER_STATUS_OK) {
        set_refusal(ledger, status, descriptor->id, bad_capability);
        return ledger->status;
    }

    if (result.result == BOOT_RECEIPT_SKIPPED && descriptor->required) {
        (void)append_receipt(ledger, descriptor, &result,
            BOOT_LEDGER_STATUS_REQUIRED_STAGE_SKIPPED);
        set_refusal(ledger, BOOT_LEDGER_STATUS_REQUIRED_STAGE_SKIPPED,
            descriptor->id, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (result.result == BOOT_RECEIPT_FAILED) {
        status = descriptor->required ?
            BOOT_LEDGER_STATUS_REQUIRED_STAGE_FAILED :
            BOOT_LEDGER_STATUS_OPTIONAL_STAGE_FAILED;
        (void)append_receipt(ledger, descriptor, &result, status);

        if (descriptor->required) {
            set_refusal(ledger, status, descriptor->id,
                BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        ledger->degraded = true;
        return BOOT_LEDGER_STATUS_OK;
    }

    status = result.result == BOOT_RECEIPT_SKIPPED ?
        BOOT_LEDGER_STATUS_OPTIONAL_STAGE_SKIPPED :
        BOOT_LEDGER_STATUS_OK;
    return append_receipt(ledger, descriptor, &result, status);
}

enum boot_ledger_status boot_ledger_execute(
    struct boot_ledger *ledger,
    struct boot_context *context
)
{
    if (ledger == NULL || context == NULL) {
        return BOOT_LEDGER_STATUS_NULL_ARGUMENT;
    }

    if (!ledger->validated) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_NOT_VALIDATED,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (ledger->executed) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_ALREADY_EXECUTED,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (fingerprint_plan(ledger) != ledger->validated_plan_fingerprint) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_PLAN_FINGERPRINT_MISMATCH,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    ledger->receipt_count = 0U;
    ledger->established_capability_count = 0U;
    ledger->executed_count = 0U;
    ledger->optional_skip_count = 0U;
    ledger->degraded = false;

    for (size_t plan_index = 0U;
         plan_index < ledger->planned_count;
         ++plan_index) {
        const size_t descriptor_index = ledger->canonical_order[plan_index];
        const struct boot_stage_descriptor *descriptor =
            &ledger->descriptors[descriptor_index];
        enum boot_ledger_status status;

        if (descriptor->id != ledger->validated_stage_ids[plan_index]) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_INSTALLED_PLAN_DIFFERS_VALIDATED_PLAN,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        status = execute_stage(ledger, context, descriptor);

        if (status != BOOT_LEDGER_STATUS_OK) {
            return status;
        }
    }

    ledger->fingerprint = fingerprint_receipts(ledger);
    ledger->executed = true;
    ledger->status = BOOT_LEDGER_STATUS_OK;
    return ledger->status;
}

static enum boot_ledger_status verify_receipt_set(
    struct boot_ledger *ledger
)
{
    enum boot_capability capabilities[BOOT_CAPABILITY_COUNT];
    size_t capability_count = 0U;
    bool seen[BOOT_STAGE_COUNT] = {false};

    if (ledger->receipt_count != ledger->planned_count) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    for (size_t index = 0U; index < ledger->receipt_count; ++index) {
        const struct boot_stage_receipt *receipt = &ledger->receipts[index];
        const struct boot_stage_descriptor *descriptor =
            boot_ledger_planned_stage_at(ledger, index);
        const enum boot_capability *declared = NULL;
        size_t declared_count = 0U;

        if (!stage_is_known(receipt->stage_id)) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_UNKNOWN_STAGE_IDENTIFIER,
                receipt->stage_id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        if (descriptor == NULL || receipt->stage_id != descriptor->id ||
            receipt->sequence != index + 1U || seen[receipt->stage_id]) {
            set_refusal(ledger,
                BOOT_LEDGER_STATUS_INSTALLED_PLAN_DIFFERS_VALIDATED_PLAN,
                receipt->stage_id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        seen[receipt->stage_id] = true;

        if (receipt->provided_capability_count >
                BOOT_STAGE_CAPABILITY_CAPACITY ||
            receipt->proof_counter_count >
                BOOT_STAGE_PROOF_COUNTER_CAPACITY ||
            (receipt->result == BOOT_RECEIPT_RAN &&
             receipt->status != BOOT_LEDGER_STATUS_OK) ||
            (receipt->result == BOOT_RECEIPT_SKIPPED &&
             receipt->status != BOOT_LEDGER_STATUS_OPTIONAL_STAGE_SKIPPED) ||
            (receipt->result == BOOT_RECEIPT_FAILED &&
             receipt->status != BOOT_LEDGER_STATUS_OPTIONAL_STAGE_FAILED) ||
            (size_t)receipt->result >= BOOT_RECEIPT_RESULT_COUNT) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        if (descriptor->required &&
            (receipt->result != BOOT_RECEIPT_RAN ||
             receipt->status != BOOT_LEDGER_STATUS_OK)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        for (size_t requirement = 0U;
             receipt->result != BOOT_RECEIPT_SKIPPED &&
             requirement < descriptor->required_capability_count;
             ++requirement) {
            const enum boot_capability capability =
                descriptor->required_capabilities[requirement];

            if (!descriptor_has_capability(capabilities, capability_count,
                    capability)) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_STAGE_EXECUTED_BEFORE_REQUIREMENTS,
                    descriptor->id, capability);
                return ledger->status;
            }
        }

        if (receipt->result == BOOT_RECEIPT_RAN) {
            declared = descriptor->provided_capabilities;
            declared_count = descriptor->provided_capability_count;
        } else if (receipt->result == BOOT_RECEIPT_SKIPPED) {
            declared = descriptor->skipped_capabilities;
            declared_count = descriptor->skipped_capability_count;
        }

        if (receipt->provided_capability_count != declared_count) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                descriptor->id, BOOT_CAPABILITY_INVALID);
            return ledger->status;
        }

        for (size_t provided = 0U;
             provided < receipt->provided_capability_count;
             ++provided) {
            const enum boot_capability capability =
                receipt->provided_capabilities[provided];

            if (!capability_is_known(capability)) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_UNKNOWN_CAPABILITY,
                    descriptor->id, capability);
                return ledger->status;
            }

            if (!descriptor_has_capability(declared, declared_count,
                    capability)) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_UNDECLARED_CAPABILITY_PROVIDED,
                    descriptor->id, capability);
                return ledger->status;
            }

            for (size_t earlier = 0U; earlier < provided; ++earlier) {
                if (receipt->provided_capabilities[earlier] == capability) {
                    set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                        descriptor->id, capability);
                    return ledger->status;
                }
            }

            if (descriptor_has_capability(capabilities, capability_count,
                    capability)) {
                set_refusal(ledger,
                    BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER,
                    descriptor->id, capability);
                return ledger->status;
            }

            if (capability_count >= BOOT_CAPABILITY_COUNT) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                    descriptor->id, capability);
                return ledger->status;
            }

            capabilities[capability_count] = capability;
            capability_count += 1U;
        }

        for (size_t declared_index = 0U;
             declared_index < declared_count;
             ++declared_index) {
            if (!descriptor_has_capability(receipt->provided_capabilities,
                    receipt->provided_capability_count,
                    declared[declared_index])) {
                set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                    descriptor->id, declared[declared_index]);
                return ledger->status;
            }
        }
    }

    if (capability_count != ledger->established_capability_count) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    for (size_t index = 0U; index < capability_count; ++index) {
        if (!boot_ledger_has_capability(ledger, capabilities[index])) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_INVALID, capabilities[index]);
            return ledger->status;
        }
    }

    return BOOT_LEDGER_STATUS_OK;
}

enum boot_ledger_status boot_ledger_verify_installed(
    struct boot_ledger *ledger,
    const struct boot_context *context
)
{
    const struct boot_stage_receipt *device_windows_receipt;
    const struct boot_stage_receipt *paging_install_receipt;
    const struct boot_stage_receipt *paging_proofs_receipt;
    size_t failed_window = 0U;
    struct paging_audit audit;
    enum paging_status paging_status;
    enum boot_ledger_status status;

    if (ledger == NULL || context == NULL) {
        return BOOT_LEDGER_STATUS_NULL_ARGUMENT;
    }

    if (!ledger->validated || !ledger->executed) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_NOT_VALIDATED,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (fingerprint_plan(ledger) != ledger->validated_plan_fingerprint) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_PLAN_FINGERPRINT_MISMATCH,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    status = verify_receipt_set(ledger);

    if (status != BOOT_LEDGER_STATUS_OK) {
        return status;
    }

    if (fingerprint_receipts(ledger) != ledger->fingerprint) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_PLAN_FINGERPRINT_MISMATCH,
            BOOT_STAGE_INVALID, BOOT_CAPABILITY_INVALID);
        return ledger->status;
    }

    if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PAGE_TABLES_INSTALLED)) {
        device_windows_receipt = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_DEVICE_WINDOWS);
        paging_install_receipt = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_PAGING_INSTALL);
        paging_proofs_receipt = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_PAGING_PROOFS);
        paging_status = paging_verify();

        if (paging_status == PAGING_STATUS_OK) {
            paging_status = paging_verify_device_windows(
                &context->device_windows, &failed_window);
        }

        if (paging_status == PAGING_STATUS_OK) {
            paging_status = paging_audit_hierarchy(&audit);
        }

        if (paging_status != PAGING_STATUS_OK ||
            audit.write_execute_leaves != 0U ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_WRITE_XOR_EXECUTE_PROVED) ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_PAGING_PROOFS,
                BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED);
            return ledger->status;
        }

        if (device_windows_receipt == NULL ||
            device_windows_receipt->result != BOOT_RECEIPT_RAN ||
            device_windows_receipt->proof_counter_count != 1U ||
            device_windows_receipt->proof_counters[0] !=
                context->device_windows.count) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DEVICE_WINDOWS,
                BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED);
            return ledger->status;
        }

        if (paging_install_receipt == NULL ||
            paging_install_receipt->result != BOOT_RECEIPT_RAN ||
            paging_install_receipt->proof_counter_count != 2U ||
            paging_install_receipt->proof_counters[0] == 0U ||
            paging_install_receipt->proof_counters[1] !=
                context->device_windows.count) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_PAGING_INSTALL,
                BOOT_CAPABILITY_PAGE_TABLES_INSTALLED);
            return ledger->status;
        }

        if (paging_proofs_receipt == NULL ||
            paging_proofs_receipt->result != BOOT_RECEIPT_RAN ||
            paging_proofs_receipt->proof_counter_count != 2U ||
            paging_proofs_receipt->proof_counters[0] == 0U ||
            paging_proofs_receipt->proof_counters[1] !=
                context->device_windows.count) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_PAGING_PROOFS,
                BOOT_CAPABILITY_INSTALLED_DEVICE_WINDOWS_PROVED);
            return ledger->status;
        }
    }

    if (framebuffer_is_active() &&
        (!boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED) ||
         !boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_SURFACE_AVAILABLE))) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_FRAMEBUFFER_OUTPUT,
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED);
        return ledger->status;
    }

    if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE) ||
        boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE)) {
        const struct pci_resource_state resources = pci_resource_get_state();
        const struct interrupt_vector_state vectors =
            interrupt_vector_get_state();
        const struct dma_state installed_dma = dma_get_state();

        if (!boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE) ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_DYNAMIC_VECTOR_FOUNDATION_AVAILABLE) ||
            !boot_ledger_has_capability(ledger,
                BOOT_CAPABILITY_DMA_FOUNDATION_AVAILABLE) ||
            pci_resource_verify() != PCI_RESOURCE_STATUS_OK ||
            dma_verify() != DMA_STATUS_OK || !resources.active ||
            !vectors.active || !installed_dma.active) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DEVICE_SUBSTRATE_PROOF,
                BOOT_CAPABILITY_PCI_RESOURCE_OWNERSHIP_AVAILABLE);
            return ledger->status;
        }
    }

    const bool substrate_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE);
    const bool substrate_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_DEVICE_SUBSTRATE_FIXTURE_ABSENT);
    const bool substrate_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_DEVICE_SUBSTRATE_PROOF) != NULL;
    if (!optional_outcome_valid(substrate_planned,
            substrate_complete, substrate_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_DEVICE_SUBSTRATE_PROOF,
            BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE);
        return ledger->status;
    }
    if (substrate_complete || substrate_absent) {
        const struct boot_stage_receipt *substrate =
            boot_ledger_receipt_for(ledger,
                BOOT_STAGE_DEVICE_SUBSTRATE_PROOF);
        const struct device_substrate_proof proof =
            device_substrate_get_proof();

        if (substrate == NULL ||
            (substrate_complete &&
                (substrate->result != BOOT_RECEIPT_RAN ||
                 substrate->proof_counter_count != 2U ||
                 substrate->proof_counters[0] != 1U ||
                 substrate->proof_counters[1] !=
                    DEVICE_SUBSTRATE_DMA_BYTES ||
                 !proof.dma_device_written || !proof.msix_delivered ||
                 !proof.ownership_round_trip || !proof.teardown_complete)) ||
            (substrate_absent &&
                substrate->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DEVICE_SUBSTRATE_PROOF,
                BOOT_CAPABILITY_DEVICE_SUBSTRATE_INSTALLED_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    const bool xhci_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE);
    const bool xhci_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_XHCI_FIXTURE_ABSENT);
    const bool xhci_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_XHCI_DESCRIPTOR_PROOF) != NULL;
    if (!optional_outcome_valid(xhci_planned, xhci_complete, xhci_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_XHCI_DESCRIPTOR_PROOF,
            BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE);
        return ledger->status;
    }
    if (xhci_complete || xhci_absent) {
        const struct boot_stage_receipt *xhci = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_XHCI_DESCRIPTOR_PROOF);
        const struct xhci_descriptor_proof proof =
            xhci_get_descriptor_proof();

        if (xhci == NULL ||
            (xhci_complete &&
                (xhci->result != BOOT_RECEIPT_RAN ||
                 xhci->proof_counter_count != 2U ||
                 xhci->proof_counters[0] != XHCI_DEVICE_DESCRIPTOR_BYTES ||
                 xhci->proof_counters[1] != 1U ||
                 !proof.controller_ready || !proof.descriptor_valid ||
                 !proof.sentinel_changed_while_controller_owned ||
                 !proof.ownership_complete || !proof.teardown_complete)) ||
            (xhci_absent && xhci->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_XHCI_DESCRIPTOR_PROOF,
                BOOT_CAPABILITY_XHCI_DESCRIPTOR_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    const bool nvme_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE);
    const bool nvme_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_NVME_FIXTURE_ABSENT);
    const bool nvme_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_NVME_READ_PROOF) != NULL;
    if (!optional_outcome_valid(nvme_planned, nvme_complete, nvme_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_NVME_READ_PROOF,
            BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE);
        return ledger->status;
    }
    if (nvme_complete || nvme_absent) {
        const struct boot_stage_receipt *nvme = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_NVME_READ_PROOF);
        const struct nvme_read_proof proof = nvme_get_read_proof();

        if (nvme == NULL ||
            (nvme_complete &&
                (nvme->result != BOOT_RECEIPT_RAN ||
                 nvme->proof_counter_count != 2U ||
                 nvme->proof_counters[0] != NVME_BLOCK_BYTES ||
                 nvme->proof_counters[1] != 1U ||
                 !proof.controller_ready || !proof.namespace_ready ||
                 !proof.contents_valid || !proof.sentinel_valid ||
                 !proof.changed_while_controller_owned ||
                 !proof.ownership_complete || !proof.teardown_complete)) ||
            (nvme_absent && nvme->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_NVME_READ_PROOF,
                BOOT_CAPABILITY_NVME_READ_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    const bool filesystem_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE);
    const bool filesystem_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_FILESYSTEM_FIXTURE_ABSENT);
    const bool filesystem_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_FILESYSTEM_FILE_PROOF) != NULL;
    if (!optional_outcome_valid(filesystem_planned, filesystem_complete,
            filesystem_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_FILESYSTEM_FILE_PROOF,
            BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE);
        return ledger->status;
    }
    if (filesystem_complete || filesystem_absent) {
        const struct boot_stage_receipt *filesystem =
            boot_ledger_receipt_for(ledger,
                BOOT_STAGE_FILESYSTEM_FILE_PROOF);
        const struct filesystem_file_proof proof =
            filesystem_get_file_proof();

        if (filesystem == NULL ||
            (filesystem_complete &&
                (filesystem->result != BOOT_RECEIPT_RAN ||
                 filesystem->proof_counter_count != 2U ||
                 filesystem->proof_counters[0] != FAT16_FILE_BYTES ||
                 filesystem->proof_counters[1] != 4U ||
                 proof.file_bytes != FAT16_FILE_BYTES ||
                 proof.read_count != 4U ||
                 proof.msix_completion_count != 4U ||
                 proof.ignored_completions != 0U ||
                 proof.robustness_tests !=
                    FILESYSTEM_CONTROLLED_ROBUSTNESS_TESTS ||
                 !proof.fat16_ready || !proof.file_located ||
                 !proof.contents_valid || !proof.sentinel_valid ||
                 !proof.changed_while_controller_owned ||
                 !proof.ownership_complete || !proof.teardown_complete)) ||
            (filesystem_absent &&
                filesystem->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_FILESYSTEM_FILE_PROOF,
                BOOT_CAPABILITY_FILESYSTEM_FILE_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    const bool process_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE);
    const bool process_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT);
    const bool process_decided = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED);
    const bool process_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_PROCESS_INSTALLED_PROOF) != NULL;
    if (process_decided != process_planned ||
        !optional_outcome_valid(process_planned, process_complete,
            process_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_PROCESS_INSTALLED_PROOF,
            BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE);
        return ledger->status;
    }
    if (process_complete || process_absent) {
        const struct boot_stage_receipt *process =
            boot_ledger_receipt_for(ledger,
                BOOT_STAGE_PROCESS_INSTALLED_PROOF);
        const struct process_proof_result proof = process_get_proof_result();

        if (process == NULL ||
            (process_complete &&
                (process->result != BOOT_RECEIPT_RAN ||
                 process->proof_counter_count != 2U ||
                 process->proof_counters[0] != ELF64_FILE_BYTES ||
                 process->proof_counters[1] != 1U ||
                 proof.file_bytes != ELF64_FILE_BYTES ||
                 proof.segment_count != 1U ||
                 proof.result != UINT32_C(0x53415037) ||
                 proof.robustness_tests !=
                    PROCESS_CONTROLLED_ROBUSTNESS_TESTS ||
                 !proof.ring_three || !proof.private_address_space ||
                 !proof.image_read_execute ||
                 !proof.stack_read_write_no_execute ||
                 !proof.guard_unmapped || !proof.interrupt_authenticated ||
                 !proof.normal_exit || !proof.teardown_complete ||
                 !proof.resource_census_equal ||
                 !process_resources_released())) ||
            (process_absent && process->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_PROCESS_INSTALLED_PROOF,
                BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    const bool linux_complete = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE);
    const bool linux_absent = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_LINUX_FIXTURE_ABSENT);
    const bool linux_decided = boot_ledger_has_capability(ledger,
        BOOT_CAPABILITY_LINUX_OUTCOME_DECIDED);
    const bool linux_planned = descriptor_for_stage(ledger,
        BOOT_STAGE_LINUX_INSTALLED_PROOF) != NULL;
    if (linux_decided != linux_planned ||
        !optional_outcome_valid(linux_planned, linux_complete, linux_absent)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_LINUX_INSTALLED_PROOF,
            BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE);
        return ledger->status;
    }
    if (linux_complete || linux_absent) {
        const struct boot_stage_receipt *linux = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_LINUX_INSTALLED_PROOF);
        const struct linux_abi_proof_result proof =
            linux_abi_get_proof_result();

        if (linux == NULL ||
            (linux_complete &&
                (linux->result != BOOT_RECEIPT_RAN ||
                 linux->proof_counter_count != 2U ||
                 linux->proof_counters[0] != 33584U ||
                 linux->proof_counters[1] != 9U ||
                 proof.file_bytes != 33584U ||
                 proof.program_headers != 5U || proof.load_segments != 4U ||
                 proof.file_clusters != 9U || proof.stdout_bytes != 7U ||
                 proof.syscall_count != 9U ||
                 proof.distinct_syscalls != 7U || proof.exit_status != 0U ||
                 proof.robustness_tests !=
                    LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS ||
                 !proof.ring_three || !proof.private_address_space ||
                 !proof.real_syscall_instruction || !proof.stdout_valid ||
                 !proof.exit_zero || !proof.unknown_enosys ||
                 !proof.write_xor_execute || !proof.kernel_cr3_restored ||
                 !proof.teardown_complete || !proof.resource_census_equal ||
                 !linux_abi_resources_released())) ||
            (linux_absent && linux->result != BOOT_RECEIPT_SKIPPED)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_LINUX_INSTALLED_PROOF,
                BOOT_CAPABILITY_LINUX_INSTALLED_PROOF_COMPLETE);
            return ledger->status;
        }
    }

    if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_FIRST_LIGHT_INSTALLED_PROOF_COMPLETE)) {
        const struct boot_stage_receipt *font = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_UI_FONT);
        const struct boot_stage_receipt *pointer_decision =
            boot_ledger_receipt_for(ledger, BOOT_STAGE_POINTER_DECISION);
        const struct boot_stage_receipt *pointer_outcome =
            boot_ledger_receipt_for(ledger, BOOT_STAGE_POINTER_OUTCOME);
        const struct boot_stage_receipt *layout = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_UI_LAYOUT);
        const struct boot_stage_receipt *construction =
            boot_ledger_receipt_for(ledger,
                BOOT_STAGE_DESKTOP_CONSTRUCTION);
        const struct boot_stage_receipt *activation =
            boot_ledger_receipt_for(ledger, BOOT_STAGE_DESKTOP_ACTIVATION);
        const struct boot_stage_receipt *proof = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_FIRST_LIGHT_PROOF);
        const struct boot_stage_receipt *wc = boot_ledger_receipt_for(ledger,
            BOOT_STAGE_FRAMEBUFFER_WC);
        const struct boot_stage_receipt *closing =
            boot_ledger_receipt_for(ledger, BOOT_STAGE_CLOSING_PROOFS);
        const struct boot_stage_descriptor *activation_descriptor =
            descriptor_for_stage(ledger, BOOT_STAGE_DESKTOP_ACTIVATION);
        const bool pointer_available = boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE);
        const bool pointer_absent = boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_POINTER_INPUT_ABSENT);
        const struct ui_state *installed_ui = ui_get_state();
        const struct ui_font_metrics metrics = ui_font_get_metrics();
        const struct framebuffer_state framebuffer = framebuffer_get_state();

        if (font == NULL || font->result != BOOT_RECEIPT_RAN ||
            font->proof_counter_count != 2U ||
            font->proof_counters[0] != sapote_ui_font_size() ||
            font->proof_counters[1] != sapote_ui_font_fingerprint() ||
            !ui_font_is_verified() || metrics.width != 8U ||
            metrics.height != 16U || metrics.ascent != 12U ||
            metrics.descent != 4U || metrics.advance != 8U ||
            metrics.first != 0x20U || metrics.count != 95U) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_UI_FONT, BOOT_CAPABILITY_UI_FONT_VERIFIED);
            return ledger->status;
        }
        if (pointer_decision == NULL || pointer_outcome == NULL ||
            pointer_decision->result != BOOT_RECEIPT_RAN ||
            pointer_available == pointer_absent ||
            (pointer_available &&
                pointer_outcome->result != BOOT_RECEIPT_RAN) ||
            (pointer_absent &&
                pointer_outcome->result != BOOT_RECEIPT_SKIPPED) ||
            pointer_is_present() != pointer_available) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_POINTER_OUTCOME,
                BOOT_CAPABILITY_POINTER_AVAILABILITY_DECIDED);
            return ledger->status;
        }
        if (activation_descriptor == NULL ||
            !descriptor_has_capability(
                activation_descriptor->required_capabilities,
                activation_descriptor->required_capability_count,
                BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED) ||
            wc == NULL || wc->result != BOOT_RECEIPT_RAN ||
            (construction != NULL &&
                wc->sequence >= construction->sequence) ||
            (activation != NULL && wc->sequence >= activation->sequence)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DESKTOP_ACTIVATION,
                BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED);
            return ledger->status;
        }
        if (!descriptor_has_capability(
                activation_descriptor->required_capabilities,
                activation_descriptor->required_capability_count,
                BOOT_CAPABILITY_SCHEDULER_AVAILABLE)) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DESKTOP_ACTIVATION,
                BOOT_CAPABILITY_SCHEDULER_AVAILABLE);
            return ledger->status;
        }
        if (layout == NULL || construction == NULL || activation == NULL ||
            proof == NULL || closing == NULL ||
            layout->result != BOOT_RECEIPT_RAN ||
            construction->result != BOOT_RECEIPT_RAN ||
            activation->result != BOOT_RECEIPT_RAN ||
            proof->result != BOOT_RECEIPT_RAN ||
            proof->proof_counter_count != 2U ||
            proof->proof_counters[0] == 0U ||
            proof->proof_counters[1] == 0U ||
            closing->result != BOOT_RECEIPT_RAN ||
            font->sequence >= construction->sequence ||
            layout->sequence >= construction->sequence ||
            construction->sequence >= activation->sequence ||
            closing->sequence >= activation->sequence ||
            activation->sequence >= proof->sequence) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_FIRST_LIGHT_PROOF,
                BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED);
            return ledger->status;
        }
        if (installed_ui == NULL || !installed_ui->active ||
            installed_ui->pointer_present != pointer_available ||
            installed_ui->layout.surface.width != framebuffer.width ||
            installed_ui->layout.surface.height != framebuffer.height ||
            ui_layout_validate(&installed_ui->layout) != UI_STATUS_OK) {
            set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
                BOOT_STAGE_DESKTOP_ACTIVATION,
                BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED);
            return ledger->status;
        }
    } else if (boot_ledger_has_capability(ledger,
            BOOT_CAPABILITY_DESKTOP_SHELL_ACTIVATED)) {
        set_refusal(ledger, BOOT_LEDGER_STATUS_RECEIPT_MISMATCH,
            BOOT_STAGE_FIRST_LIGHT_PROOF,
            BOOT_CAPABILITY_FIRST_LIGHT_INSTALLED_PROOF_COMPLETE);
        return ledger->status;
    }

    ledger->status = BOOT_LEDGER_STATUS_OK;
    return ledger->status;
}

void boot_ledger_publish(const struct boot_ledger *ledger)
{
    published_ledger = ledger;
}

const struct boot_ledger *boot_ledger_installed(void)
{
    return published_ledger;
}

const struct boot_stage_descriptor *boot_ledger_planned_stage_at(
    const struct boot_ledger *ledger,
    size_t index
)
{
    if (ledger == NULL || index >= ledger->planned_count ||
        ledger->canonical_order[index] >= ledger->descriptor_count) {
        return NULL;
    }

    return &ledger->descriptors[ledger->canonical_order[index]];
}

const struct boot_stage_receipt *boot_ledger_receipt_at(
    const struct boot_ledger *ledger,
    size_t index
)
{
    if (ledger == NULL || index >= ledger->receipt_count) {
        return NULL;
    }

    return &ledger->receipts[index];
}

const struct boot_stage_receipt *boot_ledger_receipt_for(
    const struct boot_ledger *ledger,
    enum boot_stage_id stage
)
{
    if (ledger == NULL || !stage_is_known(stage)) {
        return NULL;
    }

    for (size_t index = 0U; index < ledger->receipt_count; ++index) {
        if (ledger->receipts[index].stage_id == stage) {
            return &ledger->receipts[index];
        }
    }

    return NULL;
}

const char *boot_stage_name(enum boot_stage_id stage)
{
    if (stage < BOOT_STAGE_INVALID || stage >= BOOT_STAGE_COUNT) {
        return "unknown stage";
    }

    return stage_names[stage];
}

const char *boot_capability_string(enum boot_capability capability)
{
    if (capability < BOOT_CAPABILITY_INVALID ||
        capability >= BOOT_CAPABILITY_COUNT) {
        return "unknown capability";
    }

    return capability_names[capability];
}

const char *boot_ledger_status_string(enum boot_ledger_status status)
{
    if (status < BOOT_LEDGER_STATUS_OK ||
        status >= BOOT_LEDGER_STATUS_COUNT) {
        return "unknown boot ledger status";
    }

    return status_names[status];
}

const char *boot_receipt_result_string(enum boot_receipt_result result)
{
    if (result < BOOT_RECEIPT_RAN || result >= BOOT_RECEIPT_RESULT_COUNT) {
        return "unknown receipt result";
    }

    return result_names[result];
}

static void self_test_success(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_succeed(descriptor, result);
}

static void self_test_skip(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_skip(descriptor, result);
}

static void self_test_fail(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    (void)descriptor;
    boot_stage_result_fail(result);
}

static void self_test_undeclared(
    struct boot_context *context,
    const struct boot_stage_descriptor *descriptor,
    struct boot_stage_result *result
)
{
    (void)context;
    boot_stage_result_succeed(descriptor, result);
    result->provided_count = 0U;
    (void)boot_stage_result_provide(result,
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED);
}

static struct boot_stage_descriptor self_test_descriptor(
    enum boot_stage_id id,
    enum boot_phase phase,
    bool required,
    boot_stage_execute_t execute
)
{
    struct boot_stage_descriptor descriptor = {0};

    descriptor.id = id;
    descriptor.name = boot_stage_name(id);
    descriptor.required = required;
    descriptor.phase = phase;
    descriptor.irreversible_class = BOOT_IRREVERSIBLE_NONE;
    descriptor.execute = execute;
    return descriptor;
}

static bool self_test_add(
    struct boot_ledger *ledger,
    const struct boot_stage_descriptor *descriptor
)
{
    return boot_ledger_add_stage(ledger, descriptor) ==
        BOOT_LEDGER_STATUS_OK;
}

static bool self_test_mixed_plan(
    struct boot_ledger *ledger,
    bool permuted
)
{
    struct boot_stage_descriptor serial = self_test_descriptor(
        BOOT_STAGE_EARLY_SERIAL, BOOT_PHASE_FOUNDATION, true,
        self_test_success);
    struct boot_stage_descriptor information = self_test_descriptor(
        BOOT_STAGE_BOOT_INFORMATION, BOOT_PHASE_DISCOVERY, true,
        self_test_success);
    struct boot_stage_descriptor frames = self_test_descriptor(
        BOOT_STAGE_FRAME_ALLOCATOR, BOOT_PHASE_CONTROLLERS, true,
        self_test_success);
    struct boot_stage_descriptor heap = self_test_descriptor(
        BOOT_STAGE_MEMORY_RUNTIME, BOOT_PHASE_RUNTIME, true,
        self_test_success);
    struct boot_stage_descriptor framebuffer = self_test_descriptor(
        BOOT_STAGE_FRAMEBUFFER_WC, BOOT_PHASE_RUNTIME, false,
        self_test_success);

    serial.provided_capabilities[0] =
        BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    serial.provided_capability_count = 1U;
    information.required_capabilities[0] =
        BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    information.required_capability_count = 1U;
    information.provided_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    information.provided_capability_count = 1U;
    frames.required_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    frames.required_capability_count = 1U;
    frames.provided_capabilities[0] =
        BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
    frames.provided_capability_count = 1U;
    heap.required_capabilities[0] =
        BOOT_CAPABILITY_PHYSICAL_FRAME_ALLOCATOR_AVAILABLE;
    heap.required_capability_count = 1U;
    heap.provided_capabilities[0] = BOOT_CAPABILITY_HEAP_AVAILABLE;
    heap.provided_capability_count = 1U;
    framebuffer.required_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    framebuffer.required_capability_count = 1U;
    framebuffer.provided_capabilities[0] =
        BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
    framebuffer.provided_capability_count = 1U;

    boot_ledger_reset(ledger);

    if (permuted) {
        return self_test_add(ledger, &heap) &&
            self_test_add(ledger, &framebuffer) &&
            self_test_add(ledger, &frames) &&
            self_test_add(ledger, &information) &&
            self_test_add(ledger, &serial);
    }

    return self_test_add(ledger, &serial) &&
        self_test_add(ledger, &information) &&
        self_test_add(ledger, &frames) &&
        self_test_add(ledger, &heap) &&
        self_test_add(ledger, &framebuffer);
}

bool boot_ledger_self_test(void)
{
    /* The boot stack is 16 KiB; bounded test fixtures live in static storage. */
    static struct boot_ledger first;
    static struct boot_ledger second;
    static struct boot_context context;
    struct boot_stage_descriptor one;
    struct boot_stage_descriptor two;
    struct boot_stage_descriptor three;
    enum boot_capability saved_capability;
    uint32_t saved_sequence;

    if (!optional_outcome_valid(false, false, false) ||
        optional_outcome_valid(true, false, false) ||
        optional_outcome_valid(true, true, true) ||
        !optional_outcome_valid(true, true, false) ||
        !optional_outcome_valid(true, false, true)) {
        return false;
    }

    if (!self_test_mixed_plan(&first, false) ||
        !self_test_mixed_plan(&second, true) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_validate(&second) != BOOT_LEDGER_STATUS_OK ||
        first.validated_plan_fingerprint !=
            second.validated_plan_fingerprint ||
        boot_ledger_execute(&first, &context) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&second, &context) != BOOT_LEDGER_STATUS_OK ||
        first.fingerprint != second.fingerprint ||
        first.receipt_count != second.receipt_count) {
        return false;
    }

    for (size_t index = 0U; index < first.receipt_count; ++index) {
        if (first.receipts[index].stage_id != second.receipts[index].stage_id ||
            first.receipts[index].result != second.receipts[index].result) {
            return false;
        }
    }

    if (boot_ledger_verify_installed(&first, &context) !=
            BOOT_LEDGER_STATUS_OK ||
        boot_ledger_verify_installed(&second, &context) !=
            BOOT_LEDGER_STATUS_OK) {
        return false;
    }

    saved_sequence = first.receipts[0].sequence;
    first.receipts[0].sequence = saved_sequence + 1U;
    if (boot_ledger_verify_installed(&first, &context) !=
            BOOT_LEDGER_STATUS_INSTALLED_PLAN_DIFFERS_VALIDATED_PLAN) {
        return false;
    }
    first.receipts[0].sequence = saved_sequence;

    saved_capability = first.receipts[0].provided_capabilities[0];
    first.receipts[0].provided_capabilities[0] =
        BOOT_CAPABILITY_HEAP_AVAILABLE;
    if (boot_ledger_verify_installed(&first, &context) !=
            BOOT_LEDGER_STATUS_UNDECLARED_CAPABILITY_PROVIDED) {
        return false;
    }
    first.receipts[0].provided_capabilities[0] = saved_capability;

    if (boot_ledger_verify_installed(&first, &context) !=
            BOOT_LEDGER_STATUS_OK) {
        return false;
    }

    /* Missing provider. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_PAGING_INSTALL,
        BOOT_PHASE_MEMORY_TRANSITION, true, self_test_success);
    one.required_capabilities[0] =
        BOOT_CAPABILITY_DEVICE_WINDOW_REGISTRY_VALIDATED;
    one.required_capability_count = 1U;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_MISSING_CAPABILITY_PROVIDER) {
        return false;
    }

    /* Duplicate stage. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    if (!self_test_add(&first, &one) || !self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_DUPLICATE_STAGE) {
        return false;
    }

    /* Duplicate providers. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    two = self_test_descriptor(BOOT_STAGE_INTERRUPT_FOUNDATION,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    one.provided_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.provided_capability_count = 1U;
    two.provided_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    two.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) || !self_test_add(&first, &two) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER) {
        return false;
    }

    /* Duplicate requirement inside one typed descriptor. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_BOOT_INFORMATION,
        BOOT_PHASE_DISCOVERY, true, self_test_success);
    one.required_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.required_capabilities[1] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.required_capability_count = 2U;
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    one.provided_capability_count = 1U;
    two = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    two.provided_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    two.provided_capability_count = 1U;
    if (!self_test_add(&first, &two) || !self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_REQUIREMENT) {
        return false;
    }

    /* Unknown stage and unknown capability. */
    boot_ledger_reset(&first);
    one.id = BOOT_STAGE_COUNT;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_UNKNOWN_STAGE_IDENTIFIER) {
        return false;
    }

    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    one.provided_capabilities[0] = BOOT_CAPABILITY_COUNT;
    one.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_UNKNOWN_CAPABILITY) {
        return false;
    }

    /* Direct and three-stage dependency cycles. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    two = self_test_descriptor(BOOT_STAGE_INTERRUPT_FOUNDATION,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    one.required_capabilities[0] = BOOT_CAPABILITY_IDT_INSTALLED;
    one.required_capability_count = 1U;
    one.provided_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.provided_capability_count = 1U;
    two.required_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    two.required_capability_count = 1U;
    two.provided_capabilities[0] = BOOT_CAPABILITY_IDT_INSTALLED;
    two.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) || !self_test_add(&first, &two) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_CAPABILITY_DEPENDENCY_CYCLE) {
        return false;
    }

    boot_ledger_reset(&first);
    three = self_test_descriptor(BOOT_STAGE_PURE_SELF_TESTS,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    one.required_capabilities[0] = BOOT_CAPABILITY_BOOT_SELF_TESTS_COMPLETE;
    two.required_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    three.required_capabilities[0] = BOOT_CAPABILITY_IDT_INSTALLED;
    three.required_capability_count = 1U;
    three.provided_capabilities[0] = BOOT_CAPABILITY_BOOT_SELF_TESTS_COMPLETE;
    three.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) || !self_test_add(&first, &two) ||
        !self_test_add(&first, &three) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_CAPABILITY_DEPENDENCY_CYCLE) {
        return false;
    }

    /* Capacity plus one. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    for (size_t index = 0U; index < BOOT_LEDGER_STAGE_CAPACITY; ++index) {
        first.descriptors[index] = one;
    }
    first.descriptor_count = BOOT_LEDGER_STAGE_CAPACITY;
    if (boot_ledger_add_stage(&first, &one) !=
        BOOT_LEDGER_STATUS_TOO_MANY_STAGES) {
        return false;
    }

    /* A result cannot mint an undeclared capability. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_undeclared);
    one.provided_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&first, &context) !=
            BOOT_LEDGER_STATUS_UNDECLARED_CAPABILITY_PROVIDED) {
        return false;
    }

    /* The executor itself refuses a consumer with no requirement receipt. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_BOOT_INFORMATION,
        BOOT_PHASE_DISCOVERY, true, self_test_success);
    one.required_capabilities[0] = BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE;
    one.required_capability_count = 1U;
    if (execute_stage(&first, &context, &one) !=
            BOOT_LEDGER_STATUS_STAGE_EXECUTED_BEFORE_REQUIREMENTS ||
        first.refusal_capability != BOOT_CAPABILITY_EARLY_SERIAL_AVAILABLE) {
        return false;
    }

    /* Optional failure leaks no success capability; required failure stops. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_FRAMEBUFFER_WC,
        BOOT_PHASE_RUNTIME, false, self_test_fail);
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED;
    one.provided_capability_count = 1U;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&first, &context) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_FRAMEBUFFER_WC_INDEPENDENTLY_PROVED) ||
        first.receipts[0].result != BOOT_RECEIPT_FAILED) {
        return false;
    }

    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_BOOT_INFORMATION,
        BOOT_PHASE_DISCOVERY, true, self_test_fail);
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    one.provided_capability_count = 1U;
    two = self_test_descriptor(BOOT_STAGE_FRAME_ALLOCATOR,
        BOOT_PHASE_CONTROLLERS, true, self_test_success);
    two.required_capabilities[0] =
        BOOT_CAPABILITY_BOOT_INFORMATION_VALIDATED;
    two.required_capability_count = 1U;
    if (!self_test_add(&first, &two) || !self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&first, &context) !=
            BOOT_LEDGER_STATUS_REQUIRED_STAGE_FAILED ||
        first.receipt_count != 1U ||
        boot_ledger_receipt_for(&first, BOOT_STAGE_FRAME_ALLOCATOR) != NULL) {
        return false;
    }

    /* Stable stage identity participates in the fingerprint. */
    boot_ledger_reset(&first);
    boot_ledger_reset(&second);
    one = self_test_descriptor(BOOT_STAGE_EARLY_SERIAL,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    two = self_test_descriptor(BOOT_STAGE_INTERRUPT_FOUNDATION,
        BOOT_PHASE_FOUNDATION, true, self_test_success);
    if (!self_test_add(&first, &one) || !self_test_add(&second, &two) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_validate(&second) != BOOT_LEDGER_STATUS_OK ||
        first.validated_plan_fingerprint ==
            second.validated_plan_fingerprint) {
        return false;
    }

    /* Mutating a stable identifier after validation invalidates the plan. */
    first.descriptors[first.canonical_order[0]].id =
        BOOT_STAGE_INTERRUPT_FOUNDATION;
    if (boot_ledger_execute(&first, &context) !=
        BOOT_LEDGER_STATUS_PLAN_FINGERPRINT_MISMATCH) {
        return false;
    }

    /* Optional framebuffer absence and ECAM-independent PCI remain valid. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_FRAMEBUFFER_WC,
        BOOT_PHASE_RUNTIME, false, self_test_skip);
    one.skipped_capabilities[0] =
        BOOT_CAPABILITY_FRAMEBUFFER_SERIAL_FALLBACK;
    one.skipped_capability_count = 1U;
    two = self_test_descriptor(BOOT_STAGE_PCI, BOOT_PHASE_SERVICES, true,
        self_test_success);
    two.provided_capabilities[0] = BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE;
    two.provided_capability_count = 1U;
    if (!self_test_add(&first, &two) || !self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&first, &context) != BOOT_LEDGER_STATUS_OK ||
        !boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_FRAMEBUFFER_SERIAL_FALLBACK) ||
        !boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_PCI_ACCESS_AVAILABLE)) {
        return false;
    }

    /* Pointer absence is an explicit neutral branch, never a success leak. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_POINTER_OUTCOME,
        BOOT_PHASE_RUNTIME, false, self_test_skip);
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE;
    one.provided_capability_count = 1U;
    one.skipped_capabilities[0] = BOOT_CAPABILITY_POINTER_INPUT_ABSENT;
    one.skipped_capability_count = 1U;
    one.skip_preserves_health = true;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&first, &context) != BOOT_LEDGER_STATUS_OK ||
        first.degraded || first.optional_skip_count != 1U ||
        !boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_POINTER_INPUT_ABSENT) ||
        boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_POINTER_INPUT_AVAILABLE)) {
        return false;
    }

    /* One outcome capability may be common to success and neutral absence. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_PROCESS_INSTALLED_PROOF,
        BOOT_PHASE_SERVICES, false, self_test_skip);
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE;
    one.provided_capabilities[1] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    one.provided_capability_count = 2U;
    one.skipped_capabilities[0] = BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT;
    one.skipped_capabilities[1] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    one.skipped_capability_count = 2U;
    one.skip_preserves_health = true;
    two = self_test_descriptor(BOOT_STAGE_CLOSING_PROOFS,
        BOOT_PHASE_PROOFS, true, self_test_success);
    two.required_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    two.required_capability_count = 1U;
    if (!self_test_add(&first, &two) || !self_test_add(&first, &one) ||
        boot_ledger_validate(&first) != BOOT_LEDGER_STATUS_OK ||
        first.validated_stage_ids[0] !=
            BOOT_STAGE_PROCESS_INSTALLED_PROOF ||
        first.validated_stage_ids[1] != BOOT_STAGE_CLOSING_PROOFS ||
        boot_ledger_execute(&first, &context) != BOOT_LEDGER_STATUS_OK ||
        first.degraded || first.optional_skip_count != 1U ||
        !boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT) ||
        !boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(&first,
            BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE)) {
        return false;
    }

    boot_ledger_reset(&second);
    one.execute = self_test_success;
    if (!self_test_add(&second, &two) || !self_test_add(&second, &one) ||
        boot_ledger_validate(&second) != BOOT_LEDGER_STATUS_OK ||
        boot_ledger_execute(&second, &context) != BOOT_LEDGER_STATUS_OK ||
        !boot_ledger_has_capability(&second,
            BOOT_CAPABILITY_PROCESS_INSTALLED_PROOF_COMPLETE) ||
        !boot_ledger_has_capability(&second,
            BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED) ||
        boot_ledger_has_capability(&second,
            BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT)) {
        return false;
    }

    /* Repeating the common capability inside one outcome remains invalid. */
    boot_ledger_reset(&first);
    one.skipped_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER) {
        return false;
    }

    /* Two distinct stages may not share one outcome capability. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_PROCESS_INSTALLED_PROOF,
        BOOT_PHASE_SERVICES, false, self_test_success);
    one.provided_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    one.provided_capability_count = 1U;
    two = self_test_descriptor(BOOT_STAGE_ELF64_LOADER_FOUNDATION,
        BOOT_PHASE_SERVICES, false, self_test_skip);
    two.skipped_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_OUTCOME_DECIDED;
    two.skipped_capability_count = 1U;
    two.skip_preserves_health = true;
    if (!self_test_add(&first, &one) || !self_test_add(&first, &two) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_DUPLICATE_CAPABILITY_PROVIDER) {
        return false;
    }

    /* A required or capability-free stage cannot declare a neutral skip. */
    boot_ledger_reset(&first);
    one = self_test_descriptor(BOOT_STAGE_PROCESS_INSTALLED_PROOF,
        BOOT_PHASE_SERVICES, true, self_test_skip);
    one.skipped_capabilities[0] =
        BOOT_CAPABILITY_PROCESS_FIXTURE_ABSENT;
    one.skipped_capability_count = 1U;
    one.skip_preserves_health = true;
    if (!self_test_add(&first, &one) ||
        boot_ledger_validate(&first) !=
            BOOT_LEDGER_STATUS_INVALID_NEUTRAL_SKIP) {
        return false;
    }

    return true;
}
