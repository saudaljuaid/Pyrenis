/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * One bounded NVMe controller, namespace, queue pair and read. Register and
 * command encodings follow NVMe Base 2.4 sections 3.1.4, 3.5.1, 4.1.1,
 * 4.2.1, 4.2.3, 4.2.4, 4.3.1, 5.2.14, 5.3.1 and 5.3.2; NVM Command Set 1.3
 * sections 3.3.4 and 4.1.5.1; and NVMe over PCIe Transport 1.4 sections
 * 3.1.1, 3.1.2, 3.2 and 3.5. No command in this file changes media.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/clock.h>
#include <sapote/cpu.h>
#include <sapote/dma.h>
#include <sapote/fat16.h>
#include <sapote/interrupt_vector.h>
#include <sapote/memory.h>
#include <sapote/msix.h>
#include <sapote/nvme.h>
#include <sapote/pci.h>
#include <sapote/pci_resource.h>

#define NVME_REG_CAP UINT64_C(0x00)
#define NVME_REG_VS UINT64_C(0x08)
#define NVME_REG_CC UINT64_C(0x14)
#define NVME_REG_CSTS UINT64_C(0x1C)
#define NVME_REG_AQA UINT64_C(0x24)
#define NVME_REG_ASQ UINT64_C(0x28)
#define NVME_REG_ACQ UINT64_C(0x30)
#define NVME_CONTROLLER_REGISTER_BYTES UINT64_C(0x38)
#define NVME_DOORBELL_BASE UINT64_C(0x1000)

#define NVME_CAP_MQES_MASK UINT64_C(0xFFFF)
#define NVME_CAP_CQR UINT64_C(1U << 16)
#define NVME_CAP_TO_SHIFT 24U
#define NVME_CAP_DSTRD_SHIFT 32U
#define NVME_CAP_CSS_SHIFT 37U
#define NVME_CAP_CSS_MASK UINT64_C(0xFF)
#define NVME_CAP_CSS_NVM UINT64_C(1)
#define NVME_CAP_MPSMIN_SHIFT 48U
#define NVME_CAP_MPSMAX_SHIFT 52U

#define NVME_CC_EN UINT32_C(1)
#define NVME_CC_CSS_MASK UINT32_C(7U << 4)
#define NVME_CC_MPS_MASK UINT32_C(0xFU << 7)
#define NVME_CC_AMS_MASK UINT32_C(7U << 11)
#define NVME_CC_SHN_MASK UINT32_C(3U << 14)
#define NVME_CC_IOSQES_MASK UINT32_C(0xFU << 16)
#define NVME_CC_IOCQES_MASK UINT32_C(0xFU << 20)
#define NVME_CC_CRIME UINT32_C(1U << 24)
#define NVME_CC_WRITABLE_MASK (NVME_CC_EN | NVME_CC_CSS_MASK | \
    NVME_CC_MPS_MASK | NVME_CC_AMS_MASK | NVME_CC_SHN_MASK | \
    NVME_CC_IOSQES_MASK | NVME_CC_IOCQES_MASK | NVME_CC_CRIME)
#define NVME_CC_IOSQES UINT32_C(6U << 16)
#define NVME_CC_IOCQES UINT32_C(4U << 20)
#define NVME_CSTS_RDY UINT32_C(1)
#define NVME_CSTS_CFS UINT32_C(1U << 1)

#define NVME_ADMIN_IDENTIFY UINT8_C(0x06)
#define NVME_ADMIN_CREATE_IO_SQ UINT8_C(0x01)
#define NVME_ADMIN_CREATE_IO_CQ UINT8_C(0x05)
#define NVME_NVM_READ UINT8_C(0x02)
#define NVME_IDENTIFY_NAMESPACE UINT32_C(0)
#define NVME_IDENTIFY_CONTROLLER UINT32_C(1)
#define NVME_IDENTIFY_ACTIVE_NAMESPACE_LIST UINT32_C(2)
#define NVME_QUEUE_PHYSICALLY_CONTIGUOUS UINT32_C(1)
#define NVME_QUEUE_INTERRUPT_ENABLED UINT32_C(1U << 1)

#define NVME_IDENTIFY_BYTES UINT64_C(4096)
#define NVME_SUBMISSION_ENTRY_BYTES UINT64_C(64)
#define NVME_COMPLETION_ENTRY_BYTES UINT64_C(16)
#define NVME_SENTINEL UINT8_C(0xA5)
#define NVME_COMMAND_TIMEOUT_NS UINT64_C(2000000000)
#define NVME_TIMEOUT_UNIT_NS UINT64_C(500000000)
#define NVME_MAX_COMMAND_IDENTIFIER UINT16_C(0xFFFE)
#define NVME_READ_ALLOCATION_PAGES 3U
#define NVME_READ_DATA_PAGE 1U

#define NVME_IDENTIFY_CONTROLLER_TYPE_OFFSET 111U
#define NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET 77U
#define NVME_IDENTIFY_CONTROLLER_VERSION_OFFSET 80U
#define NVME_IDENTIFY_CONTROLLER_SQES_OFFSET 512U
#define NVME_IDENTIFY_CONTROLLER_CQES_OFFSET 513U
#define NVME_IDENTIFY_CONTROLLER_NN_OFFSET 516U

#define NVME_IDENTIFY_NS_NSZE_OFFSET 0U
#define NVME_IDENTIFY_NS_NCAP_OFFSET 8U
#define NVME_IDENTIFY_NS_NUSE_OFFSET 16U
#define NVME_IDENTIFY_NS_NLBAF_OFFSET 25U
#define NVME_IDENTIFY_NS_FLBAS_OFFSET 26U
#define NVME_IDENTIFY_NS_DPS_OFFSET 29U
#define NVME_IDENTIFY_NS_LBAF_OFFSET 128U
#define NVME_IDENTIFY_NS_LBAF_BYTES 4U

enum nvme_expect_kind {
    NVME_EXPECT_NONE = 0,
    NVME_EXPECT_ADMIN,
    NVME_EXPECT_IO
};

struct nvme_expectation {
    volatile enum nvme_expect_kind kind;
    volatile bool done;
    volatile enum nvme_status result;
    struct nvme_queue_pair *queue;
    struct dma_allocation *data;
    uint64_t data_offset;
    uint64_t data_length;
    uint16_t command_identifier;
    uint16_t submission_queue_identifier;
};

struct nvme_runtime {
    struct nvme_controller_claim claim;
    struct nvme_register_regions registers;
    struct nvme_controller_capabilities capabilities;
    volatile uint8_t *mmio;
    struct nvme_queue_pair admin;
    struct nvme_queue_pair io;
    struct nvme_identify_buffers identify;
    struct nvme_namespace_selection namespace_data;
    struct nvme_prp_read_buffer read;
    struct nvme_interrupt_binding interrupt;
    struct nvme_expectation expectation;
    uint16_t next_admin_command_identifier;
    bool controller_enabled;
    bool bus_master_enabled;
    bool teardown_started;
    bool handler_saw_freed_state;
    bool wrong_vector;
    bool ownership_failure;
    size_t ignored_completions;
    uint64_t interrupt_count;
};

_Static_assert(sizeof(struct nvme_submission_entry) ==
    NVME_SUBMISSION_ENTRY_BYTES, "NVMe SQE must be 64 bytes");
_Static_assert(sizeof(struct nvme_completion_entry) ==
    NVME_COMPLETION_ENTRY_BYTES, "NVMe CQE must be 16 bytes");
_Static_assert(SAPOTE_PAGE_SIZE == NVME_BLOCK_BYTES,
    "the bounded fixture uses Sapote's one 4 KiB DMA page");

static struct nvme_read_proof installed_proof;
static uint64_t controller_generation;

struct nvme_filesystem_runtime {
    struct nvme_runtime controller;
    struct pci_resource_state pci_before;
    struct dma_state dma_before;
    struct interrupt_vector_state vectors_before;
    struct msix_state msix_before;
    struct frame_allocator_stats frames_before;
    uint64_t generation;
    bool active;
};

static struct nvme_filesystem_runtime filesystem_runtime;

static void zero_bytes(void *pointer, uint64_t length)
{
    uint8_t *bytes = pointer;

    for (uint64_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void fill_bytes(void *pointer, uint64_t length, uint8_t value)
{
    uint8_t *bytes = pointer;

    for (uint64_t index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8U |
        (uint32_t)bytes[2] << 16U | (uint32_t)bytes[3] << 24U;
}

static uint64_t read_le64(const uint8_t *bytes)
{
    return (uint64_t)read_le32(bytes) |
        (uint64_t)read_le32(bytes + 4U) << 32U;
}

static uint32_t mmio_read32(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint32_t *)(void *)(base + offset);
}

static uint64_t mmio_read64(volatile uint8_t *base, uint64_t offset)
{
    return *(volatile uint64_t *)(void *)(base + offset);
}

static void mmio_write32(
    volatile uint8_t *base,
    uint64_t offset,
    uint32_t value
)
{
    *(volatile uint32_t *)(void *)(base + offset) = value;
}

static void mmio_write64(
    volatile uint8_t *base,
    uint64_t offset,
    uint64_t value
)
{
    *(volatile uint64_t *)(void *)(base + offset) = value;
}

static uint64_t physical_of(const struct dma_allocation *allocation)
{
    return (uint64_t)allocation->frames.physical_base;
}

static bool add_checked(uint64_t left, uint64_t right, uint64_t *sum)
{
    if (sum == NULL || left > UINT64_MAX - right) {
        return false;
    }
    *sum = left + right;
    return true;
}

static bool multiply_checked(
    uint64_t left,
    uint64_t right,
    uint64_t *product
)
{
    if (product == NULL || (left != 0U && right > UINT64_MAX / left)) {
        return false;
    }
    *product = left * right;
    return true;
}

static bool deadline_reached(uint64_t now, uint64_t deadline)
{
    return now >= deadline;
}

static enum nvme_status validate_span(
    uint64_t bar_size,
    uint64_t offset,
    uint64_t length,
    uint64_t alignment,
    struct nvme_register_span *span
)
{
    uint64_t end;

    if (span == NULL || length == 0U) {
        return NVME_STATUS_REGISTER_OUTSIDE_BAR;
    }
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
        (offset & (alignment - 1U)) != 0U) {
        return NVME_STATUS_REGISTER_ALIGNMENT;
    }
    if (!add_checked(offset, length, &end)) {
        return NVME_STATUS_REGISTER_OVERFLOW;
    }
    if (end > bar_size) {
        return NVME_STATUS_REGISTER_OUTSIDE_BAR;
    }
    span->offset = offset;
    span->length = length;
    span->valid = true;
    return NVME_STATUS_OK;
}

static enum nvme_status transition(
    struct nvme_controller_claim *claim,
    enum nvme_controller_state next
)
{
    enum nvme_controller_state current;
    bool valid = false;

    if (claim == NULL || next <= NVME_CONTROLLER_UNINITIALIZED ||
        next >= NVME_CONTROLLER_STATE_COUNT) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    current = claim->state;
    if (current == next) {
        return NVME_STATUS_TRANSITION_REPEATED;
    }
    if (next < current && next != NVME_CONTROLLER_RELEASED) {
        return NVME_STATUS_TRANSITION_REVERSED;
    }
    if (current == NVME_CONTROLLER_UNINITIALIZED &&
        next == NVME_CONTROLLER_DISCOVERED) {
        valid = true;
    } else if (current == NVME_CONTROLLER_DISCOVERED &&
        next == NVME_CONTROLLER_CLAIMED) {
        valid = true;
    } else if (current == NVME_CONTROLLER_CLAIMED &&
        next == NVME_CONTROLLER_DISABLED) {
        valid = true;
    } else if (current == NVME_CONTROLLER_DISABLED &&
        next == NVME_CONTROLLER_PREPARED) {
        valid = true;
    } else if (current == NVME_CONTROLLER_PREPARED &&
        next == NVME_CONTROLLER_RUNNING) {
        valid = true;
    } else if (current == NVME_CONTROLLER_RUNNING &&
        next == NVME_CONTROLLER_STOPPING) {
        valid = true;
    } else if ((current == NVME_CONTROLLER_DISCOVERED ||
            current == NVME_CONTROLLER_CLAIMED ||
            current == NVME_CONTROLLER_DISABLED ||
            current == NVME_CONTROLLER_PREPARED ||
            current == NVME_CONTROLLER_STOPPING) &&
        next == NVME_CONTROLLER_RELEASED) {
        valid = true;
    }
    if (!valid) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    claim->state = next;
    return NVME_STATUS_OK;
}

static const struct pci_function *discover_controller(
    enum nvme_status *result
)
{
    const struct pci_function *found = NULL;

    if (result == NULL) {
        return NULL;
    }
    *result = NVME_STATUS_ABSENT;
    for (size_t index = 0U; index < pci_function_count(); ++index) {
        const struct pci_function *function = pci_function_at(index);

        if (function != NULL &&
            function->class_code == PCI_CLASS_MASS_STORAGE &&
            function->subclass ==
                NVME_PCI_SUBCLASS_NON_VOLATILE_MEMORY &&
            function->prog_if == NVME_PCI_PROGRAMMING_INTERFACE) {
            if (found != NULL) {
                *result = NVME_STATUS_MULTIPLE_CONTROLLERS;
                return NULL;
            }
            found = function;
            *result = NVME_STATUS_OK;
        }
    }
    return found;
}

static enum nvme_status doorbell_offset(
    uint64_t stride,
    uint16_t queue_identifier,
    bool completion,
    uint64_t *offset
)
{
    uint64_t index;
    uint64_t displacement;

    if (offset == NULL || stride < sizeof(uint32_t) ||
        (stride & (stride - 1U)) != 0U) {
        return NVME_STATUS_CAP_DOORBELL_GEOMETRY;
    }
    index = (uint64_t)queue_identifier * 2U + (completion ? 1U : 0U);
    if (!multiply_checked(index, stride, &displacement) ||
        !add_checked(NVME_DOORBELL_BASE, displacement, offset)) {
        return NVME_STATUS_CAP_DOORBELL_GEOMETRY;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status validate_fixed_registers(
    struct nvme_runtime *controller
)
{
    volatile void *pointer = NULL;
    enum nvme_status status;

    controller->registers.bar_size = controller->registers.mapping->size;
    status = validate_span(controller->registers.bar_size, 0U,
        NVME_CONTROLLER_REGISTER_BYTES, sizeof(uint32_t),
        &controller->registers.controller);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    status = validate_span(controller->registers.bar_size, NVME_REG_AQA,
        NVME_CONTROLLER_REGISTER_BYTES - NVME_REG_AQA,
        sizeof(uint32_t), &controller->registers.admin);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    if (pci_mmio_subregion(controller->registers.mapping, 0U,
            NVME_CONTROLLER_REGISTER_BYTES, &pointer) !=
            PCI_RESOURCE_STATUS_OK) {
        return NVME_STATUS_REGISTER_OUTSIDE_BAR;
    }
    controller->mmio = (volatile uint8_t *)pointer;
    return NVME_STATUS_OK;
}

static enum nvme_status validate_doorbells(
    struct nvme_runtime *controller
)
{
    struct nvme_register_span *spans[4] = {
        &controller->registers.admin_submission_doorbell,
        &controller->registers.admin_completion_doorbell,
        &controller->registers.io_submission_doorbell,
        &controller->registers.io_completion_doorbell
    };
    const uint16_t identifiers[4] = {0U, 0U, 1U, 1U};
    const bool completion[4] = {false, true, false, true};

    for (size_t index = 0U; index < 4U; ++index) {
        uint64_t offset;
        volatile void *pointer = NULL;
        enum nvme_status status = doorbell_offset(
            controller->capabilities.doorbell_stride,
            identifiers[index], completion[index], &offset);

        if (status != NVME_STATUS_OK) {
            return status;
        }
        status = validate_span(controller->registers.bar_size, offset,
            sizeof(uint32_t), sizeof(uint32_t), spans[index]);
        if (status != NVME_STATUS_OK) {
            return status;
        }
        if (pci_mmio_subregion(controller->registers.mapping, offset,
                sizeof(uint32_t), &pointer) != PCI_RESOURCE_STATUS_OK ||
            pointer == NULL) {
            return NVME_STATUS_REGISTER_OUTSIDE_BAR;
        }
    }
    return NVME_STATUS_OK;
}

static enum nvme_status decode_capabilities(
    uint64_t raw,
    uint32_t version,
    struct nvme_controller_capabilities *capabilities
)
{
    uint64_t timeout_units;
    uint8_t major = (uint8_t)(version >> 16U);
    uint8_t minor = (uint8_t)(version >> 8U);

    if (capabilities == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(capabilities, sizeof(*capabilities));
    capabilities->raw = raw;
    capabilities->version = version;
    capabilities->maximum_queue_entries =
        (uint32_t)(raw & NVME_CAP_MQES_MASK) + 1U;
    if (capabilities->maximum_queue_entries < NVME_QUEUE_DEPTH) {
        return NVME_STATUS_CAP_QUEUE_GEOMETRY;
    }
    capabilities->doorbell_stride = UINT32_C(1) <<
        (2U + (uint32_t)((raw >> NVME_CAP_DSTRD_SHIFT) & 0xFU));
    if (capabilities->doorbell_stride < sizeof(uint32_t)) {
        return NVME_STATUS_CAP_DOORBELL_GEOMETRY;
    }
    timeout_units = (raw >> NVME_CAP_TO_SHIFT) & UINT64_C(0xFF);
    if (!multiply_checked(timeout_units, NVME_TIMEOUT_UNIT_NS,
            &capabilities->ready_timeout_ns)) {
        return NVME_STATUS_CAP_QUEUE_GEOMETRY;
    }
    capabilities->minimum_page_shift = (uint8_t)(12U +
        ((raw >> NVME_CAP_MPSMIN_SHIFT) & 0xFU));
    capabilities->maximum_page_shift = (uint8_t)(12U +
        ((raw >> NVME_CAP_MPSMAX_SHIFT) & 0xFU));
    if (capabilities->minimum_page_shift > 12U ||
        capabilities->maximum_page_shift < 12U ||
        capabilities->minimum_page_shift >
            capabilities->maximum_page_shift) {
        return NVME_STATUS_UNSUPPORTED_PAGE_SIZE;
    }
    capabilities->queues_contiguous_required =
        (raw & NVME_CAP_CQR) != 0U;
    capabilities->nvm_command_set =
        (((raw >> NVME_CAP_CSS_SHIFT) & NVME_CAP_CSS_MASK) &
            NVME_CAP_CSS_NVM) != 0U;
    if (!capabilities->nvm_command_set) {
        return NVME_STATUS_UNSUPPORTED_COMMAND_SET;
    }
    if (!((major == 1U && minor >= 4U) || major == 2U)) {
        return NVME_STATUS_UNSUPPORTED_VERSION;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status wait_ready(
    struct nvme_runtime *controller,
    bool ready,
    enum nvme_status timeout_status
)
{
    uint64_t deadline;

    if (!add_checked(clock_monotonic_ns(),
            controller->capabilities.ready_timeout_ns, &deadline)) {
        return timeout_status;
    }
    for (;;) {
        uint32_t status = mmio_read32(controller->mmio, NVME_REG_CSTS);

        if ((status & NVME_CSTS_CFS) != 0U) {
            return NVME_STATUS_CONTROLLER_FATAL;
        }
        if (((status & NVME_CSTS_RDY) != 0U) == ready) {
            return NVME_STATUS_OK;
        }
        if (deadline_reached(clock_monotonic_ns(), deadline)) {
            return timeout_status;
        }
        __asm__ volatile ("" : : : "memory");
    }
}

static enum nvme_status disable_controller(struct nvme_runtime *controller)
{
    uint32_t configuration = mmio_read32(controller->mmio, NVME_REG_CC);

    if ((configuration & NVME_CC_EN) != 0U) {
        mmio_write32(controller->mmio, NVME_REG_CC,
            configuration & ~NVME_CC_EN);
    }
    controller->controller_enabled = false;
    return wait_ready(controller, false, NVME_STATUS_DISABLE_TIMEOUT);
}

static enum nvme_status validate_queue_geometry(
    uint64_t base,
    uint64_t allocation_length,
    uint64_t entry_bytes,
    size_t depth,
    enum nvme_status malformed
)
{
    uint64_t required;
    uint64_t end;

    if ((base & (SAPOTE_PAGE_SIZE - 1U)) != 0U || depth < 2U ||
        !multiply_checked((uint64_t)depth, entry_bytes, &required) ||
        required > allocation_length || !add_checked(base, required, &end) ||
        end <= base) {
        return malformed;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status initialize_queue_pair(
    struct nvme_queue_pair *queue,
    enum nvme_queue_kind kind,
    uint16_t identifier,
    enum nvme_status malformed
)
{
    enum nvme_status status;

    if (queue == NULL || !queue->submission.active ||
        !queue->completion.active) {
        return malformed;
    }
    status = validate_queue_geometry(physical_of(&queue->submission),
        queue->submission.byte_length, NVME_SUBMISSION_ENTRY_BYTES,
        NVME_QUEUE_DEPTH, malformed);
    if (status == NVME_STATUS_OK) {
        status = validate_queue_geometry(physical_of(&queue->completion),
            queue->completion.byte_length, NVME_COMPLETION_ENTRY_BYTES,
            NVME_QUEUE_DEPTH, malformed);
    }
    if (status != NVME_STATUS_OK) {
        return status;
    }
    queue->kind = kind;
    queue->identifier = identifier;
    queue->depth = NVME_QUEUE_DEPTH;
    queue->submission_tail = 0U;
    queue->completion_head = 0U;
    queue->completion_phase = 1U;
    queue->outstanding.value = 0U;
    queue->outstanding.active = false;
    queue->active = kind == NVME_QUEUE_ADMIN;
    return NVME_STATUS_OK;
}

static enum nvme_status allocate_dma(
    struct dma_allocation *allocation,
    size_t page_count
)
{
    const struct dma_request request = {
        .page_count = page_count,
        .alignment = SAPOTE_PAGE_SIZE,
        .maximum_physical_address = UINT64_MAX
    };

    return dma_allocate(&request, allocation) == DMA_STATUS_OK ?
        NVME_STATUS_OK : NVME_STATUS_DMA_ALLOCATION;
}

static enum nvme_status prepare_dma(struct nvme_runtime *controller)
{
    struct dma_allocation *allocations[7] = {
        &controller->admin.submission,
        &controller->admin.completion,
        &controller->identify.controller,
        &controller->identify.namespace_data,
        &controller->io.submission,
        &controller->io.completion,
        &controller->read.dma
    };
    const size_t pages[7] = {1U, 1U, 1U, 1U, 1U, 1U,
        NVME_READ_ALLOCATION_PAGES};

    for (size_t index = 0U; index < 7U; ++index) {
        enum nvme_status status = allocate_dma(allocations[index],
            pages[index]);

        if (status != NVME_STATUS_OK) {
            return status;
        }
        zero_bytes(allocations[index]->cpu_address,
            allocations[index]->byte_length);
    }
    fill_bytes(controller->read.dma.cpu_address,
        controller->read.dma.byte_length, NVME_SENTINEL);
    controller->read.data_offset =
        (uint64_t)NVME_READ_DATA_PAGE * SAPOTE_PAGE_SIZE;
    controller->read.data_length = NVME_BLOCK_BYTES;
    controller->read.state = NVME_DMA_CPU_OWNED;
    controller->admin.submission_state = NVME_DMA_CPU_OWNED;
    controller->admin.completion_state = NVME_DMA_CPU_OWNED;
    controller->io.submission_state = NVME_DMA_CPU_OWNED;
    controller->io.completion_state = NVME_DMA_CPU_OWNED;
    controller->identify.controller_state = NVME_DMA_CPU_OWNED;
    controller->identify.namespace_state = NVME_DMA_CPU_OWNED;
    if (initialize_queue_pair(&controller->admin, NVME_QUEUE_ADMIN,
            NVME_QUEUE_IDENTIFIER_ADMIN, NVME_STATUS_ADMIN_QUEUE_INVALID) !=
            NVME_STATUS_OK ||
        initialize_queue_pair(&controller->io, NVME_QUEUE_IO,
            NVME_QUEUE_IDENTIFIER_IO, NVME_STATUS_IO_QUEUE_INVALID) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_DMA_LAYOUT;
    }
    return NVME_STATUS_OK;
}

static void set_dma_state(
    enum nvme_dma_object_state *state,
    enum dma_owner owner
)
{
    *state = owner == DMA_OWNER_CPU ? NVME_DMA_CPU_OWNED :
        NVME_DMA_CONTROLLER_OWNED;
}

static enum nvme_status transfer_all_to_controller(
    struct nvme_runtime *controller
)
{
    struct dma_allocation *allocations[7] = {
        &controller->admin.submission,
        &controller->admin.completion,
        &controller->identify.controller,
        &controller->identify.namespace_data,
        &controller->io.submission,
        &controller->io.completion,
        &controller->read.dma
    };

    for (size_t index = 0U; index < 7U; ++index) {
        if (dma_mark_initialized(allocations[index]) != DMA_STATUS_OK ||
            dma_transfer_to_device(allocations[index]) != DMA_STATUS_OK) {
            return NVME_STATUS_DMA_OWNERSHIP;
        }
    }
    controller->admin.submission_state = NVME_DMA_CONTROLLER_OWNED;
    controller->admin.completion_state = NVME_DMA_CONTROLLER_OWNED;
    controller->identify.controller_state = NVME_DMA_CONTROLLER_OWNED;
    controller->identify.namespace_state = NVME_DMA_CONTROLLER_OWNED;
    controller->io.submission_state = NVME_DMA_CONTROLLER_OWNED;
    controller->io.completion_state = NVME_DMA_CONTROLLER_OWNED;
    controller->read.state = NVME_DMA_CONTROLLER_OWNED;
    return NVME_STATUS_OK;
}

static bool dma_preparation_complete(const struct nvme_runtime *controller)
{
    const struct dma_allocation *allocations[7] = {
        &controller->admin.submission,
        &controller->admin.completion,
        &controller->identify.controller,
        &controller->identify.namespace_data,
        &controller->io.submission,
        &controller->io.completion,
        &controller->read.dma
    };

    for (size_t index = 0U; index < 7U; ++index) {
        if (!allocations[index]->active ||
            !allocations[index]->initialized ||
            allocations[index]->owner != DMA_OWNER_DEVICE) {
            return false;
        }
    }
    return true;
}

static struct pci_bus_master_request bus_master_request(
    struct nvme_runtime *controller
)
{
    struct pci_bus_master_request request = {0};

    request.allocations[0] = &controller->admin.submission;
    request.allocations[1] = &controller->admin.completion;
    request.allocations[2] = &controller->identify.controller;
    request.allocations[3] = &controller->identify.namespace_data;
    request.allocations[4] = &controller->io.submission;
    request.allocations[5] = &controller->io.completion;
    request.allocations[6] = &controller->read.dma;
    request.allocation_count = 7U;
    return request;
}

static enum nvme_status validate_prp(
    const struct dma_allocation *allocation,
    uint64_t offset,
    uint64_t length
)
{
    uint64_t end;
    uint64_t address;

    if (allocation == NULL || !allocation->active || length == 0U ||
        !add_checked(offset, length, &end) || end > allocation->byte_length ||
        !add_checked(physical_of(allocation), offset, &address) ||
        (address & (SAPOTE_PAGE_SIZE - 1U)) != 0U ||
        length > SAPOTE_PAGE_SIZE ||
        (address & ~(SAPOTE_PAGE_SIZE - 1U)) !=
            ((address + length - 1U) & ~(SAPOTE_PAGE_SIZE - 1U))) {
        return NVME_STATUS_PRP_INVALID;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status allocate_command_identifier(
    struct nvme_queue_pair *queue,
    uint16_t value
)
{
    if (queue == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (value == 0U || value > NVME_MAX_COMMAND_IDENTIFIER) {
        return NVME_STATUS_COMMAND_ID_RANGE;
    }
    if (queue->outstanding.active) {
        return queue->outstanding.value == value ?
            NVME_STATUS_COMMAND_ID_DUPLICATE : NVME_STATUS_QUEUE_FULL;
    }
    queue->outstanding.value = value;
    queue->outstanding.active = true;
    return NVME_STATUS_OK;
}

static enum nvme_status validate_completion(
    const struct nvme_completion_entry *entry,
    uint8_t expected_phase,
    uint16_t expected_command_identifier,
    uint16_t expected_submission_queue_identifier,
    uint16_t queue_depth
)
{
    uint16_t submission_head;
    uint16_t submission_queue_identifier;
    uint16_t command_identifier;
    uint16_t status;

    if (entry == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    status = (uint16_t)(entry->dword[3] >> 16U);
    if ((status & 1U) != expected_phase) {
        return NVME_STATUS_COMPLETION_PHASE;
    }
    command_identifier = (uint16_t)entry->dword[3];
    if (command_identifier != expected_command_identifier) {
        return NVME_STATUS_COMPLETION_COMMAND_ID;
    }
    submission_head = (uint16_t)entry->dword[2];
    submission_queue_identifier = (uint16_t)(entry->dword[2] >> 16U);
    if (submission_queue_identifier !=
            expected_submission_queue_identifier) {
        return NVME_STATUS_COMPLETION_QUEUE_ID;
    }
    if (submission_head >= queue_depth) {
        return NVME_STATUS_COMPLETION_LENGTH;
    }
    if ((status & UINT16_C(0xFFFE)) != 0U) {
        return NVME_STATUS_COMPLETION_STATUS;
    }
    return NVME_STATUS_OK;
}

static bool interrupt_prerequisites_ready(
    const struct nvme_runtime *controller
)
{
    return controller->interrupt.active &&
        controller->interrupt.handler_ready &&
        controller->interrupt.queues_ready &&
        controller->admin.active &&
        controller->admin.completion_state ==
            NVME_DMA_CONTROLLER_OWNED;
}

static bool doorbell_ready(
    const struct nvme_runtime *controller,
    const struct nvme_queue_pair *queue,
    const struct dma_allocation *data
)
{
    return controller->claim.state == NVME_CONTROLLER_RUNNING &&
        controller->bus_master_enabled &&
        controller->interrupt.delivery_enabled &&
        queue != NULL && queue->active && queue->outstanding.active &&
        queue->submission_state == NVME_DMA_CONTROLLER_OWNED &&
        queue->completion_state == NVME_DMA_CONTROLLER_OWNED &&
        (data == NULL || data->owner == DMA_OWNER_DEVICE);
}

static enum nvme_status validate_cpu_access(
    enum nvme_dma_object_state state
)
{
    return state == NVME_DMA_CPU_OWNED ? NVME_STATUS_OK :
        NVME_STATUS_DMA_OWNERSHIP;
}

static enum nvme_status teardown_observation_hook(
    struct nvme_runtime *controller
)
{
    if (controller != NULL && controller->teardown_started &&
        !controller->admin.completion.active) {
        controller->handler_saw_freed_state = true;
        return NVME_STATUS_TEARDOWN_RACE;
    }
    return NVME_STATUS_OK;
}

static uint64_t completion_doorbell_offset(
    const struct nvme_runtime *controller,
    const struct nvme_queue_pair *queue
)
{
    return queue->kind == NVME_QUEUE_ADMIN ?
        controller->registers.admin_completion_doorbell.offset :
        controller->registers.io_completion_doorbell.offset;
}

static void nvme_interrupt_handler(
    struct interrupt_frame *frame,
    void *opaque
)
{
    struct nvme_runtime *controller = opaque;
    struct nvme_queue_pair *queue;
    volatile struct nvme_completion_entry *entries;
    struct nvme_completion_entry entry;
    enum nvme_status completion_status;
    bool consumed = false;

    if (controller == NULL || frame == NULL ||
        teardown_observation_hook(controller) != NVME_STATUS_OK) {
        return;
    }
    if (frame->vector != controller->interrupt.vector) {
        controller->wrong_vector = true;
        return;
    }
    ++controller->interrupt_count;
    if (msix_set_masked(&controller->interrupt.msix, true) !=
            MSIX_STATUS_OK) {
        controller->ownership_failure = true;
        return;
    }
    controller->interrupt.delivery_enabled = false;
    queue = controller->expectation.queue;
    if (queue == NULL || !queue->active ||
        queue->completion_state != NVME_DMA_CONTROLLER_OWNED ||
        queue->completion.owner != DMA_OWNER_DEVICE ||
        dma_transfer_to_cpu(&queue->completion) != DMA_STATUS_OK) {
        controller->ownership_failure = true;
        goto unmask;
    }
    queue->completion_state = NVME_DMA_CPU_OWNED;
    __asm__ volatile ("" : : : "memory");
    entries = queue->completion.cpu_address;
    for (size_t dword = 0U; dword < 4U; ++dword) {
        entry.dword[dword] =
            entries[queue->completion_head].dword[dword];
    }
    completion_status = validate_completion(&entry,
        queue->completion_phase,
        controller->expectation.command_identifier,
        controller->expectation.submission_queue_identifier,
        queue->depth);
    if (completion_status == NVME_STATUS_COMPLETION_PHASE) {
        controller->expectation.result = completion_status;
    } else {
        consumed = true;
        ++queue->completion_head;
        if (queue->completion_head == queue->depth) {
            queue->completion_head = 0U;
            queue->completion_phase ^= 1U;
        }
        if (completion_status != NVME_STATUS_OK) {
            ++controller->ignored_completions;
            controller->expectation.result = completion_status;
        } else if (controller->expectation.data != NULL) {
            if (validate_prp(controller->expectation.data,
                    controller->expectation.data_offset,
                    controller->expectation.data_length) != NVME_STATUS_OK ||
                controller->expectation.data->owner != DMA_OWNER_DEVICE ||
                dma_transfer_to_cpu(controller->expectation.data) !=
                    DMA_STATUS_OK) {
                controller->ownership_failure = true;
                controller->expectation.result =
                    NVME_STATUS_COMPLETION_OWNERSHIP;
            } else {
                controller->expectation.result = NVME_STATUS_OK;
                controller->expectation.done = true;
            }
        } else {
            controller->expectation.result = NVME_STATUS_OK;
            controller->expectation.done = true;
        }
        if (controller->expectation.done) {
            queue->outstanding.active = false;
        }
    }
    if (dma_transfer_to_device(&queue->completion) != DMA_STATUS_OK) {
        controller->ownership_failure = true;
        goto unmask;
    }
    queue->completion_state = NVME_DMA_CONTROLLER_OWNED;
    if (consumed) {
        cpu_store_fence();
        mmio_write32(controller->mmio,
            completion_doorbell_offset(controller, queue),
            queue->completion_head);
    }

unmask:
    if (msix_set_masked(&controller->interrupt.msix, false) !=
            MSIX_STATUS_OK) {
        controller->ownership_failure = true;
    } else {
        controller->interrupt.delivery_enabled = true;
    }
}

static enum nvme_status configure_interrupt(
    struct nvme_runtime *controller
)
{
    const struct msix_state before = msix_get_state();
    const struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    const size_t mappings_before = controller->claim.pci.mapping_count;

    msix_test_inject_failure_once();
    if (msix_bind_masked(&controller->claim.pci, 0U,
            nvme_interrupt_handler, controller, &controller->interrupt.msix) !=
            MSIX_STATUS_INJECTED_FAILURE ||
        controller->interrupt.msix.active ||
        msix_get_state().active_bindings != before.active_bindings ||
        interrupt_vector_get_state().allocated != vectors_before.allocated ||
        controller->claim.pci.mapping_count != mappings_before) {
        return NVME_STATUS_MSIX_ROLLBACK_FAILURE;
    }
    if (msix_bind_masked(&controller->claim.pci, 0U,
            nvme_interrupt_handler, controller, &controller->interrupt.msix) !=
            MSIX_STATUS_OK ||
        !controller->interrupt.msix.delivery_masked) {
        return NVME_STATUS_MSIX_FAILURE;
    }
    controller->interrupt.vector =
        controller->interrupt.msix.vector.vector;
    controller->interrupt.handler_ready = true;
    controller->interrupt.queues_ready = controller->admin.active;
    controller->interrupt.delivery_enabled = false;
    controller->interrupt.active = true;
    return NVME_STATUS_OK;
}

static enum nvme_status prepare_controller_ownership(
    struct nvme_runtime *controller
)
{
    const struct pci_bus_master_request request =
        bus_master_request(controller);
    enum pci_resource_status pci_status;

    pci_status = pci_claim_enable_bus_master(&controller->claim.pci,
        &request);
    if (pci_status != PCI_RESOURCE_STATUS_DMA_NOT_PREPARED) {
        return NVME_STATUS_BUS_MASTER_PREMATURE;
    }
    const enum nvme_status status = transfer_all_to_controller(controller);

    if (status != NVME_STATUS_OK || !dma_preparation_complete(controller)) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status program_and_enable_controller(
    struct nvme_runtime *controller
)
{
    const struct pci_bus_master_request request =
        bus_master_request(controller);
    uint32_t configuration;
    enum pci_resource_status pci_status;
    enum nvme_status status;

    if (!dma_preparation_complete(controller)) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    pci_status = pci_claim_enable_bus_master(&controller->claim.pci,
        &request);
    if (pci_status != PCI_RESOURCE_STATUS_OK) {
        return NVME_STATUS_BUS_MASTER_FAILURE;
    }
    controller->bus_master_enabled = true;
    mmio_write32(controller->mmio, NVME_REG_AQA,
        (uint32_t)(NVME_QUEUE_DEPTH - 1U) |
            (uint32_t)(NVME_QUEUE_DEPTH - 1U) << 16U);
    mmio_write64(controller->mmio, NVME_REG_ASQ,
        physical_of(&controller->admin.submission));
    mmio_write64(controller->mmio, NVME_REG_ACQ,
        physical_of(&controller->admin.completion));
    configuration = mmio_read32(controller->mmio, NVME_REG_CC);
    configuration &= ~NVME_CC_WRITABLE_MASK;
    configuration |= NVME_CC_IOSQES | NVME_CC_IOCQES | NVME_CC_EN;
    cpu_store_fence();
    mmio_write32(controller->mmio, NVME_REG_CC, configuration);
    controller->controller_enabled = true;
    status = wait_ready(controller, true, NVME_STATUS_ENABLE_TIMEOUT);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    if (!interrupt_prerequisites_ready(controller)) {
        return NVME_STATUS_INTERRUPT_NOT_READY;
    }
    if (msix_set_masked(&controller->interrupt.msix, false) !=
            MSIX_STATUS_OK) {
        return NVME_STATUS_MSIX_FAILURE;
    }
    controller->interrupt.delivery_enabled = true;
    return NVME_STATUS_OK;
}

static enum nvme_status prepare_submission_entry(
    struct nvme_queue_pair *queue,
    struct nvme_submission_entry **entry
)
{
    if (queue == NULL || entry == NULL || !queue->active ||
        queue->submission_tail >= queue->depth ||
        queue->outstanding.active) {
        return NVME_STATUS_QUEUE_FULL;
    }
    if (queue->submission_state == NVME_DMA_CONTROLLER_OWNED) {
        if (queue->submission.owner != DMA_OWNER_DEVICE ||
            dma_transfer_to_cpu(&queue->submission) != DMA_STATUS_OK) {
            return NVME_STATUS_QUEUE_OWNERSHIP;
        }
        queue->submission_state = NVME_DMA_CPU_OWNED;
    }
    if (queue->submission_state != NVME_DMA_CPU_OWNED ||
        queue->submission.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_QUEUE_OWNERSHIP;
    }
    *entry = &((struct nvme_submission_entry *)
        queue->submission.cpu_address)[queue->submission_tail];
    return NVME_STATUS_OK;
}

static void set_prp1(
    struct nvme_submission_entry *entry,
    uint64_t address
)
{
    entry->dword[6] = (uint32_t)address;
    entry->dword[7] = (uint32_t)(address >> 32U);
}

static enum nvme_status wait_for_completion(
    struct nvme_runtime *controller,
    struct nvme_queue_pair *queue
)
{
    uint64_t deadline;
    enum nvme_status status;

    if (!add_checked(clock_monotonic_ns(), NVME_COMMAND_TIMEOUT_NS,
            &deadline)) {
        return NVME_STATUS_COMMAND_TIMEOUT;
    }
    cpu_interrupt_enable();
    while (!controller->expectation.done && !controller->wrong_vector &&
        !controller->ownership_failure &&
        !deadline_reached(clock_monotonic_ns(), deadline)) {
        if ((mmio_read32(controller->mmio, NVME_REG_CSTS) &
                NVME_CSTS_CFS) != 0U) {
            break;
        }
        __asm__ volatile ("" : : : "memory");
    }
    cpu_interrupt_disable();
    if ((mmio_read32(controller->mmio, NVME_REG_CSTS) &
            NVME_CSTS_CFS) != 0U) {
        return NVME_STATUS_CONTROLLER_FATAL;
    }
    if (controller->wrong_vector) {
        return NVME_STATUS_INTERRUPT_COUNT;
    }
    if (controller->ownership_failure) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    if (!controller->expectation.done) {
        return controller->expectation.result != NVME_STATUS_COMMAND_TIMEOUT ?
            controller->expectation.result : NVME_STATUS_COMMAND_TIMEOUT;
    }
    status = controller->expectation.result;
    if (status == NVME_STATUS_OK &&
        queue->submission.owner == DMA_OWNER_DEVICE) {
        if (dma_transfer_to_cpu(&queue->submission) != DMA_STATUS_OK) {
            return NVME_STATUS_DMA_OWNERSHIP;
        }
        queue->submission_state = NVME_DMA_CPU_OWNED;
    }
    return status;
}

static enum nvme_status submit_entry(
    struct nvme_runtime *controller,
    struct nvme_queue_pair *queue,
    uint16_t command_identifier,
    struct dma_allocation *data,
    uint64_t data_offset,
    uint64_t data_length
)
{
    uint64_t doorbell;
    enum nvme_status status;

    if (queue->submission_state != NVME_DMA_CPU_OWNED ||
        queue->submission.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_QUEUE_OWNERSHIP;
    }
    status = allocate_command_identifier(queue, command_identifier);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    if (data != NULL && (validate_prp(data, data_offset, data_length) !=
            NVME_STATUS_OK || data->owner != DMA_OWNER_DEVICE)) {
        queue->outstanding.active = false;
        return NVME_STATUS_PRP_INVALID;
    }
    controller->expectation.kind = queue->kind == NVME_QUEUE_ADMIN ?
        NVME_EXPECT_ADMIN : NVME_EXPECT_IO;
    controller->expectation.done = false;
    controller->expectation.result = NVME_STATUS_COMMAND_TIMEOUT;
    controller->expectation.queue = queue;
    controller->expectation.data = data;
    controller->expectation.data_offset = data_offset;
    controller->expectation.data_length = data_length;
    controller->expectation.command_identifier = command_identifier;
    controller->expectation.submission_queue_identifier = queue->identifier;
    if (dma_transfer_to_device(&queue->submission) != DMA_STATUS_OK) {
        queue->outstanding.active = false;
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    queue->submission_state = NVME_DMA_CONTROLLER_OWNED;
    if (!doorbell_ready(controller, queue, data)) {
        return NVME_STATUS_DOORBELL_PREMATURE;
    }
    ++queue->submission_tail;
    if (queue->submission_tail == queue->depth) {
        queue->submission_tail = 0U;
    }
    doorbell = queue->kind == NVME_QUEUE_ADMIN ?
        controller->registers.admin_submission_doorbell.offset :
        controller->registers.io_submission_doorbell.offset;
    cpu_store_fence();
    mmio_write32(controller->mmio, doorbell, queue->submission_tail);
    status = wait_for_completion(controller, queue);
    controller->expectation.kind = NVME_EXPECT_NONE;
    controller->expectation.queue = NULL;
    controller->expectation.data = NULL;
    return status;
}

static enum nvme_status identify_controller(
    struct nvme_runtime *controller
)
{
    struct nvme_submission_entry *entry;
    uint16_t command_identifier =
        controller->next_admin_command_identifier++;
    enum nvme_status status = prepare_submission_entry(
        &controller->admin, &entry);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_ADMIN_IDENTIFY |
        (uint32_t)command_identifier << 16U;
    set_prp1(entry, physical_of(&controller->identify.controller));
    entry->dword[10] = NVME_IDENTIFY_CONTROLLER;
    return submit_entry(controller, &controller->admin,
        command_identifier, &controller->identify.controller, 0U,
        NVME_IDENTIFY_BYTES);
}

static enum nvme_status validate_identify_controller(
    struct nvme_runtime *controller
)
{
    const uint8_t *data = controller->identify.controller.cpu_address;
    uint8_t sqes;
    uint8_t cqes;
    uint32_t namespace_limit;
    uint8_t mdts;
    uint64_t maximum_transfer;
    uint64_t minimum_page;

    if (controller->identify.controller.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->identify.controller_state = NVME_DMA_CPU_OWNED;
    sqes = data[NVME_IDENTIFY_CONTROLLER_SQES_OFFSET];
    cqes = data[NVME_IDENTIFY_CONTROLLER_CQES_OFFSET];
    namespace_limit = read_le32(data +
        NVME_IDENTIFY_CONTROLLER_NN_OFFSET);
    if ((sqes & 0xFU) > 6U || (sqes >> 4U) < 6U ||
        (cqes & 0xFU) > 4U || (cqes >> 4U) < 4U ||
        read_le32(data + NVME_IDENTIFY_CONTROLLER_VERSION_OFFSET) !=
            controller->capabilities.version ||
        data[NVME_IDENTIFY_CONTROLLER_TYPE_OFFSET] != 1U) {
        return NVME_STATUS_IDENTIFY_CONTROLLER;
    }
    if (namespace_limit == 0U) {
        return NVME_STATUS_NAMESPACE_ABSENT;
    }
    mdts = data[NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET];
    if (mdts != 0U) {
        if (mdts >= 64U ||
            controller->capabilities.minimum_page_shift >= 64U) {
            return NVME_STATUS_IDENTIFY_CONTROLLER;
        }
        minimum_page = UINT64_C(1) <<
            controller->capabilities.minimum_page_shift;
        if (!multiply_checked(UINT64_C(1) << mdts, minimum_page,
                &maximum_transfer) || maximum_transfer < NVME_BLOCK_BYTES) {
            return NVME_STATUS_IDENTIFY_CONTROLLER;
        }
    }
    return NVME_STATUS_OK;
}

static enum nvme_status identify_namespace(
    struct nvme_runtime *controller
)
{
    struct nvme_submission_entry *entry;
    uint16_t command_identifier =
        controller->next_admin_command_identifier++;
    enum nvme_status status = prepare_submission_entry(
        &controller->admin, &entry);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_ADMIN_IDENTIFY |
        (uint32_t)command_identifier << 16U;
    entry->dword[1] = NVME_NAMESPACE_IDENTIFIER;
    set_prp1(entry, physical_of(&controller->identify.namespace_data));
    entry->dword[10] = NVME_IDENTIFY_NAMESPACE;
    return submit_entry(controller, &controller->admin,
        command_identifier, &controller->identify.namespace_data, 0U,
        NVME_IDENTIFY_BYTES);
}

static bool all_zero(const uint8_t *data, size_t length)
{
    for (size_t index = 0U; index < length; ++index) {
        if (data[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum nvme_status identify_active_namespaces(
    struct nvme_runtime *controller
)
{
    struct nvme_submission_entry *entry;
    uint16_t command_identifier =
        controller->next_admin_command_identifier++;
    enum nvme_status status = prepare_submission_entry(
        &controller->admin, &entry);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_ADMIN_IDENTIFY |
        (uint32_t)command_identifier << 16U;
    set_prp1(entry, physical_of(&controller->identify.namespace_data));
    entry->dword[10] = NVME_IDENTIFY_ACTIVE_NAMESPACE_LIST;
    return submit_entry(controller, &controller->admin,
        command_identifier, &controller->identify.namespace_data, 0U,
        NVME_IDENTIFY_BYTES);
}

static enum nvme_status validate_active_namespace_list(
    struct nvme_runtime *controller
)
{
    const uint8_t *data =
        controller->identify.namespace_data.cpu_address;
    uint32_t first;

    if (controller->identify.namespace_data.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->identify.namespace_state = NVME_DMA_CPU_OWNED;
    first = read_le32(data);
    if (first == 0U) {
        return NVME_STATUS_NAMESPACE_ABSENT;
    }
    if (first != NVME_NAMESPACE_IDENTIFIER ||
        !all_zero(data + sizeof(uint32_t),
            NVME_IDENTIFY_BYTES - sizeof(uint32_t))) {
        return NVME_STATUS_MULTIPLE_NAMESPACES;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status prepare_namespace_identify_buffer(
    struct nvme_runtime *controller
)
{
    if (controller->identify.namespace_state != NVME_DMA_CPU_OWNED ||
        controller->identify.namespace_data.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    zero_bytes(controller->identify.namespace_data.cpu_address,
        controller->identify.namespace_data.byte_length);
    if (dma_transfer_to_device(&controller->identify.namespace_data) !=
            DMA_STATUS_OK) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->identify.namespace_state = NVME_DMA_CONTROLLER_OWNED;
    return NVME_STATUS_OK;
}

static enum nvme_status parse_namespace(
    const uint8_t *data,
    struct nvme_namespace_selection *selection
)
{
    uint64_t namespace_size;
    uint64_t namespace_capacity;
    uint64_t namespace_use;
    uint8_t number_of_formats;
    uint8_t formatted;
    uint8_t format_index;
    uint64_t format_offset;
    uint32_t format;
    uint16_t metadata_bytes;
    uint8_t lba_shift;

    if (data == NULL || selection == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (all_zero(data, NVME_IDENTIFY_BYTES)) {
        return NVME_STATUS_NAMESPACE_INACTIVE;
    }
    namespace_size = read_le64(data + NVME_IDENTIFY_NS_NSZE_OFFSET);
    namespace_capacity = read_le64(data + NVME_IDENTIFY_NS_NCAP_OFFSET);
    namespace_use = read_le64(data + NVME_IDENTIFY_NS_NUSE_OFFSET);
    if (namespace_size == 0U || namespace_capacity == 0U ||
        namespace_capacity > namespace_size ||
        namespace_use > namespace_capacity) {
        return namespace_size == 0U ? NVME_STATUS_NAMESPACE_INACTIVE :
            NVME_STATUS_IDENTIFY_NAMESPACE;
    }
    number_of_formats = (uint8_t)(
        data[NVME_IDENTIFY_NS_NLBAF_OFFSET] + 1U);
    if (number_of_formats == 0U || number_of_formats > 64U) {
        return NVME_STATUS_IDENTIFY_NAMESPACE;
    }
    formatted = data[NVME_IDENTIFY_NS_FLBAS_OFFSET];
    format_index = (uint8_t)((formatted & 0xFU) |
        ((formatted >> 5U) & 0x3U) << 4U);
    if (format_index >= number_of_formats) {
        return NVME_STATUS_LBA_FORMAT;
    }
    if ((formatted & UINT8_C(1U << 4)) != 0U) {
        return NVME_STATUS_METADATA;
    }
    if ((data[NVME_IDENTIFY_NS_DPS_OFFSET] & 0x7U) != 0U) {
        return NVME_STATUS_PROTECTION_INFORMATION;
    }
    format_offset = NVME_IDENTIFY_NS_LBAF_OFFSET +
        (uint64_t)format_index * NVME_IDENTIFY_NS_LBAF_BYTES;
    if (format_offset > NVME_IDENTIFY_BYTES -
            NVME_IDENTIFY_NS_LBAF_BYTES) {
        return NVME_STATUS_IDENTIFY_NAMESPACE;
    }
    format = read_le32(data + format_offset);
    metadata_bytes = (uint16_t)format;
    lba_shift = (uint8_t)(format >> 16U);
    if (metadata_bytes != 0U) {
        return NVME_STATUS_METADATA;
    }
    if (lba_shift != 12U) {
        return NVME_STATUS_LBA_FORMAT;
    }
    selection->identifier = NVME_NAMESPACE_IDENTIFIER;
    selection->logical_blocks = namespace_size;
    selection->logical_block_bytes = UINT32_C(1) << lba_shift;
    selection->format_index = format_index;
    selection->active = true;
    return NVME_STATUS_OK;
}

static enum nvme_status validate_block_range(
    const struct nvme_namespace_selection *selection,
    const struct nvme_logical_block_range *range
)
{
    uint64_t end;

    if (selection == NULL || range == NULL || !selection->active) {
        return NVME_STATUS_NAMESPACE_INACTIVE;
    }
    if (range->count == 0U) {
        return NVME_STATUS_BLOCK_ZERO_LENGTH;
    }
    if (!add_checked(range->first, range->count, &end)) {
        return NVME_STATUS_BLOCK_OVERFLOW;
    }
    if (end > selection->logical_blocks) {
        return NVME_STATUS_BLOCK_RANGE;
    }
    if (range->count != 1U ||
        selection->logical_block_bytes != NVME_BLOCK_BYTES) {
        return NVME_STATUS_LBA_FORMAT;
    }
    return NVME_STATUS_OK;
}

static enum nvme_status create_io_completion_queue(
    struct nvme_runtime *controller
)
{
    struct nvme_submission_entry *entry;
    uint16_t command_identifier =
        controller->next_admin_command_identifier++;
    enum nvme_status status = prepare_submission_entry(
        &controller->admin, &entry);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_ADMIN_CREATE_IO_CQ |
        (uint32_t)command_identifier << 16U;
    set_prp1(entry, physical_of(&controller->io.completion));
    entry->dword[10] = NVME_QUEUE_IDENTIFIER_IO |
        (uint32_t)(NVME_QUEUE_DEPTH - 1U) << 16U;
    entry->dword[11] = NVME_QUEUE_PHYSICALLY_CONTIGUOUS |
        NVME_QUEUE_INTERRUPT_ENABLED;
    return submit_entry(controller, &controller->admin,
        command_identifier, NULL, 0U, 0U);
}

static enum nvme_status create_io_submission_queue(
    struct nvme_runtime *controller
)
{
    struct nvme_submission_entry *entry;
    uint16_t command_identifier =
        controller->next_admin_command_identifier++;
    enum nvme_status status = prepare_submission_entry(
        &controller->admin, &entry);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_ADMIN_CREATE_IO_SQ |
        (uint32_t)command_identifier << 16U;
    set_prp1(entry, physical_of(&controller->io.submission));
    entry->dword[10] = NVME_QUEUE_IDENTIFIER_IO |
        (uint32_t)(NVME_QUEUE_DEPTH - 1U) << 16U;
    entry->dword[11] = NVME_QUEUE_PHYSICALLY_CONTIGUOUS |
        (uint32_t)NVME_QUEUE_IDENTIFIER_IO << 16U;
    return submit_entry(controller, &controller->admin,
        command_identifier, NULL, 0U, 0U);
}

static enum nvme_status create_io_queues(struct nvme_runtime *controller)
{
    enum nvme_status status = create_io_completion_queue(controller);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    status = create_io_submission_queue(controller);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    controller->io.active = true;
    return NVME_STATUS_OK;
}

static enum nvme_status submit_read_at(
    struct nvme_runtime *controller,
    uint64_t lba,
    uint16_t command_identifier
)
{
    const struct nvme_logical_block_range range = {
        .first = lba,
        .count = 1U
    };
    struct nvme_submission_entry *entry;
    uint64_t prp;
    enum nvme_status status = validate_block_range(
        &controller->namespace_data, &range);

    if (status != NVME_STATUS_OK) {
        return status;
    }
    status = validate_prp(&controller->read.dma,
        controller->read.data_offset, controller->read.data_length);
    if (status != NVME_STATUS_OK ||
        controller->read.state != NVME_DMA_CONTROLLER_OWNED) {
        return NVME_STATUS_PRP_INVALID;
    }
    status = prepare_submission_entry(&controller->io, &entry);
    if (status != NVME_STATUS_OK) {
        return status;
    }
    prp = physical_of(&controller->read.dma) +
        controller->read.data_offset;
    zero_bytes(entry, sizeof(*entry));
    entry->dword[0] = NVME_NVM_READ |
        (uint32_t)command_identifier << 16U;
    entry->dword[1] = NVME_NAMESPACE_IDENTIFIER;
    set_prp1(entry, prp);
    entry->dword[10] = (uint32_t)range.first;
    entry->dword[11] = (uint32_t)(range.first >> 32U);
    entry->dword[12] = 0U;
    return submit_entry(controller, &controller->io, command_identifier,
        &controller->read.dma, controller->read.data_offset,
        controller->read.data_length);
}

static enum nvme_status submit_read(struct nvme_runtime *controller)
{
    return submit_read_at(controller, NVME_FIXTURE_LBA, 1U);
}

static enum nvme_status prepare_guarded_read(
    struct nvme_runtime *controller
)
{
    if (controller == NULL || !controller->read.dma.active) {
        return NVME_STATUS_SESSION_INVALID;
    }
    if (controller->read.dma.owner == DMA_OWNER_DEVICE) {
        if (dma_transfer_to_cpu(&controller->read.dma) != DMA_STATUS_OK) {
            return NVME_STATUS_DMA_OWNERSHIP;
        }
    }
    if (controller->read.dma.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->read.state = NVME_DMA_CPU_OWNED;
    fill_bytes(controller->read.dma.cpu_address,
        controller->read.dma.byte_length, NVME_SENTINEL);
    controller->read.changed_while_controller_owned = false;
    cpu_store_fence();
    if (dma_transfer_to_device(&controller->read.dma) != DMA_STATUS_OK) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->read.state = NVME_DMA_CONTROLLER_OWNED;
    return NVME_STATUS_OK;
}

static enum nvme_status inspect_guarded_read(
    struct nvme_runtime *controller,
    bool *changed
)
{
    const uint8_t *allocation;
    const uint8_t *data;

    if (changed != NULL) {
        *changed = false;
    }
    if (controller == NULL || changed == NULL ||
        controller->read.dma.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    allocation = controller->read.dma.cpu_address;
    data = allocation + controller->read.data_offset;
    controller->read.state = NVME_DMA_CPU_OWNED;
    for (size_t index = 0U; index < NVME_BLOCK_BYTES; ++index) {
        if (data[index] != NVME_SENTINEL) {
            *changed = true;
        }
    }
    for (uint64_t index = 0U; index < controller->read.data_offset;
         ++index) {
        if (allocation[index] != NVME_SENTINEL) {
            return NVME_STATUS_SENTINEL_MISMATCH;
        }
    }
    for (uint64_t index = controller->read.data_offset +
            controller->read.data_length;
         index < controller->read.dma.byte_length; ++index) {
        if (allocation[index] != NVME_SENTINEL) {
            return NVME_STATUS_SENTINEL_MISMATCH;
        }
    }
    return NVME_STATUS_OK;
}

static enum nvme_status validate_guarded_read(
    struct nvme_runtime *controller
)
{
    bool changed;
    enum nvme_status result = inspect_guarded_read(controller, &changed);

    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (!changed) {
        return NVME_STATUS_CONTENT_MISMATCH;
    }
    controller->read.changed_while_controller_owned = true;
    return NVME_STATUS_OK;
}

static uint8_t fixture_byte(size_t index)
{
    return (uint8_t)((index * 37U + 11U) & 0xFFU);
}

static enum nvme_status validate_read_data(struct nvme_runtime *controller)
{
    const uint8_t *data;
    bool changed;
    enum nvme_status result;

    result = inspect_guarded_read(controller, &changed);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    data = (const uint8_t *)controller->read.dma.cpu_address +
        controller->read.data_offset;
    for (size_t index = 0U; index < NVME_BLOCK_BYTES; ++index) {
        if (data[index] != fixture_byte(index)) {
            return NVME_STATUS_CONTENT_MISMATCH;
        }
    }
    if (!changed) {
        return NVME_STATUS_CONTENT_MISMATCH;
    }
    controller->read.changed_while_controller_owned = true;
    return NVME_STATUS_OK;
}

static bool resource_state_matches(
    struct pci_resource_state pci_before,
    struct dma_state dma_before,
    struct interrupt_vector_state vectors_before,
    struct msix_state msix_before,
    struct frame_allocator_stats frames_before
)
{
    const struct pci_resource_state pci_after = pci_resource_get_state();
    const struct dma_state dma_after = dma_get_state();
    const struct interrupt_vector_state vectors_after =
        interrupt_vector_get_state();
    const struct msix_state msix_after = msix_get_state();
    const struct frame_allocator_stats frames_after =
        frame_allocator_get_stats();

    return pci_after.active_claims == pci_before.active_claims &&
        pci_after.active_mappings == pci_before.active_mappings &&
        pci_after.arena_pages == pci_before.arena_pages &&
        pci_after.mapped_pages == pci_before.mapped_pages &&
        pci_after.bus_masters == pci_before.bus_masters &&
        pci_after.active == pci_before.active &&
        dma_after.active_allocations == dma_before.active_allocations &&
        dma_after.cpu_owned_allocations == dma_before.cpu_owned_allocations &&
        dma_after.device_owned_allocations ==
            dma_before.device_owned_allocations &&
        dma_after.active == dma_before.active &&
        vectors_after.capacity == vectors_before.capacity &&
        vectors_after.allocated == vectors_before.allocated &&
        vectors_after.free == vectors_before.free &&
        vectors_after.active == vectors_before.active &&
        msix_after.active_bindings == msix_before.active_bindings &&
        !msix_after.failure_injection_armed &&
        !msix_before.failure_injection_armed &&
        frames_after.addressable_frames == frames_before.addressable_frames &&
        frames_after.allocatable_frames ==
            frames_before.allocatable_frames &&
        frames_after.free_frames == frames_before.free_frames &&
        frames_after.allocated_frames == frames_before.allocated_frames &&
        frames_after.reserved_frames == frames_before.reserved_frames &&
        frames_after.highest_allocatable_address ==
            frames_before.highest_allocatable_address;
}

static enum nvme_status reclaim_all(struct nvme_runtime *controller)
{
    struct dma_allocation *allocations[7] = {
        &controller->admin.submission,
        &controller->admin.completion,
        &controller->identify.controller,
        &controller->identify.namespace_data,
        &controller->io.submission,
        &controller->io.completion,
        &controller->read.dma
    };

    for (size_t index = 0U; index < 7U; ++index) {
        if (allocations[index]->active &&
            allocations[index]->owner == DMA_OWNER_DEVICE &&
            dma_transfer_to_cpu(allocations[index]) != DMA_STATUS_OK) {
            return NVME_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->admin.submission.active) {
        set_dma_state(&controller->admin.submission_state,
            controller->admin.submission.owner);
    }
    if (controller->admin.completion.active) {
        set_dma_state(&controller->admin.completion_state,
            controller->admin.completion.owner);
    }
    if (controller->identify.controller.active) {
        set_dma_state(&controller->identify.controller_state,
            controller->identify.controller.owner);
    }
    if (controller->identify.namespace_data.active) {
        set_dma_state(&controller->identify.namespace_state,
            controller->identify.namespace_data.owner);
    }
    if (controller->io.submission.active) {
        set_dma_state(&controller->io.submission_state,
            controller->io.submission.owner);
    }
    if (controller->io.completion.active) {
        set_dma_state(&controller->io.completion_state,
            controller->io.completion.owner);
    }
    if (controller->read.dma.active) {
        set_dma_state(&controller->read.state, controller->read.dma.owner);
    }
    return NVME_STATUS_OK;
}

static enum nvme_status release_all(struct nvme_runtime *controller)
{
    struct dma_allocation *reverse[7] = {
        &controller->read.dma,
        &controller->io.completion,
        &controller->io.submission,
        &controller->identify.namespace_data,
        &controller->identify.controller,
        &controller->admin.completion,
        &controller->admin.submission
    };

    for (size_t index = 0U; index < 7U; ++index) {
        if (reverse[index]->active &&
            dma_release(reverse[index]) != DMA_STATUS_OK) {
            return NVME_STATUS_TEARDOWN_FAILURE;
        }
    }
    controller->admin.submission_state = NVME_DMA_RECLAIMED;
    controller->admin.completion_state = NVME_DMA_RECLAIMED;
    controller->identify.controller_state = NVME_DMA_RECLAIMED;
    controller->identify.namespace_state = NVME_DMA_RECLAIMED;
    controller->io.submission_state = NVME_DMA_RECLAIMED;
    controller->io.completion_state = NVME_DMA_RECLAIMED;
    controller->read.state = NVME_DMA_RECLAIMED;
    return NVME_STATUS_OK;
}

static enum nvme_status teardown_controller(
    struct nvme_runtime *controller
)
{
    enum nvme_status result = NVME_STATUS_OK;
    bool interrupts_were_enabled = cpu_interrupts_enabled();
    bool dma_stopped = true;

    if (interrupts_were_enabled) {
        cpu_interrupt_disable();
    }
    controller->teardown_started = true;
    if (controller->claim.state == NVME_CONTROLLER_RUNNING &&
        transition(&controller->claim, NVME_CONTROLLER_STOPPING) !=
            NVME_STATUS_OK) {
        result = NVME_STATUS_TEARDOWN_FAILURE;
    }
    if (controller->mmio != NULL &&
        (controller->controller_enabled ||
         (mmio_read32(controller->mmio, NVME_REG_CSTS) &
            NVME_CSTS_RDY) != 0U)) {
        if (disable_controller(controller) != NVME_STATUS_OK) {
            result = NVME_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->interrupt.active) {
        if (msix_set_masked(&controller->interrupt.msix, true) !=
                MSIX_STATUS_OK) {
            result = NVME_STATUS_TEARDOWN_FAILURE;
        } else {
            controller->interrupt.delivery_enabled = false;
        }
    }
    if (controller->bus_master_enabled) {
        if (pci_claim_disable_bus_master(&controller->claim.pci) !=
                PCI_RESOURCE_STATUS_OK) {
            result = NVME_STATUS_TEARDOWN_FAILURE;
            dma_stopped = false;
        } else {
            controller->bus_master_enabled = false;
        }
    }
    if (controller->interrupt.active) {
        if (msix_unbind(&controller->interrupt.msix) != MSIX_STATUS_OK) {
            result = NVME_STATUS_TEARDOWN_FAILURE;
        } else {
            controller->interrupt.active = false;
            controller->interrupt.handler_ready = false;
        }
    }
    if (dma_stopped) {
        if (reclaim_all(controller) != NVME_STATUS_OK ||
            controller->handler_saw_freed_state ||
            release_all(controller) != NVME_STATUS_OK) {
            result = controller->handler_saw_freed_state ?
                NVME_STATUS_TEARDOWN_RACE :
                NVME_STATUS_TEARDOWN_FAILURE;
        }
        if (controller->claim.pci.active &&
            pci_release_device(&controller->claim.pci) !=
                PCI_RESOURCE_STATUS_OK) {
            result = NVME_STATUS_TEARDOWN_FAILURE;
        }
    }
    if (controller->claim.state != NVME_CONTROLLER_UNINITIALIZED &&
        controller->claim.state != NVME_CONTROLLER_RELEASED &&
        transition(&controller->claim, NVME_CONTROLLER_RELEASED) !=
            NVME_STATUS_OK) {
        result = NVME_STATUS_TEARDOWN_FAILURE;
    }
    if (interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return result;
}

static bool exercise_cleanup_boundary(size_t boundary)
{
    struct nvme_runtime controller = {0};
    const struct pci_resource_state pci_before = pci_resource_get_state();
    const struct dma_state dma_before = dma_get_state();
    const struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    const struct msix_state msix_before = msix_get_state();
    const struct frame_allocator_stats frames_before =
        frame_allocator_get_stats();
    struct dma_allocation *allocations[7] = {
        &controller.admin.submission,
        &controller.admin.completion,
        &controller.identify.controller,
        &controller.identify.namespace_data,
        &controller.io.submission,
        &controller.io.completion,
        &controller.read.dma
    };
    bool prepared = true;

    if (boundary > 7U) {
        return false;
    }
    for (size_t index = 0U; index < boundary; ++index) {
        if (allocate_dma(allocations[index], 1U) != NVME_STATUS_OK ||
            dma_mark_initialized(allocations[index]) != DMA_STATUS_OK ||
            dma_transfer_to_device(allocations[index]) != DMA_STATUS_OK) {
            prepared = false;
            break;
        }
    }
    if (reclaim_all(&controller) != NVME_STATUS_OK ||
        release_all(&controller) != NVME_STATUS_OK) {
        return false;
    }
    return prepared && resource_state_matches(pci_before, dma_before,
        vectors_before, msix_before, frames_before);
}

static bool exercise_owned_release_refusal(void)
{
    const struct pci_resource_state pci_before = pci_resource_get_state();
    const struct dma_state dma_before = dma_get_state();
    const struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    const struct msix_state msix_before = msix_get_state();
    const struct frame_allocator_stats frames_before =
        frame_allocator_get_stats();
    struct dma_allocation allocation = {0};
    bool refused;

    if (allocate_dma(&allocation, 1U) != NVME_STATUS_OK ||
        dma_mark_initialized(&allocation) != DMA_STATUS_OK ||
        dma_transfer_to_device(&allocation) != DMA_STATUS_OK) {
        return false;
    }
    refused = validate_cpu_access(NVME_DMA_CONTROLLER_OWNED) ==
            NVME_STATUS_DMA_OWNERSHIP &&
        dma_release(&allocation) == DMA_STATUS_WRONG_OWNER;
    if (dma_transfer_to_cpu(&allocation) != DMA_STATUS_OK ||
        dma_release(&allocation) != DMA_STATUS_OK) {
        return false;
    }
    return refused && resource_state_matches(pci_before, dma_before,
        vectors_before, msix_before, frames_before);
}

static bool test_record(bool passed, size_t *completed)
{
    if (!passed) {
        return false;
    }
    ++*completed;
    return true;
}

bool nvme_foundation_self_test(size_t *completed_tests)
{
    struct nvme_register_span span = {0};
    struct nvme_controller_capabilities capabilities = {0};
    struct nvme_controller_claim lifecycle = {0};
    struct nvme_queue_pair queue = {0};
    struct nvme_completion_entry completion = {0};
    struct nvme_namespace_selection selection = {
        .logical_blocks = 16U,
        .logical_block_bytes = NVME_BLOCK_BYTES,
        .active = true
    };
    struct nvme_logical_block_range range = {0};
    struct nvme_runtime prerequisites = {0};
    struct nvme_runtime teardown_race = {0};
    struct nvme_runtime malformed_identify = {0};
    uint8_t identify_controller_data[NVME_IDENTIFY_BYTES] = {0};
    uint8_t identify_namespace_data[NVME_IDENTIFY_BYTES] = {0};
    size_t completed = 0U;
    uint64_t offset = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;

    /* 1: fixed registers and doorbells reject range, overflow and alignment. */
    if (!test_record(validate_span(UINT64_C(0x1000), UINT64_C(0xFF0),
            UINT64_C(0x20), 4U, &span) ==
            NVME_STATUS_REGISTER_OUTSIDE_BAR &&
        validate_span(UINT64_MAX, UINT64_MAX - 3U, 8U, 4U, &span) ==
            NVME_STATUS_REGISTER_OVERFLOW &&
        validate_span(UINT64_C(0x2000), UINT64_C(0x1001), 4U, 4U,
            &span) == NVME_STATUS_REGISTER_ALIGNMENT, &completed)) {
        return false;
    }
    /* 2: encoded maximums do not wrap and impossible doorbells reject. */
    if (!test_record(decode_capabilities(
            UINT64_C(0xFFFF) | NVME_CAP_CSS_NVM << NVME_CAP_CSS_SHIFT,
            UINT32_C(0x00010400), &capabilities) == NVME_STATUS_OK &&
        capabilities.maximum_queue_entries == UINT32_C(65536) &&
        doorbell_offset(UINT64_C(1) << 63U, UINT16_MAX, true, &offset) ==
            NVME_STATUS_CAP_DOORBELL_GEOMETRY, &completed)) {
        return false;
    }
    /* 3: unsupported command sets and page-size combinations are named. */
    if (!test_record(decode_capabilities(UINT64_C(1),
            UINT32_C(0x00010400), &capabilities) ==
            NVME_STATUS_UNSUPPORTED_COMMAND_SET &&
        decode_capabilities(UINT64_C(1) |
            NVME_CAP_CSS_NVM << NVME_CAP_CSS_SHIFT |
            UINT64_C(1) << NVME_CAP_MPSMIN_SHIFT,
            UINT32_C(0x00010400), &capabilities) ==
            NVME_STATUS_UNSUPPORTED_PAGE_SIZE, &completed)) {
        return false;
    }
    /* 4: controller enable and disable expiries remain distinct and finite. */
    if (!test_record(NVME_STATUS_DISABLE_TIMEOUT !=
            NVME_STATUS_ENABLE_TIMEOUT &&
        deadline_reached(NVME_TIMEOUT_UNIT_NS, NVME_TIMEOUT_UNIT_NS) &&
        NVME_COMMAND_TIMEOUT_NS != 0U, &completed)) {
        return false;
    }
    /* 5: malformed, misaligned and overflowing Admin queues reject. */
    if (!test_record(validate_queue_geometry(UINT64_C(0x1001),
            SAPOTE_PAGE_SIZE, NVME_SUBMISSION_ENTRY_BYTES,
            NVME_QUEUE_DEPTH, NVME_STATUS_ADMIN_QUEUE_INVALID) ==
                NVME_STATUS_ADMIN_QUEUE_INVALID &&
        validate_queue_geometry(UINT64_MAX - 4095U,
            SAPOTE_PAGE_SIZE * 2U, NVME_SUBMISSION_ENTRY_BYTES, 128U,
            NVME_STATUS_ADMIN_QUEUE_INVALID) ==
                NVME_STATUS_ADMIN_QUEUE_INVALID, &completed)) {
        return false;
    }
    /* 6: malformed, short and overflowing I/O queues reject independently. */
    if (!test_record(validate_queue_geometry(UINT64_C(0x2000), 16U,
            NVME_COMPLETION_ENTRY_BYTES, NVME_QUEUE_DEPTH,
            NVME_STATUS_IO_QUEUE_INVALID) == NVME_STATUS_IO_QUEUE_INVALID &&
        validate_queue_geometry(UINT64_C(0x2000), SAPOTE_PAGE_SIZE,
            NVME_COMPLETION_ENTRY_BYTES, SIZE_MAX,
            NVME_STATUS_IO_QUEUE_INVALID) == NVME_STATUS_IO_QUEUE_INVALID,
            &completed)) {
        return false;
    }
    /* 7: queue submission rejects wrong phase and controller ownership. */
    completion.dword[3] = UINT32_C(1U << 16) | 1U;
    if (!test_record(validate_completion(&completion, 0U, 1U, 0U, 2U) ==
            NVME_STATUS_COMPLETION_PHASE &&
        validate_cpu_access(NVME_DMA_CONTROLLER_OWNED) ==
            NVME_STATUS_DMA_OWNERSHIP, &completed)) {
        return false;
    }
    /* 8: duplicate, zero and reserved command identifiers reject. */
    queue.outstanding.active = true;
    queue.outstanding.value = 1U;
    if (!test_record(allocate_command_identifier(&queue, 1U) ==
            NVME_STATUS_COMMAND_ID_DUPLICATE &&
        (queue.outstanding.active = false,
            allocate_command_identifier(&queue, 0U)) ==
                NVME_STATUS_COMMAND_ID_RANGE &&
        allocate_command_identifier(&queue, UINT16_MAX) ==
            NVME_STATUS_COMMAND_ID_RANGE, &completed)) {
        return false;
    }
    /* 9: malformed Identify Controller and Namespace structures reject. */
    identify_controller_data[NVME_IDENTIFY_CONTROLLER_SQES_OFFSET] = 0x55U;
    identify_namespace_data[NVME_IDENTIFY_NS_NSZE_OFFSET] = 1U;
    malformed_identify.identify.controller.cpu_address =
        identify_controller_data;
    malformed_identify.identify.controller.owner = DMA_OWNER_CPU;
    malformed_identify.capabilities.version = UINT32_C(0x00010400);
    if (!test_record(validate_identify_controller(&malformed_identify) ==
            NVME_STATUS_IDENTIFY_CONTROLLER &&
        parse_namespace(identify_namespace_data, &selection) ==
            NVME_STATUS_IDENTIFY_NAMESPACE, &completed)) {
        return false;
    }
    /* 10: absent, multiple and inactive namespace results stay distinct. */
    zero_bytes(identify_namespace_data, sizeof(identify_namespace_data));
    malformed_identify.identify.namespace_data.cpu_address =
        identify_namespace_data;
    malformed_identify.identify.namespace_data.owner = DMA_OWNER_CPU;
    if (!test_record(read_le32(identify_controller_data +
            NVME_IDENTIFY_CONTROLLER_NN_OFFSET) == 0U &&
        validate_active_namespace_list(&malformed_identify) ==
            NVME_STATUS_NAMESPACE_ABSENT &&
        (identify_namespace_data[0] = 1U,
            identify_namespace_data[4] = 2U,
            validate_active_namespace_list(&malformed_identify)) ==
                NVME_STATUS_MULTIPLE_NAMESPACES &&
        (zero_bytes(identify_namespace_data,
            sizeof(identify_namespace_data)), true) &&
        parse_namespace(identify_namespace_data, &selection) ==
            NVME_STATUS_NAMESPACE_INACTIVE, &completed)) {
        return false;
    }
    /* 11: unsupported LBA, metadata and protection settings are distinct. */
    zero_bytes(identify_namespace_data, sizeof(identify_namespace_data));
    identify_namespace_data[0] = 16U;
    identify_namespace_data[8] = 16U;
    identify_namespace_data[NVME_IDENTIFY_NS_LBAF_OFFSET + 2U] = 9U;
    if (!test_record(parse_namespace(identify_namespace_data, &selection) ==
            NVME_STATUS_LBA_FORMAT &&
        (identify_namespace_data[NVME_IDENTIFY_NS_LBAF_OFFSET] = 8U,
            parse_namespace(identify_namespace_data, &selection)) ==
                NVME_STATUS_METADATA &&
        (identify_namespace_data[NVME_IDENTIFY_NS_LBAF_OFFSET] = 0U,
            identify_namespace_data[
                NVME_IDENTIFY_NS_LBAF_OFFSET + 2U] = 12U,
            identify_namespace_data[NVME_IDENTIFY_NS_DPS_OFFSET] = 1U,
            parse_namespace(identify_namespace_data, &selection)) ==
                NVME_STATUS_PROTECTION_INFORMATION, &completed)) {
        return false;
    }
    /* 12: zero, out-of-range and overflowing block requests are named. */
    selection.logical_blocks = 16U;
    selection.logical_block_bytes = NVME_BLOCK_BYTES;
    selection.active = true;
    if (!test_record(validate_block_range(&selection, &range) ==
            NVME_STATUS_BLOCK_ZERO_LENGTH &&
        (range.first = 16U, range.count = 1U,
            validate_block_range(&selection, &range)) ==
                NVME_STATUS_BLOCK_RANGE &&
        (range.first = UINT64_MAX, range.count = 2U,
            validate_block_range(&selection, &range)) ==
                NVME_STATUS_BLOCK_OVERFLOW, &completed)) {
        return false;
    }
    /* 13: PRP1 must be inside one allocation and one DMA page. */
    struct dma_allocation synthetic_dma = {
        .frames = {.physical_base = UINT64_C(0x1000), .active = true},
        .cpu_address = identify_namespace_data,
        .byte_length = SAPOTE_PAGE_SIZE * 2U,
        .owner = DMA_OWNER_CPU,
        .active = true
    };
    if (!test_record(validate_prp(&synthetic_dma, SAPOTE_PAGE_SIZE,
            SAPOTE_PAGE_SIZE) == NVME_STATUS_OK &&
        validate_prp(&synthetic_dma, SAPOTE_PAGE_SIZE - 4U, 8U) ==
            NVME_STATUS_PRP_INVALID &&
        validate_prp(&synthetic_dma, SAPOTE_PAGE_SIZE * 2U,
            SAPOTE_PAGE_SIZE) == NVME_STATUS_PRP_INVALID, &completed)) {
        return false;
    }
    /* 14: phase, CID, SQID and status mismatches never report success. */
    completion.dword[2] = 0U;
    completion.dword[3] = UINT32_C(1U << 16) | 1U;
    if (!test_record(validate_completion(&completion, 0U, 1U, 0U, 2U) ==
            NVME_STATUS_COMPLETION_PHASE &&
        validate_completion(&completion, 1U, 2U, 0U, 2U) ==
            NVME_STATUS_COMPLETION_COMMAND_ID &&
        (completion.dword[2] = UINT32_C(1U << 16),
            validate_completion(&completion, 1U, 1U, 0U, 2U)) ==
                NVME_STATUS_COMPLETION_QUEUE_ID &&
        (completion.dword[2] = 0U,
            completion.dword[3] |= UINT32_C(1U << 17),
            validate_completion(&completion, 1U, 1U, 0U, 2U)) ==
                NVME_STATUS_COMPLETION_STATUS, &completed)) {
        return false;
    }
    /* 15: handler, MSI-X binding and controller-owned Admin CQ gate delivery. */
    prerequisites.interrupt.active = true;
    prerequisites.interrupt.handler_ready = true;
    prerequisites.interrupt.queues_ready = true;
    prerequisites.admin.active = true;
    if (!test_record(!interrupt_prerequisites_ready(&prerequisites) &&
        (prerequisites.admin.completion_state =
            NVME_DMA_CONTROLLER_OWNED,
            interrupt_prerequisites_ready(&prerequisites)), &completed)) {
        return false;
    }
    /* 16: a CPU-owned queue or PRP prevents the corresponding doorbell. */
    prerequisites.claim.state = NVME_CONTROLLER_RUNNING;
    prerequisites.bus_master_enabled = true;
    prerequisites.interrupt.delivery_enabled = true;
    prerequisites.admin.outstanding.active = true;
    prerequisites.admin.submission_state = NVME_DMA_CPU_OWNED;
    if (!test_record(!doorbell_ready(&prerequisites,
            &prerequisites.admin, NULL) &&
        (prerequisites.admin.submission_state =
            NVME_DMA_CONTROLLER_OWNED,
            doorbell_ready(&prerequisites, &prerequisites.admin, NULL)),
            &completed)) {
        return false;
    }
    /* 17: bus-master readiness is false until all seven DMA objects belong. */
    struct dma_allocation *prepared[7] = {
        &prerequisites.admin.submission,
        &prerequisites.admin.completion,
        &prerequisites.identify.controller,
        &prerequisites.identify.namespace_data,
        &prerequisites.io.submission,
        &prerequisites.io.completion,
        &prerequisites.read.dma
    };
    for (size_t index = 0U; index < 7U; ++index) {
        prepared[index]->active = true;
        prepared[index]->initialized = true;
        prepared[index]->owner = DMA_OWNER_DEVICE;
    }
    prerequisites.admin.submission.owner = DMA_OWNER_CPU;
    if (!test_record(!dma_preparation_complete(&prerequisites) &&
        (prerequisites.admin.submission.owner = DMA_OWNER_DEVICE,
            dma_preparation_complete(&prerequisites)), &completed)) {
        return false;
    }
    /* 18: CPU inspection and release are both forbidden while owned. */
    if (!test_record(validate_cpu_access(NVME_DMA_CPU_OWNED) ==
            NVME_STATUS_OK &&
        validate_cpu_access(NVME_DMA_CONTROLLER_OWNED) ==
            NVME_STATUS_DMA_OWNERSHIP &&
        validate_cpu_access(NVME_DMA_RECLAIMED) ==
            NVME_STATUS_DMA_OWNERSHIP &&
        exercise_owned_release_refusal(), &completed)) {
        return false;
    }
    /* 19: every partial DMA/queue boundary unwinds without retained state. */
    bool every_boundary_clean = true;
    for (size_t boundary = 0U; boundary <= 7U; ++boundary) {
        every_boundary_clean = every_boundary_clean &&
            exercise_cleanup_boundary(boundary);
    }
    if (!test_record(every_boundary_clean, &completed)) {
        return false;
    }
    /* 20: the teardown hook detects freed state without inspecting it. */
    teardown_race.teardown_started = true;
    if (!test_record(teardown_observation_hook(&teardown_race) ==
            NVME_STATUS_TEARDOWN_RACE &&
        teardown_race.handler_saw_freed_state &&
        (teardown_race.handler_saw_freed_state = false,
            teardown_race.admin.completion.active = true,
            teardown_observation_hook(&teardown_race)) == NVME_STATUS_OK &&
        !teardown_race.handler_saw_freed_state, &completed)) {
        return false;
    }

    if (transition(&lifecycle, NVME_CONTROLLER_DISCOVERED) !=
            NVME_STATUS_OK ||
        transition(&lifecycle, NVME_CONTROLLER_DISCOVERED) !=
            NVME_STATUS_TRANSITION_REPEATED ||
        transition(&lifecycle, NVME_CONTROLLER_CLAIMED) != NVME_STATUS_OK ||
        transition(&lifecycle, NVME_CONTROLLER_DISCOVERED) !=
            NVME_STATUS_TRANSITION_REVERSED ||
        transition(&lifecycle, NVME_CONTROLLER_RUNNING) !=
            NVME_STATUS_TRANSITION_INVALID ||
        completed != NVME_FOUNDATION_ROBUSTNESS_TESTS) {
        return false;
    }
    *completed_tests = completed;
    return true;
}

/*
 * The one shared bring-up path for the v0.5.0 raw-block proof and the private
 * filesystem session. It owns the controller once, identifies namespace one,
 * validates the 4096-byte LBA format and creates the one I/O queue pair.
 */
static enum nvme_status initialize_runtime(struct nvme_runtime *controller)
{
    const struct pci_function *function;
    enum nvme_status result;

    if (controller == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    function = discover_controller(&result);
    if (function == NULL) {
        return result;
    }
    controller->claim.discovery.address = function->address;
    controller->claim.discovery.generation = ++controller_generation;
    controller->claim.discovery.active = true;
    controller->next_admin_command_identifier = 1U;
    if (transition(&controller->claim, NVME_CONTROLLER_DISCOVERED) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    if (function->class_code != PCI_CLASS_MASS_STORAGE ||
        function->subclass != NVME_PCI_SUBCLASS_NON_VOLATILE_MEMORY ||
        function->prog_if != NVME_PCI_PROGRAMMING_INTERFACE) {
        return NVME_STATUS_BAD_PCI_CLASS;
    }
    if (pci_claim_device(function, &controller->claim.pci) !=
            PCI_RESOURCE_STATUS_OK) {
        return NVME_STATUS_CLAIM_FAILURE;
    }
    if (transition(&controller->claim, NVME_CONTROLLER_CLAIMED) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    if (pci_claim_map_bar(&controller->claim.pci, 0U,
            &controller->registers.mapping) != PCI_RESOURCE_STATUS_OK) {
        return NVME_STATUS_MAPPING_FAILURE;
    }
    result = validate_fixed_registers(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = decode_capabilities(mmio_read64(controller->mmio, NVME_REG_CAP),
        mmio_read32(controller->mmio, NVME_REG_VS),
        &controller->capabilities);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = validate_doorbells(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = disable_controller(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (transition(&controller->claim, NVME_CONTROLLER_DISABLED) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    result = prepare_dma(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = configure_interrupt(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = prepare_controller_ownership(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (transition(&controller->claim, NVME_CONTROLLER_PREPARED) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    result = program_and_enable_controller(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (transition(&controller->claim, NVME_CONTROLLER_RUNNING) !=
            NVME_STATUS_OK) {
        return NVME_STATUS_TRANSITION_INVALID;
    }
    result = identify_controller(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = validate_identify_controller(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = identify_active_namespaces(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = validate_active_namespace_list(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = prepare_namespace_identify_buffer(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    result = identify_namespace(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (controller->identify.namespace_data.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_DMA_OWNERSHIP;
    }
    controller->identify.namespace_state = NVME_DMA_CPU_OWNED;
    result = parse_namespace(controller->identify.namespace_data.cpu_address,
        &controller->namespace_data);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    return create_io_queues(controller);
}

enum nvme_status nvme_read_prove(struct nvme_read_proof *proof)
{
    struct nvme_runtime controller = {0};
    const struct pci_resource_state pci_before = pci_resource_get_state();
    const struct dma_state dma_before = dma_get_state();
    const struct interrupt_vector_state vectors_before =
        interrupt_vector_get_state();
    const struct msix_state msix_before = msix_get_state();
    const struct frame_allocator_stats frames_before =
        frame_allocator_get_stats();
    enum nvme_status result;
    enum nvme_status teardown_status;
    uint64_t read_interrupts_before = 0U;
    uint64_t read_interrupts_after = 0U;

    if (proof == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(proof, sizeof(*proof));
    result = initialize_runtime(&controller);
    if (result != NVME_STATUS_OK) {
        goto cleanup;
    }
    proof->controller_ready = true;
    proof->namespace_ready = true;
    read_interrupts_before = controller.interrupt_count;
    result = submit_read(&controller);
    read_interrupts_after = controller.interrupt_count;
    if (result != NVME_STATUS_OK) {
        goto cleanup;
    }
    if (read_interrupts_after != read_interrupts_before + 1U) {
        result = NVME_STATUS_INTERRUPT_COUNT;
        goto cleanup;
    }
    result = validate_read_data(&controller);
    if (result != NVME_STATUS_OK) {
        goto cleanup;
    }
    proof->block_bytes = controller.namespace_data.logical_block_bytes;
    proof->msix_completion_count =
        read_interrupts_after - read_interrupts_before;
    proof->ignored_completions = controller.ignored_completions;
    proof->contents_valid = true;
    proof->sentinel_valid = true;
    proof->changed_while_controller_owned =
        controller.read.changed_while_controller_owned;
    proof->ownership_complete = true;
    result = NVME_STATUS_OK;

cleanup:
    proof->ignored_completions = controller.ignored_completions;
    teardown_status = teardown_controller(&controller);
    if (teardown_status != NVME_STATUS_OK) {
        result = teardown_status;
    }
    if (!resource_state_matches(pci_before, dma_before, vectors_before,
            msix_before, frames_before)) {
        result = NVME_STATUS_TEARDOWN_FAILURE;
    }
    proof->teardown_complete = result == NVME_STATUS_OK;
    if (result == NVME_STATUS_OK) {
        proof->robustness_tests = NVME_CONTROLLED_ROBUSTNESS_TESTS;
        installed_proof = *proof;
    }
    return result;
}

struct nvme_read_proof nvme_get_read_proof(void)
{
    return installed_proof;
}

static bool filesystem_session_matches(
    const struct nvme_filesystem_read_session *session
)
{
    return session != NULL && filesystem_runtime.active &&
        session->generation != 0U &&
        session->generation == filesystem_runtime.generation &&
        session->generation ==
            filesystem_runtime.controller.claim.discovery.generation;
}

enum nvme_status nvme_filesystem_session_open(
    struct nvme_filesystem_read_session *session
)
{
    enum nvme_status result;
    enum nvme_status teardown_status;

    if (session == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (session->state != NVME_FILESYSTEM_SESSION_UNOPENED ||
        filesystem_runtime.active) {
        return session->state == NVME_FILESYSTEM_SESSION_RELEASED ?
            NVME_STATUS_TRANSITION_REVERSED :
            NVME_STATUS_TRANSITION_REPEATED;
    }
    zero_bytes(&filesystem_runtime, sizeof(filesystem_runtime));
    filesystem_runtime.pci_before = pci_resource_get_state();
    filesystem_runtime.dma_before = dma_get_state();
    filesystem_runtime.vectors_before = interrupt_vector_get_state();
    filesystem_runtime.msix_before = msix_get_state();
    filesystem_runtime.frames_before = frame_allocator_get_stats();
    filesystem_runtime.active = true;
    result = initialize_runtime(&filesystem_runtime.controller);
    if (result == NVME_STATUS_OK &&
        (filesystem_runtime.controller.namespace_data.logical_blocks !=
            FAT16_TOTAL_SECTORS ||
         filesystem_runtime.controller.namespace_data.logical_block_bytes !=
            NVME_BLOCK_BYTES)) {
        /* A controller with the v0.5 raw fixture is not the FAT16 fixture. */
        result = NVME_STATUS_ABSENT;
    }
    if (result != NVME_STATUS_OK) {
        teardown_status = teardown_controller(&filesystem_runtime.controller);
        if (teardown_status != NVME_STATUS_OK ||
            !resource_state_matches(filesystem_runtime.pci_before,
                filesystem_runtime.dma_before,
                filesystem_runtime.vectors_before,
                filesystem_runtime.msix_before,
                filesystem_runtime.frames_before)) {
            return teardown_status != NVME_STATUS_OK ? teardown_status :
                NVME_STATUS_TEARDOWN_FAILURE;
        }
        zero_bytes(&filesystem_runtime, sizeof(filesystem_runtime));
        return result;
    }
    filesystem_runtime.generation =
        filesystem_runtime.controller.claim.discovery.generation;
    zero_bytes(session, sizeof(*session));
    session->generation = filesystem_runtime.generation;
    session->namespace_blocks =
        filesystem_runtime.controller.namespace_data.logical_blocks;
    session->logical_block_bytes =
        filesystem_runtime.controller.namespace_data.logical_block_bytes;
    session->state = NVME_FILESYSTEM_SESSION_READY;
    session->guard_pages_clean = true;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_filesystem_session_read(
    struct nvme_filesystem_read_session *session,
    uint64_t lba,
    uint32_t ordinal
)
{
    struct nvme_runtime *controller;
    uint64_t interrupts_before;
    uint64_t interrupts_after;
    enum nvme_status result;

    if (session == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (!filesystem_session_matches(session)) {
        return NVME_STATUS_SESSION_INVALID;
    }
    if (session->state != NVME_FILESYSTEM_SESSION_READY &&
        session->state != NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED) {
        return session->state ==
            NVME_FILESYSTEM_SESSION_BLOCK_CONTROLLER_OWNED ?
            NVME_STATUS_BLOCK_NOT_CPU_OWNED :
            NVME_STATUS_TRANSITION_INVALID;
    }
    if (ordinal == 0U || ordinal > NVME_FILESYSTEM_READ_LIMIT ||
        ordinal != session->read_count + 1U) {
        return NVME_STATUS_READ_ORDINAL;
    }
    session->last_read_changed_while_controller_owned = false;
    controller = &filesystem_runtime.controller;
    result = prepare_guarded_read(controller);
    if (result != NVME_STATUS_OK) {
        return result;
    }
    session->state = NVME_FILESYSTEM_SESSION_BLOCK_CONTROLLER_OWNED;
    interrupts_before = controller->interrupt_count;
    result = submit_read_at(controller, lba, (uint16_t)ordinal);
    interrupts_after = controller->interrupt_count;
    if (result != NVME_STATUS_OK) {
        return result;
    }
    if (interrupts_after != interrupts_before + 1U) {
        return NVME_STATUS_INTERRUPT_COUNT;
    }
    result = validate_guarded_read(controller);
    if (result != NVME_STATUS_OK) {
        if (result == NVME_STATUS_SENTINEL_MISMATCH) {
            session->guard_pages_clean = false;
        }
        return result;
    }
    session->state = NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED;
    session->read_count = ordinal;
    session->last_ordinal = ordinal;
    session->msix_completion_count +=
        interrupts_after - interrupts_before;
    session->ignored_completions = controller->ignored_completions;
    session->last_read_changed_while_controller_owned =
        controller->read.changed_while_controller_owned;
    session->changed_while_controller_owned =
        session->changed_while_controller_owned ||
        session->last_read_changed_while_controller_owned;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_filesystem_session_view(
    const struct nvme_filesystem_read_session *session,
    uint32_t ordinal,
    const uint8_t **data,
    size_t *data_length
)
{
    const struct nvme_runtime *controller;

    if (data != NULL) {
        *data = NULL;
    }
    if (data_length != NULL) {
        *data_length = 0U;
    }
    if (session == NULL || data == NULL || data_length == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (!filesystem_session_matches(session)) {
        return NVME_STATUS_SESSION_INVALID;
    }
    controller = &filesystem_runtime.controller;
    if (session->state != NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED ||
        ordinal != session->last_ordinal || ordinal != session->read_count ||
        controller->read.state != NVME_DMA_CPU_OWNED ||
        controller->read.dma.owner != DMA_OWNER_CPU) {
        return NVME_STATUS_BLOCK_NOT_CPU_OWNED;
    }
    *data = (const uint8_t *)controller->read.dma.cpu_address +
        controller->read.data_offset;
    *data_length = (size_t)controller->read.data_length;
    return NVME_STATUS_OK;
}

enum nvme_status nvme_filesystem_session_close(
    struct nvme_filesystem_read_session *session
)
{
    enum nvme_status result;

    if (session == NULL) {
        return NVME_STATUS_NULL_ARGUMENT;
    }
    if (!filesystem_session_matches(session)) {
        return session->state == NVME_FILESYSTEM_SESSION_RELEASED ?
            NVME_STATUS_TRANSITION_REPEATED : NVME_STATUS_SESSION_INVALID;
    }
    if (session->state == NVME_FILESYSTEM_SESSION_STOPPING ||
        session->state == NVME_FILESYSTEM_SESSION_RELEASED) {
        return NVME_STATUS_TRANSITION_REPEATED;
    }
    session->state = NVME_FILESYSTEM_SESSION_STOPPING;
    result = teardown_controller(&filesystem_runtime.controller);
    if (result == NVME_STATUS_OK &&
        !resource_state_matches(filesystem_runtime.pci_before,
            filesystem_runtime.dma_before,
            filesystem_runtime.vectors_before,
            filesystem_runtime.msix_before,
            filesystem_runtime.frames_before)) {
        result = NVME_STATUS_TEARDOWN_FAILURE;
    }
    if (result != NVME_STATUS_OK) {
        return result;
    }
    filesystem_runtime.active = false;
    filesystem_runtime.generation = 0U;
    session->state = NVME_FILESYSTEM_SESSION_RELEASED;
    session->teardown_complete = true;
    zero_bytes(&filesystem_runtime, sizeof(filesystem_runtime));
    return NVME_STATUS_OK;
}

bool nvme_filesystem_session_resources_released(void)
{
    return !filesystem_runtime.active && filesystem_runtime.generation == 0U;
}

const char *nvme_status_string(enum nvme_status status)
{
    switch (status) {
    case NVME_STATUS_OK: return "ok";
    case NVME_STATUS_ABSENT: return "NVMe proof fixture is absent";
    case NVME_STATUS_NULL_ARGUMENT: return "null NVMe argument";
    case NVME_STATUS_MULTIPLE_CONTROLLERS:
        return "more than one NVMe controller is unsupported";
    case NVME_STATUS_BAD_PCI_CLASS:
        return "PCI function is not the NVMe class tuple";
    case NVME_STATUS_CLAIM_FAILURE: return "NVMe PCI claim failed";
    case NVME_STATUS_MAPPING_FAILURE: return "NVMe BAR mapping failed";
    case NVME_STATUS_REGISTER_OUTSIDE_BAR:
        return "NVMe register or doorbell lies outside its BAR";
    case NVME_STATUS_REGISTER_OVERFLOW:
        return "NVMe register range overflowed";
    case NVME_STATUS_REGISTER_ALIGNMENT:
        return "NVMe register is misaligned";
    case NVME_STATUS_CAP_QUEUE_GEOMETRY:
        return "NVMe CAP queue geometry is invalid";
    case NVME_STATUS_CAP_DOORBELL_GEOMETRY:
        return "NVMe CAP doorbell geometry is invalid";
    case NVME_STATUS_UNSUPPORTED_COMMAND_SET:
        return "NVMe controller lacks the NVM command set";
    case NVME_STATUS_UNSUPPORTED_PAGE_SIZE:
        return "NVMe controller cannot use Sapote's page size";
    case NVME_STATUS_UNSUPPORTED_VERSION:
        return "NVMe controller version is unsupported";
    case NVME_STATUS_DISABLE_TIMEOUT:
        return "NVMe controller-disable wait timed out";
    case NVME_STATUS_ENABLE_TIMEOUT:
        return "NVMe controller-enable wait timed out";
    case NVME_STATUS_CONTROLLER_FATAL:
        return "NVMe controller fatal status is asserted";
    case NVME_STATUS_ADMIN_QUEUE_INVALID:
        return "NVMe Admin queue geometry is invalid";
    case NVME_STATUS_IO_QUEUE_INVALID:
        return "NVMe I/O queue geometry is invalid";
    case NVME_STATUS_DMA_ALLOCATION: return "NVMe DMA allocation failed";
    case NVME_STATUS_DMA_LAYOUT: return "NVMe DMA layout is invalid";
    case NVME_STATUS_DMA_OWNERSHIP:
        return "NVMe DMA ownership was violated";
    case NVME_STATUS_QUEUE_PHASE: return "NVMe queue phase is invalid";
    case NVME_STATUS_QUEUE_OWNERSHIP:
        return "NVMe queue ownership is invalid";
    case NVME_STATUS_QUEUE_FULL:
        return "NVMe queue already has its one outstanding command";
    case NVME_STATUS_COMMAND_ID_DUPLICATE:
        return "NVMe command identifier is duplicated";
    case NVME_STATUS_COMMAND_ID_RANGE:
        return "NVMe command identifier is out of range";
    case NVME_STATUS_PRP_INVALID:
        return "NVMe PRP1 is outside its one-page DMA allocation";
    case NVME_STATUS_INTERRUPT_NOT_READY:
        return "NVMe interrupt prerequisites are incomplete";
    case NVME_STATUS_MSIX_FAILURE: return "NVMe MSI-X binding failed";
    case NVME_STATUS_MSIX_ROLLBACK_FAILURE:
        return "NVMe MSI-X rollback leaked state";
    case NVME_STATUS_BUS_MASTER_PREMATURE:
        return "NVMe bus mastering preceded DMA preparation";
    case NVME_STATUS_BUS_MASTER_FAILURE:
        return "NVMe bus mastering failed";
    case NVME_STATUS_DOORBELL_PREMATURE:
        return "NVMe doorbell preceded controller ownership";
    case NVME_STATUS_COMMAND_TIMEOUT: return "NVMe command timed out";
    case NVME_STATUS_COMPLETION_PHASE:
        return "NVMe completion phase did not match";
    case NVME_STATUS_COMPLETION_COMMAND_ID:
        return "NVMe completion command identifier did not match";
    case NVME_STATUS_COMPLETION_QUEUE_ID:
        return "NVMe completion submission queue did not match";
    case NVME_STATUS_COMPLETION_STATUS:
        return "NVMe completion status was not successful";
    case NVME_STATUS_COMPLETION_LENGTH:
        return "NVMe completion returned an invalid queue position";
    case NVME_STATUS_COMPLETION_OWNERSHIP:
        return "NVMe completion did not return DMA ownership";
    case NVME_STATUS_IDENTIFY_CONTROLLER:
        return "Identify Controller data is malformed or unsupported";
    case NVME_STATUS_IDENTIFY_NAMESPACE:
        return "Identify Namespace data is malformed";
    case NVME_STATUS_NAMESPACE_ABSENT: return "NVMe namespace is absent";
    case NVME_STATUS_NAMESPACE_INACTIVE: return "NVMe namespace is inactive";
    case NVME_STATUS_MULTIPLE_NAMESPACES:
        return "more than one NVMe namespace is unsupported";
    case NVME_STATUS_LBA_FORMAT: return "NVMe LBA format is unsupported";
    case NVME_STATUS_METADATA: return "NVMe metadata is unsupported";
    case NVME_STATUS_PROTECTION_INFORMATION:
        return "NVMe protection information is unsupported";
    case NVME_STATUS_BLOCK_ZERO_LENGTH:
        return "NVMe block request has zero length";
    case NVME_STATUS_BLOCK_RANGE:
        return "NVMe block request is outside the namespace";
    case NVME_STATUS_BLOCK_OVERFLOW:
        return "NVMe block request overflowed";
    case NVME_STATUS_CONTENT_MISMATCH:
        return "NVMe fixture block contents did not match";
    case NVME_STATUS_SENTINEL_MISMATCH:
        return "NVMe changed bytes outside its PRP1 block";
    case NVME_STATUS_INTERRUPT_COUNT:
        return "NVMe MSI-X count did not advance exactly once";
    case NVME_STATUS_SESSION_INVALID:
        return "NVMe filesystem session identity is invalid";
    case NVME_STATUS_READ_ORDINAL:
        return "NVMe filesystem read ordinal is invalid";
    case NVME_STATUS_BLOCK_NOT_CPU_OWNED:
        return "NVMe filesystem block is not CPU-owned";
    case NVME_STATUS_TRANSITION_REPEATED:
        return "NVMe lifecycle transition was repeated";
    case NVME_STATUS_TRANSITION_REVERSED:
        return "NVMe lifecycle transition was reversed";
    case NVME_STATUS_TRANSITION_INVALID:
        return "NVMe lifecycle transition is invalid";
    case NVME_STATUS_TEARDOWN_RACE:
        return "NVMe teardown race observed freed state";
    case NVME_STATUS_TEARDOWN_FAILURE:
        return "NVMe teardown leaked or failed";
    default: return "unknown NVMe status";
    }
}
