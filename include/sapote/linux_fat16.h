/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_LINUX_FAT16_H
#define SAPOTE_LINUX_FAT16_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/fat16.h>

#define LINUX_FAT16_FILE_BYTES 33584U
#define LINUX_FAT16_FILE_CLUSTERS 9U
#define LINUX_FAT16_MAX_CLUSTERS 512U
#define LINUX_FAT16_ROBUSTNESS_CONTROLS 12U

enum linux_fat16_status {
    LINUX_FAT16_STATUS_OK = 0,
    LINUX_FAT16_STATUS_NULL_ARGUMENT,
    LINUX_FAT16_STATUS_TRUNCATED,
    LINUX_FAT16_STATUS_ROOT_END_MISSING,
    LINUX_FAT16_STATUS_TARGET_ABSENT,
    LINUX_FAT16_STATUS_TARGET_DUPLICATE,
    LINUX_FAT16_STATUS_UNSUPPORTED_ENTRY,
    LINUX_FAT16_STATUS_NAME_MALFORMED,
    LINUX_FAT16_STATUS_CLUSTER_RANGE,
    LINUX_FAT16_STATUS_FILE_SIZE,
    LINUX_FAT16_STATUS_CHAIN_CAPACITY,
    LINUX_FAT16_STATUS_FAT_RESERVED,
    LINUX_FAT16_STATUS_FAT_FREE,
    LINUX_FAT16_STATUS_FAT_BAD,
    LINUX_FAT16_STATUS_PREMATURE_EOC,
    LINUX_FAT16_STATUS_OVERLONG_CHAIN,
    LINUX_FAT16_STATUS_CHAIN_CYCLE,
    LINUX_FAT16_STATUS_CLUSTER_TRANSLATION,
    LINUX_FAT16_STATUS_PAYLOAD_LENGTH,
    LINUX_FAT16_STATUS_PAYLOAD_DIGEST,
    LINUX_FAT16_STATUS_COUNT
};

struct linux_fat16_chain {
    uint16_t clusters[LINUX_FAT16_MAX_CLUSTERS];
    uint64_t lbas[LINUX_FAT16_MAX_CLUSTERS];
    uint32_t cluster_count;
    uint32_t file_bytes;
    uint32_t final_cluster_bytes;
    uint32_t valid;
};

struct linux_fat16_payload {
    uint8_t sha256[FAT16_SHA256_BYTES];
    uint32_t byte_count;
    uint32_t deterministic;
};

enum linux_fat16_status sapote_linux_fat16_find_root(
    const uint8_t *block,
    size_t block_len,
    const struct fat16_geometry *geometry,
    const struct fat16_root_query *query,
    uint32_t destination_bytes,
    struct fat16_root_entry *out
);
enum linux_fat16_status sapote_linux_fat16_make_query(
    struct fat16_root_query *out
);
enum linux_fat16_status sapote_linux_fat16_build_chain(
    const uint8_t *fat,
    size_t fat_len,
    const struct fat16_geometry *geometry,
    const struct fat16_root_entry *entry,
    struct linux_fat16_chain *out
);
enum linux_fat16_status sapote_linux_fat16_validate_payload(
    const uint8_t *data,
    size_t data_len,
    struct linux_fat16_payload *out
);
uint32_t sapote_linux_fat16_self_test(void);

#endif
