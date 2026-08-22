/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_NVME_H
#define SAPOTE_NVME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/dma.h>
#include <sapote/msix.h>
#include <sapote/pci.h>
#include <sapote/pci_resource.h>

#define NVME_PCI_SUBCLASS_NON_VOLATILE_MEMORY \
    PCI_SUBCLASS_NON_VOLATILE_MEMORY
#define NVME_PCI_PROGRAMMING_INTERFACE PCI_PROG_IF_NVME
#define NVME_QUEUE_DEPTH 2U
#define NVME_QUEUE_IDENTIFIER_ADMIN UINT16_C(0)
#define NVME_QUEUE_IDENTIFIER_IO UINT16_C(1)
#define NVME_NAMESPACE_IDENTIFIER UINT32_C(1)
#define NVME_FIXTURE_LBA UINT64_C(8)
#define NVME_BLOCK_BYTES 4096U
#define NVME_FILESYSTEM_READ_LIMIT 12U
#define NVME_FOUNDATION_ROBUSTNESS_TESTS 20U
#define NVME_CONTROLLED_ROBUSTNESS_TESTS 22U

enum nvme_controller_state {
    NVME_CONTROLLER_UNINITIALIZED = 0,
    NVME_CONTROLLER_DISCOVERED,
    NVME_CONTROLLER_CLAIMED,
    NVME_CONTROLLER_DISABLED,
    NVME_CONTROLLER_PREPARED,
    NVME_CONTROLLER_RUNNING,
    NVME_CONTROLLER_STOPPING,
    NVME_CONTROLLER_RELEASED,
    NVME_CONTROLLER_STATE_COUNT
};

enum nvme_dma_object_state {
    NVME_DMA_UNALLOCATED = 0,
    NVME_DMA_CPU_OWNED,
    NVME_DMA_CONTROLLER_OWNED,
    NVME_DMA_RECLAIMED,
    NVME_DMA_OBJECT_STATE_COUNT
};

enum nvme_queue_kind {
    NVME_QUEUE_ADMIN = 0,
    NVME_QUEUE_IO,
    NVME_QUEUE_KIND_COUNT
};

enum nvme_filesystem_session_state {
    NVME_FILESYSTEM_SESSION_UNOPENED = 0,
    NVME_FILESYSTEM_SESSION_READY,
    NVME_FILESYSTEM_SESSION_BLOCK_CONTROLLER_OWNED,
    NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED,
    NVME_FILESYSTEM_SESSION_STOPPING,
    NVME_FILESYSTEM_SESSION_RELEASED,
    NVME_FILESYSTEM_SESSION_STATE_COUNT
};

enum nvme_status {
    NVME_STATUS_OK = 0,
    NVME_STATUS_ABSENT,
    NVME_STATUS_NULL_ARGUMENT,
    NVME_STATUS_MULTIPLE_CONTROLLERS,
    NVME_STATUS_BAD_PCI_CLASS,
    NVME_STATUS_CLAIM_FAILURE,
    NVME_STATUS_MAPPING_FAILURE,
    NVME_STATUS_REGISTER_OUTSIDE_BAR,
    NVME_STATUS_REGISTER_OVERFLOW,
    NVME_STATUS_REGISTER_ALIGNMENT,
    NVME_STATUS_CAP_QUEUE_GEOMETRY,
    NVME_STATUS_CAP_DOORBELL_GEOMETRY,
    NVME_STATUS_UNSUPPORTED_COMMAND_SET,
    NVME_STATUS_UNSUPPORTED_PAGE_SIZE,
    NVME_STATUS_UNSUPPORTED_VERSION,
    NVME_STATUS_DISABLE_TIMEOUT,
    NVME_STATUS_ENABLE_TIMEOUT,
    NVME_STATUS_CONTROLLER_FATAL,
    NVME_STATUS_ADMIN_QUEUE_INVALID,
    NVME_STATUS_IO_QUEUE_INVALID,
    NVME_STATUS_DMA_ALLOCATION,
    NVME_STATUS_DMA_LAYOUT,
    NVME_STATUS_DMA_OWNERSHIP,
    NVME_STATUS_QUEUE_PHASE,
    NVME_STATUS_QUEUE_OWNERSHIP,
    NVME_STATUS_QUEUE_FULL,
    NVME_STATUS_COMMAND_ID_DUPLICATE,
    NVME_STATUS_COMMAND_ID_RANGE,
    NVME_STATUS_PRP_INVALID,
    NVME_STATUS_INTERRUPT_NOT_READY,
    NVME_STATUS_MSIX_FAILURE,
    NVME_STATUS_MSIX_ROLLBACK_FAILURE,
    NVME_STATUS_BUS_MASTER_PREMATURE,
    NVME_STATUS_BUS_MASTER_FAILURE,
    NVME_STATUS_DOORBELL_PREMATURE,
    NVME_STATUS_COMMAND_TIMEOUT,
    NVME_STATUS_COMPLETION_PHASE,
    NVME_STATUS_COMPLETION_COMMAND_ID,
    NVME_STATUS_COMPLETION_QUEUE_ID,
    NVME_STATUS_COMPLETION_STATUS,
    NVME_STATUS_COMPLETION_LENGTH,
    NVME_STATUS_COMPLETION_OWNERSHIP,
    NVME_STATUS_IDENTIFY_CONTROLLER,
    NVME_STATUS_IDENTIFY_NAMESPACE,
    NVME_STATUS_NAMESPACE_ABSENT,
    NVME_STATUS_NAMESPACE_INACTIVE,
    NVME_STATUS_MULTIPLE_NAMESPACES,
    NVME_STATUS_LBA_FORMAT,
    NVME_STATUS_METADATA,
    NVME_STATUS_PROTECTION_INFORMATION,
    NVME_STATUS_BLOCK_ZERO_LENGTH,
    NVME_STATUS_BLOCK_RANGE,
    NVME_STATUS_BLOCK_OVERFLOW,
    NVME_STATUS_CONTENT_MISMATCH,
    NVME_STATUS_SENTINEL_MISMATCH,
    NVME_STATUS_INTERRUPT_COUNT,
    NVME_STATUS_SESSION_INVALID,
    NVME_STATUS_READ_ORDINAL,
    NVME_STATUS_BLOCK_NOT_CPU_OWNED,
    NVME_STATUS_TRANSITION_REPEATED,
    NVME_STATUS_TRANSITION_REVERSED,
    NVME_STATUS_TRANSITION_INVALID,
    NVME_STATUS_TEARDOWN_RACE,
    NVME_STATUS_TEARDOWN_FAILURE,
    NVME_STATUS_COUNT
};

struct nvme_register_span {
    uint64_t offset;
    uint64_t length;
    bool valid;
};

struct nvme_register_regions {
    struct pci_mmio_region *mapping;
    struct nvme_register_span controller;
    struct nvme_register_span admin;
    struct nvme_register_span admin_submission_doorbell;
    struct nvme_register_span admin_completion_doorbell;
    struct nvme_register_span io_submission_doorbell;
    struct nvme_register_span io_completion_doorbell;
    uint64_t bar_size;
};

struct nvme_controller_capabilities {
    uint64_t raw;
    uint32_t version;
    uint32_t doorbell_stride;
    uint64_t ready_timeout_ns;
    uint32_t maximum_queue_entries;
    uint8_t minimum_page_shift;
    uint8_t maximum_page_shift;
    bool queues_contiguous_required;
    bool nvm_command_set;
};

struct nvme_controller_discovery {
    struct pci_address address;
    uint64_t generation;
    bool active;
};

struct nvme_controller_claim {
    struct nvme_controller_discovery discovery;
    struct pci_device_claim pci;
    enum nvme_controller_state state;
};

struct nvme_command_identifier {
    uint16_t value;
    bool active;
};

struct nvme_submission_entry {
    uint32_t dword[16];
};

struct nvme_completion_entry {
    uint32_t dword[4];
};

struct nvme_queue_pair {
    struct dma_allocation submission;
    struct dma_allocation completion;
    enum nvme_dma_object_state submission_state;
    enum nvme_dma_object_state completion_state;
    struct nvme_command_identifier outstanding;
    enum nvme_queue_kind kind;
    uint16_t identifier;
    uint16_t depth;
    uint16_t submission_tail;
    uint16_t completion_head;
    uint8_t completion_phase;
    bool active;
};

struct nvme_identify_buffers {
    struct dma_allocation controller;
    struct dma_allocation namespace_data;
    enum nvme_dma_object_state controller_state;
    enum nvme_dma_object_state namespace_state;
};

struct nvme_namespace_selection {
    uint32_t identifier;
    uint64_t logical_blocks;
    uint32_t logical_block_bytes;
    uint8_t format_index;
    bool active;
};

struct nvme_logical_block_range {
    uint64_t first;
    uint64_t count;
};

struct nvme_prp_read_buffer {
    struct dma_allocation dma;
    enum nvme_dma_object_state state;
    uint64_t data_offset;
    uint64_t data_length;
    bool changed_while_controller_owned;
};

struct nvme_interrupt_binding {
    struct msix_binding msix;
    uint8_t vector;
    bool handler_ready;
    bool queues_ready;
    bool delivery_enabled;
    bool active;
};

struct nvme_read_proof {
    uint32_t block_bytes;
    uint64_t msix_completion_count;
    size_t ignored_completions;
    size_t robustness_tests;
    bool controller_ready;
    bool namespace_ready;
    bool contents_valid;
    bool sentinel_valid;
    bool changed_while_controller_owned;
    bool ownership_complete;
    bool teardown_complete;
};

/*
 * Private to src/kernel/filesystem.c. This token owns no exposed DMA pointer;
 * nvme_filesystem_session_view returns a CPU-owned view only for the current
 * synchronous block and Makefile rejects callers outside that one consumer.
 */
struct nvme_filesystem_read_session {
    uint64_t generation;
    uint64_t namespace_blocks;
    uint64_t msix_completion_count;
    size_t ignored_completions;
    uint32_t logical_block_bytes;
    uint32_t read_count;
    uint32_t last_ordinal;
    enum nvme_filesystem_session_state state;
    bool guard_pages_clean;
    bool last_read_changed_while_controller_owned;
    bool changed_while_controller_owned;
    bool teardown_complete;
};

bool nvme_foundation_self_test(size_t *completed_tests);
enum nvme_status nvme_read_prove(struct nvme_read_proof *proof);
struct nvme_read_proof nvme_get_read_proof(void);
enum nvme_status nvme_filesystem_session_open(
    struct nvme_filesystem_read_session *session
);
enum nvme_status nvme_filesystem_session_read(
    struct nvme_filesystem_read_session *session,
    uint64_t lba,
    uint32_t ordinal
);
enum nvme_status nvme_filesystem_session_view(
    const struct nvme_filesystem_read_session *session,
    uint32_t ordinal,
    const uint8_t **data,
    size_t *data_length
);
enum nvme_status nvme_filesystem_session_close(
    struct nvme_filesystem_read_session *session
);
bool nvme_filesystem_session_resources_released(void);
const char *nvme_status_string(enum nvme_status status);

#endif
