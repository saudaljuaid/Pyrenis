/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_FILESYSTEM_H
#define SAPOTE_FILESYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/fat16.h>
#include <sapote/linux_fat16.h>
#include <sapote/nvme.h>

#define FILESYSTEM_INTEGRATION_CONTROLS 26U
#define FILESYSTEM_CONTROLLED_ROBUSTNESS_TESTS 28U

enum filesystem_state {
    FILESYSTEM_UNOPENED = 0,
    FILESYSTEM_SESSION_READY,
    FILESYSTEM_BLOCK_CONTROLLER_OWNED,
    FILESYSTEM_BLOCK_CPU_OWNED,
    FILESYSTEM_VOLUME_VALIDATED,
    FILESYSTEM_FILE_LOCATED,
    FILESYSTEM_FILE_READ,
    FILESYSTEM_STOPPING,
    FILESYSTEM_RELEASED,
    FILESYSTEM_STATE_COUNT
};

enum filesystem_status {
    FILESYSTEM_STATUS_OK = 0,
    FILESYSTEM_STATUS_ABSENT,
    FILESYSTEM_STATUS_NULL_ARGUMENT,
    FILESYSTEM_STATUS_PARSER_SELF_TEST,
    FILESYSTEM_STATUS_PARSER_FAILURE,
    FILESYSTEM_STATUS_NVME_FAILURE,
    FILESYSTEM_STATUS_SESSION_STATE,
    FILESYSTEM_STATUS_BLOCK_RESULT,
    FILESYSTEM_STATUS_OWNERSHIP,
    FILESYSTEM_STATUS_CONTENT,
    FILESYSTEM_STATUS_READ_COUNT,
    FILESYSTEM_STATUS_INTERRUPT_COUNT,
    FILESYSTEM_STATUS_TRANSITION_REPEATED,
    FILESYSTEM_STATUS_TRANSITION_REVERSED,
    FILESYSTEM_STATUS_TRANSITION_INVALID,
    FILESYSTEM_STATUS_TEARDOWN_FAILURE,
    FILESYSTEM_STATUS_PRIVATE_BUSY,
    FILESYSTEM_STATUS_PRIVATE_BAD_TOKEN,
    FILESYSTEM_STATUS_PRIVATE_BAD_BUFFER,
    FILESYSTEM_STATUS_LINUX_CHAIN,
    FILESYSTEM_STATUS_LINUX_PAYLOAD,
    FILESYSTEM_STATUS_COUNT
};

struct filesystem_candidate_volume {
    uint64_t namespace_blocks;
    uint32_t logical_block_bytes;
    bool active;
};

struct filesystem_validated_volume {
    struct filesystem_candidate_volume candidate;
    struct fat16_geometry geometry;
    enum filesystem_state state;
    bool active;
};

struct filesystem_cpu_file_content {
    uint8_t bytes[FAT16_FILE_BYTES];
    struct fat16_payload payload;
    enum filesystem_state state;
    bool cpu_owned;
};

struct filesystem_file_proof {
    uint8_t canonical_name[FAT16_CANONICAL_NAME_BYTES];
    uint8_t payload_sha256[FAT16_SHA256_BYTES];
    uint32_t file_bytes;
    uint32_t read_count;
    uint64_t msix_completion_count;
    size_t ignored_completions;
    size_t robustness_tests;
    bool fat16_ready;
    bool file_located;
    bool contents_valid;
    bool sentinel_valid;
    bool changed_while_controller_owned;
    bool ownership_complete;
    bool teardown_complete;
};

struct filesystem_private_file {
    uint64_t generation;
    uint64_t msix_completion_count;
    uint32_t file_bytes;
    uint32_t read_count;
    bool cpu_owned;
    bool active;
};

struct filesystem_linux_file {
    uint64_t generation;
    uint64_t msix_completion_count;
    uint32_t file_bytes;
    uint32_t read_count;
    uint32_t cluster_count;
    bool cpu_owned;
    bool active;
};

bool filesystem_foundation_self_test(size_t *completed_tests);
enum filesystem_status filesystem_file_prove(
    struct filesystem_file_proof *proof
);
struct filesystem_file_proof filesystem_get_file_proof(void);
enum filesystem_status filesystem_private_read_open(
    struct filesystem_private_file *file,
    uint8_t *destination,
    size_t destination_bytes
);
enum filesystem_status filesystem_private_read_close(
    struct filesystem_private_file *file
);
enum filesystem_status filesystem_linux_read_open(
    struct filesystem_linux_file *file,
    uint8_t *destination,
    size_t destination_bytes
);
enum filesystem_status filesystem_linux_read_close(
    struct filesystem_linux_file *file
);
bool filesystem_resources_released(void);
const char *filesystem_status_string(enum filesystem_status status);

#endif
