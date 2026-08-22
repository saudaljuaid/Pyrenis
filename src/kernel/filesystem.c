/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * One read-only FAT16 superfloppy and one canonical root file. Rust parses
 * every filesystem-controlled byte; this C half owns the NVMe/DMA lifecycle,
 * checked stage transitions, immediate parser calls and final bounded copy.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/filesystem.h>

struct filesystem_block_result {
    uint32_t ordinal;
    uint32_t byte_count;
    uint64_t msix_delta;
    bool completion_identity;
    bool guard_clean;
    bool changed_while_controller_owned;
    bool cpu_owned;
};

static struct filesystem_file_proof installed_proof;
static bool filesystem_proof_active;

struct filesystem_private_read_runtime {
    struct nvme_filesystem_read_session session;
    enum filesystem_state state;
    uint64_t generation;
    uint8_t fat_block[FAT16_BLOCK_BYTES];
    uint32_t kind;
    bool owned;
};

enum private_read_kind {
    PRIVATE_READ_NONE = 0,
    PRIVATE_READ_PROCESS,
    PRIVATE_READ_LINUX
};

static struct filesystem_private_read_runtime private_read_runtime;
static uint64_t next_private_read_generation = UINT64_C(1);

_Static_assert(sizeof(struct fat16_geometry) == 88U,
    "Rust/C FAT16 geometry ABI changed");
_Static_assert(sizeof(struct fat16_root_query) == 11U,
    "Rust/C FAT16 query ABI changed");
_Static_assert(sizeof(struct fat16_root_entry) == 20U,
    "Rust/C FAT16 root entry ABI changed");
_Static_assert(sizeof(struct fat16_fat_state) == 8U,
    "Rust/C FAT16 state ABI changed");
_Static_assert(sizeof(struct fat16_extent) == 24U,
    "Rust/C FAT16 extent ABI changed");
_Static_assert(sizeof(struct fat16_payload) == 40U,
    "Rust/C FAT16 payload ABI changed");
_Static_assert(FAT16_STATUS_COUNT == 32,
    "Rust/C FAT16 status ABI changed");
_Static_assert(sizeof(struct linux_fat16_chain) == 5136U,
    "Rust/C Linux FAT16 chain ABI changed");
_Static_assert(sizeof(struct linux_fat16_payload) == 40U,
    "Rust/C Linux FAT16 payload ABI changed");
_Static_assert(LINUX_FAT16_STATUS_COUNT == 20,
    "Rust/C Linux FAT16 status ABI changed");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool all_zero(const void *pointer, size_t length)
{
    const uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool equal_bytes(
    const uint8_t *left,
    const uint8_t *right,
    size_t length
)
{
    for (size_t index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

enum fat16_test_offset {
    FAT16_TEST_BPS = 11,
    FAT16_TEST_SPC = 13,
    FAT16_TEST_RESERVED = 14,
    FAT16_TEST_FAT_COUNT = 16,
    FAT16_TEST_ROOT_ENTRIES = 17,
    FAT16_TEST_TOTAL_16 = 19,
    FAT16_TEST_MEDIA = 21,
    FAT16_TEST_FAT_16 = 22,
    FAT16_TEST_HIDDEN = 28,
    FAT16_TEST_TOTAL_32 = 32,
    FAT16_TEST_EBPB_RESERVED = 37,
    FAT16_TEST_EBPB_SIGNATURE = 38,
    FAT16_TEST_BOOT_SIGNATURE = 510
};

static const uint8_t fat16_test_name[FAT16_CANONICAL_NAME_BYTES] = {
    'S', 'A', 'P', 'O', 'T', 'E', ' ', ' ', 'B', 'I', 'N'
};

static const uint8_t fat16_test_sha256[FAT16_SHA256_BYTES] = {
    UINT8_C(0xD3), UINT8_C(0x99), UINT8_C(0xF0), UINT8_C(0x65),
    UINT8_C(0xC9), UINT8_C(0xF2), UINT8_C(0x1E), UINT8_C(0x2F),
    UINT8_C(0xD5), UINT8_C(0x1E), UINT8_C(0x2A), UINT8_C(0xEA),
    UINT8_C(0xDB), UINT8_C(0x77), UINT8_C(0x68), UINT8_C(0xEA),
    UINT8_C(0xB7), UINT8_C(0xE6), UINT8_C(0xE4), UINT8_C(0x5E),
    UINT8_C(0x51), UINT8_C(0x50), UINT8_C(0xF3), UINT8_C(0x12),
    UINT8_C(0x27), UINT8_C(0xC9), UINT8_C(0x71), UINT8_C(0x19),
    UINT8_C(0x34), UINT8_C(0xA4), UINT8_C(0xD1), UINT8_C(0xD3)
};

static void fat16_test_put_u16(uint8_t *block, size_t offset, uint16_t value)
{
    block[offset] = (uint8_t)value;
    block[offset + 1U] = (uint8_t)(value >> 8U);
}

static void fat16_test_put_u32(uint8_t *block, size_t offset, uint32_t value)
{
    block[offset] = (uint8_t)value;
    block[offset + 1U] = (uint8_t)(value >> 8U);
    block[offset + 2U] = (uint8_t)(value >> 16U);
    block[offset + 3U] = (uint8_t)(value >> 24U);
}

static void fat16_test_make_bpb(uint8_t *block)
{
    zero_bytes(block, FAT16_BLOCK_BYTES);
    block[0] = UINT8_C(0xEB);
    block[1] = UINT8_C(0x3C);
    block[2] = UINT8_C(0x90);
    block[3] = 'S';
    block[4] = 'A';
    block[5] = 'P';
    block[6] = 'O';
    block[7] = 'T';
    block[8] = 'E';
    block[9] = ' ';
    block[10] = ' ';
    fat16_test_put_u16(block, FAT16_TEST_BPS, FAT16_BLOCK_BYTES);
    block[FAT16_TEST_SPC] = 1U;
    fat16_test_put_u16(block, FAT16_TEST_RESERVED, 1U);
    block[FAT16_TEST_FAT_COUNT] = 1U;
    fat16_test_put_u16(block, FAT16_TEST_ROOT_ENTRIES,
        FAT16_ROOT_ENTRIES);
    fat16_test_put_u16(block, FAT16_TEST_TOTAL_16,
        (uint16_t)FAT16_TOTAL_SECTORS);
    block[FAT16_TEST_MEDIA] = UINT8_C(0xF8);
    fat16_test_put_u16(block, FAT16_TEST_FAT_16, 2U);
    block[FAT16_TEST_EBPB_RESERVED] = 0U;
    block[FAT16_TEST_EBPB_SIGNATURE] = UINT8_C(0x29);
    block[FAT16_TEST_BOOT_SIGNATURE] = UINT8_C(0x55);
    block[FAT16_TEST_BOOT_SIGNATURE + 1U] = UINT8_C(0xAA);
}

static void fat16_test_make_root(uint8_t *block)
{
    zero_bytes(block, FAT16_BLOCK_BYTES);
    for (size_t index = 0U; index < sizeof(fat16_test_name); ++index) {
        block[index] = fat16_test_name[index];
    }
    block[11] = UINT8_C(0x20);
    fat16_test_put_u16(block, 26U, FAT16_FILE_CLUSTER);
    fat16_test_put_u32(block, 28U, FAT16_FILE_BYTES);
}

static void fat16_test_make_fat(uint8_t *block)
{
    zero_bytes(block, FAT16_BLOCK_BYTES);
    fat16_test_put_u16(block, 0U, UINT16_C(0xFFF8));
    fat16_test_put_u16(block, 2U, UINT16_C(0xFFFF));
    fat16_test_put_u16(block, FAT16_FILE_CLUSTER * 2U,
        UINT16_C(0xFFFF));
}

static bool fat16_parse_bpb_status(
    const uint8_t *block,
    size_t length,
    uint64_t namespace_blocks,
    uint32_t namespace_block_bytes,
    enum fat16_status expected
)
{
    struct fat16_geometry output;

    return sapote_fat16_parse_bpb(block, length, namespace_blocks,
        namespace_block_bytes, &output) == expected;
}

static bool fat16_find_root_status(
    const uint8_t *block,
    const struct fat16_geometry *geometry,
    const struct fat16_root_query *query,
    uint32_t destination_bytes,
    enum fat16_status expected
)
{
    struct fat16_root_entry output;

    return sapote_fat16_find_root(block, FAT16_BLOCK_BYTES, geometry,
        query, destination_bytes, &output) == expected;
}

static bool fat16_parse_fat_status(
    const uint8_t *block,
    const struct fat16_geometry *geometry,
    enum fat16_status expected
)
{
    struct fat16_fat_state output;

    return sapote_fat16_parse_fat(block, FAT16_BLOCK_BYTES, geometry,
        &output) == expected;
}

static bool control_rust_parser(void)
{
    uint8_t block[FAT16_BLOCK_BYTES];
    uint8_t query_bytes[FAT16_CANONICAL_NAME_BYTES];
    uint8_t payload_bytes[FAT16_FILE_BYTES];
    struct fat16_geometry geometry;
    struct fat16_geometry corrupt_geometry;
    struct fat16_root_query query;
    struct fat16_root_entry entry;
    struct fat16_fat_state fat;
    struct fat16_extent extent;
    struct fat16_payload payload;
    const uint16_t invalid_fat_values[] = {
        0U, 1U, UINT16_C(0xFFF7), UINT16_C(0xFFF0),
        UINT16_C(0xFFF6), UINT16_C(5000)
    };

    fat16_test_make_bpb(block);
    if (sapote_fat16_parse_bpb(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, &geometry) != FAT16_STATUS_OK ||
        geometry.cluster_count != UINT64_C(4092) ||
        geometry.first_data_sector != UINT64_C(4)) {
        return false;
    }

    /* 1: both signatures and the bounded EBPB reserved byte. */
    block[FAT16_TEST_BOOT_SIGNATURE] = 0U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_BOOT_SIGNATURE)) {
        return false;
    }
    fat16_test_make_bpb(block);
    block[FAT16_TEST_EBPB_SIGNATURE] = 0U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_BOOT_SIGNATURE)) {
        return false;
    }
    fat16_test_make_bpb(block);
    block[FAT16_TEST_EBPB_RESERVED] = 1U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_BOOT_SIGNATURE)) {
        return false;
    }

    /* 2: truncation, namespace mismatch and unsupported sector size. */
    fat16_test_make_bpb(block);
    if (!fat16_parse_bpb_status(block, 511U, FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_TRUNCATED) ||
        !fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            512U, FAT16_STATUS_BYTES_PER_SECTOR)) {
        return false;
    }
    fat16_test_put_u16(block, FAT16_TEST_BPS, 512U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_BYTES_PER_SECTOR)) {
        return false;
    }

    /* 3: the subset has exactly one sector per cluster. */
    fat16_test_make_bpb(block);
    block[FAT16_TEST_SPC] = 2U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_SECTORS_PER_CLUSTER)) {
        return false;
    }
    fat16_test_make_bpb(block);
    block[FAT16_TEST_SPC] = 0U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_SECTORS_PER_CLUSTER)) {
        return false;
    }

    /* 4: one reserved sector and one FAT are mandatory. */
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_RESERVED, 0U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_RESERVED_COUNT)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_RESERVED, 2U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_RESERVED_COUNT)) {
        return false;
    }
    fat16_test_make_bpb(block);
    block[FAT16_TEST_FAT_COUNT] = 0U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_FAT_COUNT)) {
        return false;
    }
    fat16_test_make_bpb(block);
    block[FAT16_TEST_FAT_COUNT] = 2U;
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_FAT_COUNT)) {
        return false;
    }

    /* 5: exactly one populated and exact total-sector field. */
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_TOTAL_16, 0U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_TOTAL_SECTORS)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u32(block, FAT16_TEST_TOTAL_32,
        (uint32_t)FAT16_TOTAL_SECTORS);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_TOTAL_SECTORS)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_TOTAL_16,
        (uint16_t)(FAT16_TOTAL_SECTORS - 1U));
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_TOTAL_SECTORS)) {
        return false;
    }

    /* 6: the FAT16 size is nonzero, exact and arithmetically bounded. */
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_FAT_16, 0U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_FAT_SIZE)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_FAT_16, UINT16_MAX);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_SPAN_RANGE)) {
        return false;
    }

    /* 7: the fixed root must be 128 aligned 32-byte entries. */
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_ROOT_ENTRIES, 0U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_ROOT_GEOMETRY)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_ROOT_ENTRIES, 127U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_ROOT_GEOMETRY)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_ROOT_ENTRIES, UINT16_MAX);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_ROOT_GEOMETRY)) {
        return false;
    }

    /* 8: derived spans cannot escape the superfloppy namespace. */
    fat16_test_make_bpb(block);
    if (!fat16_parse_bpb_status(block, sizeof(block),
            FAT16_TOTAL_SECTORS - 1U, FAT16_BLOCK_BYTES,
            FAT16_STATUS_SPAN_RANGE)) {
        return false;
    }
    fat16_test_put_u32(block, FAT16_TEST_HIDDEN, 1U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_SPAN_RANGE)) {
        return false;
    }

    /* 9: cluster count, never a label, classifies FAT12/16/32. */
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_TOTAL_16, 4088U);
    if (!fat16_parse_bpb_status(block, sizeof(block), FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, FAT16_STATUS_FAT_CLASS)) {
        return false;
    }
    fat16_test_make_bpb(block);
    fat16_test_put_u16(block, FAT16_TEST_TOTAL_16, 0U);
    fat16_test_put_u32(block, FAT16_TEST_TOTAL_32, 70000U);
    if (!fat16_parse_bpb_status(block, sizeof(block), UINT64_C(70000),
            FAT16_BLOCK_BYTES, FAT16_STATUS_FAT_CLASS)) {
        return false;
    }

    for (size_t index = 0U; index < sizeof(query_bytes); ++index) {
        query_bytes[index] = fat16_test_name[index];
    }
    if (sapote_fat16_make_query(query_bytes, sizeof(query_bytes), &query) !=
            FAT16_STATUS_OK) {
        return false;
    }
    query_bytes[0] = 'X';
    if (!equal_bytes(query.canonical_name, fat16_test_name,
            sizeof(fat16_test_name))) {
        return false;
    }
    fat16_test_make_root(block);
    if (sapote_fat16_find_root(block, sizeof(block), &geometry, &query,
            FAT16_FILE_BYTES, &entry) != FAT16_STATUS_OK) {
        return false;
    }
    fat16_test_make_fat(block);
    if (sapote_fat16_parse_fat(block, sizeof(block), &geometry, &fat) !=
            FAT16_STATUS_OK) {
        return false;
    }
    fat16_test_put_u16(block, FAT16_FILE_CLUSTER * 2U,
        UINT16_C(0xFFF8));
    if (!fat16_parse_fat_status(block, &geometry, FAT16_STATUS_OK)) {
        return false;
    }
    fat16_test_make_fat(block);
    if (sapote_fat16_validate_extent(&geometry, &entry, &fat, &extent) !=
            FAT16_STATUS_OK || extent.lba != UINT64_C(4) ||
        extent.fat_value < UINT16_C(0xFFF8)) {
        return false;
    }

    /* 10: FAT media and reserved entries must agree with the BPB. */
    block[0] = UINT8_C(0xF0);
    if (!fat16_parse_fat_status(block, &geometry, FAT16_STATUS_MEDIA)) {
        return false;
    }
    fat16_test_make_fat(block);
    fat16_test_put_u16(block, 2U, 0U);
    if (!fat16_parse_fat_status(block, &geometry,
            FAT16_STATUS_FAT_RESERVED)) {
        return false;
    }

    /* 11: a root scan cannot run beyond its one-sector end marker. */
    fat16_test_make_root(block);
    for (size_t index = 0U; index < FAT16_ROOT_ENTRIES; ++index) {
        block[index * 32U] = 'A';
        block[index * 32U + 11U] = UINT8_C(0x20);
    }
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_ROOT_END_MISSING)) {
        return false;
    }

    /* 12: an empty root does not contain the target. */
    zero_bytes(block, sizeof(block));
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_TARGET_ABSENT)) {
        return false;
    }

    /* 13: exactly one matching active target is required. */
    fat16_test_make_root(block);
    for (size_t index = 0U; index < sizeof(fat16_test_name); ++index) {
        block[32U + index] = fat16_test_name[index];
    }
    block[43] = UINT8_C(0x20);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_TARGET_DUPLICATE)) {
        return false;
    }

    /* 14: long-file-name entries are outside this reader. */
    fat16_test_make_root(block);
    block[11] = UINT8_C(0x0F);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_LONG_NAME)) {
        return false;
    }

    /* 15: deleted and non-regular attributes are unsupported. */
    fat16_test_make_root(block);
    block[0] = UINT8_C(0xE5);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_DELETED)) {
        return false;
    }
    const uint8_t unsupported_attributes[] = {
        UINT8_C(0x10), UINT8_C(0x08), UINT8_C(0x40)
    };
    for (size_t index = 0U; index < sizeof(unsupported_attributes); ++index) {
        fat16_test_make_root(block);
        block[11] = unsupported_attributes[index];
        if (!fat16_find_root_status(block, &geometry, &query,
                FAT16_FILE_BYTES, FAT16_STATUS_UNSUPPORTED_ENTRY)) {
            return false;
        }
    }

    /* 16: queries and active names must be canonical uppercase 8.3. */
    query_bytes[0] = 's';
    for (size_t index = 1U; index < sizeof(query_bytes); ++index) {
        query_bytes[index] = fat16_test_name[index];
    }
    if (sapote_fat16_make_query(query_bytes, sizeof(query_bytes), &query) !=
            FAT16_STATUS_NAME_MALFORMED) {
        return false;
    }
    if (sapote_fat16_make_query(fat16_test_name, sizeof(fat16_test_name),
            &query) != FAT16_STATUS_OK) {
        return false;
    }
    fat16_test_make_root(block);
    block[0] = 's';
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_NAME_MALFORMED)) {
        return false;
    }

    /* 17: data clusters begin at two and remain within the data region. */
    fat16_test_make_root(block);
    fat16_test_put_u16(block, 26U, 1U);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_CLUSTER_RANGE)) {
        return false;
    }
    fat16_test_make_root(block);
    fat16_test_put_u16(block, 26U, 5000U);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_CLUSTER_RANGE)) {
        return false;
    }

    /* 18: free, bad, reserved and out-of-range FAT entries fail. */
    for (size_t index = 0U;
         index < sizeof(invalid_fat_values) / sizeof(invalid_fat_values[0]);
         ++index) {
        fat16_test_make_fat(block);
        fat16_test_put_u16(block, FAT16_FILE_CLUSTER * 2U,
            invalid_fat_values[index]);
        if (!fat16_parse_fat_status(block, &geometry,
                FAT16_STATUS_FAT_ENTRY)) {
            return false;
        }
    }

    /* 19: even a valid next data cluster is a refused multi-cluster chain. */
    fat16_test_make_fat(block);
    fat16_test_put_u16(block, FAT16_FILE_CLUSTER * 2U,
        FAT16_FILE_CLUSTER);
    if (!fat16_parse_fat_status(block, &geometry,
            FAT16_STATUS_MULTI_CLUSTER)) {
        return false;
    }

    /* 20: lengths are nonzero, exact, contained and destination-bounded. */
    fat16_test_make_root(block);
    fat16_test_put_u32(block, 28U, 0U);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_FILE_SIZE)) {
        return false;
    }
    fat16_test_make_root(block);
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES - 1U, FAT16_STATUS_FILE_SIZE)) {
        return false;
    }
    fat16_test_make_root(block);
    fat16_test_put_u32(block, 28U, FAT16_BLOCK_BYTES + 1U);
    if (!fat16_find_root_status(block, &geometry, &query, UINT32_MAX,
            FAT16_STATUS_FILE_SIZE)) {
        return false;
    }

    /* 21: malformed geometry is refused before translation can overflow. */
    fat16_test_make_root(block);
    if (sapote_fat16_find_root(block, sizeof(block), &geometry, &query,
            FAT16_FILE_BYTES, &entry) != FAT16_STATUS_OK) {
        return false;
    }
    fat16_test_make_fat(block);
    if (sapote_fat16_parse_fat(block, sizeof(block), &geometry, &fat) !=
            FAT16_STATUS_OK) {
        return false;
    }
    corrupt_geometry = geometry;
    corrupt_geometry.first_data_sector = UINT64_MAX;
    enum fat16_status status = sapote_fat16_validate_extent(
        &corrupt_geometry, &entry, &fat, &extent);
    if (status != FAT16_STATUS_SPAN_OVERFLOW &&
        status != FAT16_STATUS_SPAN_RANGE) {
        return false;
    }

    /* 22: no concealed state, retained pointer, or partial FFI result. */
    fat16_test_make_root(block);
    block[33] = 'X';
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_TRAILING_STATE)) {
        return false;
    }
    fat16_test_make_root(block);
    block[64] = 'X';
    if (!fat16_find_root_status(block, &geometry, &query,
            FAT16_FILE_BYTES, FAT16_STATUS_TRAILING_STATE)) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(geometry); ++index) {
        ((uint8_t *)&geometry)[index] = UINT8_C(0xA5);
    }
    if (sapote_fat16_parse_bpb(block, 1U, FAT16_TOTAL_SECTORS,
            FAT16_BLOCK_BYTES, &geometry) != FAT16_STATUS_TRUNCATED ||
        !all_zero(&geometry, sizeof(geometry))) {
        return false;
    }
    for (size_t index = 0U; index < sizeof(payload_bytes); ++index) {
        payload_bytes[index] = (uint8_t)((index * 73U + 19U) & 0xFFU);
    }
    if (sapote_fat16_validate_payload(payload_bytes, sizeof(payload_bytes),
            &payload) != FAT16_STATUS_OK ||
        payload.byte_count != FAT16_FILE_BYTES || payload.deterministic != 1U ||
        !equal_bytes(payload.sha256, fat16_test_sha256,
            sizeof(fat16_test_sha256)) ||
        sapote_fat16_validate_payload(payload_bytes,
            sizeof(payload_bytes) - 1U, &payload) !=
            FAT16_STATUS_PAYLOAD_LENGTH) {
        return false;
    }
    payload_bytes[64] ^= 1U;
    return sapote_fat16_validate_payload(payload_bytes,
        sizeof(payload_bytes), &payload) == FAT16_STATUS_PAYLOAD_CONTENT;
}

static enum filesystem_status transition(
    enum filesystem_state *state,
    enum filesystem_state next
)
{
    bool allowed = false;

    if (state == NULL || next <= FILESYSTEM_UNOPENED ||
        next >= FILESYSTEM_STATE_COUNT) {
        return FILESYSTEM_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return FILESYSTEM_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case FILESYSTEM_UNOPENED:
        allowed = next == FILESYSTEM_SESSION_READY;
        break;
    case FILESYSTEM_SESSION_READY:
    case FILESYSTEM_VOLUME_VALIDATED:
    case FILESYSTEM_FILE_LOCATED:
        allowed = next == FILESYSTEM_BLOCK_CONTROLLER_OWNED ||
            next == FILESYSTEM_STOPPING;
        break;
    case FILESYSTEM_BLOCK_CONTROLLER_OWNED:
        allowed = next == FILESYSTEM_BLOCK_CPU_OWNED ||
            next == FILESYSTEM_STOPPING;
        break;
    case FILESYSTEM_BLOCK_CPU_OWNED:
        allowed = next == FILESYSTEM_BLOCK_CONTROLLER_OWNED ||
            next == FILESYSTEM_VOLUME_VALIDATED ||
            next == FILESYSTEM_FILE_LOCATED ||
            next == FILESYSTEM_FILE_READ ||
            next == FILESYSTEM_STOPPING;
        break;
    case FILESYSTEM_FILE_READ:
        allowed = next == FILESYSTEM_STOPPING;
        break;
    case FILESYSTEM_STOPPING:
        allowed = next == FILESYSTEM_RELEASED;
        break;
    case FILESYSTEM_RELEASED:
    case FILESYSTEM_STATE_COUNT:
        break;
    }
    if (!allowed) {
        if (next < *state && next != FILESYSTEM_STOPPING &&
            next != FILESYSTEM_RELEASED) {
            return FILESYSTEM_STATUS_TRANSITION_REVERSED;
        }
        return FILESYSTEM_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return FILESYSTEM_STATUS_OK;
}

static bool block_result_valid(
    const struct filesystem_block_result *result,
    uint32_t expected_ordinal
)
{
    return result != NULL && result->ordinal == expected_ordinal &&
        result->byte_count == FAT16_BLOCK_BYTES &&
        result->msix_delta == 1U && result->completion_identity &&
        result->guard_clean && result->changed_while_controller_owned &&
        result->cpu_owned;
}

static bool control_block_results(void)
{
    const struct filesystem_block_result good = {
        .ordinal = 3U,
        .byte_count = FAT16_BLOCK_BYTES,
        .msix_delta = 1U,
        .completion_identity = true,
        .guard_clean = true,
        .changed_while_controller_owned = true,
        .cpu_owned = true
    };
    struct filesystem_block_result bad = good;

    if (!block_result_valid(&good, 3U)) {
        return false;
    }
    bad.byte_count = FAT16_BLOCK_BYTES - 1U;
    if (block_result_valid(&bad, 3U)) {
        return false;
    }
    bad = good;
    bad.guard_clean = false;
    if (block_result_valid(&bad, 3U)) {
        return false;
    }
    bad = good;
    bad.completion_identity = false;
    if (block_result_valid(&bad, 3U) || block_result_valid(&good, 4U)) {
        return false;
    }
    bad = good;
    bad.msix_delta = 0U;
    if (block_result_valid(&bad, 3U)) {
        return false;
    }
    bad = good;
    bad.changed_while_controller_owned = false;
    if (block_result_valid(&bad, 3U)) {
        return false;
    }
    bad = good;
    bad.cpu_owned = false;
    if (block_result_valid(&bad, 3U)) {
        return false;
    }
    return true;
}

static bool cpu_block_operation_allowed(enum filesystem_state state)
{
    return state == FILESYSTEM_BLOCK_CPU_OWNED ||
        state == FILESYSTEM_VOLUME_VALIDATED ||
        state == FILESYSTEM_FILE_LOCATED ||
        state == FILESYSTEM_FILE_READ;
}

static bool control_ownership(void)
{
    enum filesystem_state state = FILESYSTEM_UNOPENED;

    if (transition(&state, FILESYSTEM_SESSION_READY) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) !=
            FILESYSTEM_STATUS_OK || cpu_block_operation_allowed(state) ||
        transition(&state, FILESYSTEM_VOLUME_VALIDATED) !=
            FILESYSTEM_STATUS_TRANSITION_INVALID ||
        transition(&state, FILESYSTEM_BLOCK_CPU_OWNED) !=
            FILESYSTEM_STATUS_OK || !cpu_block_operation_allowed(state) ||
        transition(&state, FILESYSTEM_VOLUME_VALIDATED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_BLOCK_CPU_OWNED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_FILE_LOCATED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_BLOCK_CPU_OWNED) !=
            FILESYSTEM_STATUS_OK ||
        transition(&state, FILESYSTEM_FILE_READ) != FILESYSTEM_STATUS_OK) {
        return false;
    }
    return cpu_block_operation_allowed(state) &&
        transition(&state, FILESYSTEM_FILE_READ) ==
            FILESYSTEM_STATUS_TRANSITION_REPEATED;
}

static bool control_cleanup_boundaries(void)
{
    const uint32_t all_resources = UINT32_C(0x00000FFF);

    /*
     * Claim, mapping, DMA, frame, vector, handler, MSI-X binding, queues,
     * session, filesystem object, file buffer and bus master. Each prefix is
     * a partial setup boundary and every bit is released in exact reverse.
     */
    for (uint32_t boundary = 0U; boundary <= 12U; ++boundary) {
        uint32_t acquired = all_resources &
            ((UINT32_C(1) << boundary) - 1U);

        for (uint32_t release = boundary; release > 0U; --release) {
            acquired &= ~(UINT32_C(1) << (release - 1U));
        }
        if (acquired != 0U) {
            return false;
        }
    }
    /* BPB, FAT, root and data failures all close the same one-read session. */
    for (uint32_t read_boundary = 0U; read_boundary <= 4U;
         ++read_boundary) {
        enum filesystem_state state = FILESYSTEM_UNOPENED;
        uint32_t live = all_resources;

        if (transition(&state, FILESYSTEM_SESSION_READY) !=
                FILESYSTEM_STATUS_OK) {
            return false;
        }
        for (uint32_t read = 0U; read < read_boundary; ++read) {
            if (transition(&state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) !=
                    FILESYSTEM_STATUS_OK ||
                transition(&state, FILESYSTEM_BLOCK_CPU_OWNED) !=
                    FILESYSTEM_STATUS_OK) {
                return false;
            }
        }
        if (transition(&state, FILESYSTEM_STOPPING) != FILESYSTEM_STATUS_OK) {
            return false;
        }
        for (uint32_t release = 12U; release > 0U; --release) {
            live &= ~(UINT32_C(1) << (release - 1U));
        }
        if (transition(&state, FILESYSTEM_RELEASED) != FILESYSTEM_STATUS_OK ||
            live != 0U || cpu_block_operation_allowed(state)) {
            return false;
        }
    }
    return true;
}

static bool control_teardown_race(void)
{
    enum filesystem_state state = FILESYSTEM_BLOCK_CPU_OWNED;

    return transition(&state, FILESYSTEM_STOPPING) == FILESYSTEM_STATUS_OK &&
        transition(&state, FILESYSTEM_RELEASED) == FILESYSTEM_STATUS_OK &&
        !cpu_block_operation_allowed(state) &&
        transition(&state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) ==
            FILESYSTEM_STATUS_TRANSITION_REVERSED &&
        filesystem_resources_released();
}

bool filesystem_foundation_self_test(size_t *completed_tests)
{
    size_t completed = 0U;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!control_rust_parser()) {
        return false;
    }
    completed = FAT16_PARSER_ROBUSTNESS_CONTROLS;
    if (!control_block_results()) {
        return false;
    }
    ++completed;
    if (!control_ownership()) {
        return false;
    }
    ++completed;
    if (!control_cleanup_boundaries()) {
        return false;
    }
    ++completed;
    if (!control_teardown_race()) {
        return false;
    }
    ++completed;
    *completed_tests = completed;
    return completed == FILESYSTEM_INTEGRATION_CONTROLS;
}

static enum filesystem_status read_block(
    struct nvme_filesystem_read_session *session,
    enum filesystem_state *state,
    uint64_t lba,
    uint32_t ordinal,
    const uint8_t **block,
    size_t *block_length
)
{
    const uint64_t msix_before = session->msix_completion_count;
    enum nvme_status nvme_status;
    struct filesystem_block_result result;

    *block = NULL;
    *block_length = 0U;
    if (transition(state, FILESYSTEM_BLOCK_CONTROLLER_OWNED) !=
            FILESYSTEM_STATUS_OK) {
        return FILESYSTEM_STATUS_SESSION_STATE;
    }
    nvme_status = nvme_filesystem_session_read(session, lba, ordinal);
    if (nvme_status != NVME_STATUS_OK) {
        return FILESYSTEM_STATUS_NVME_FAILURE;
    }
    if (transition(state, FILESYSTEM_BLOCK_CPU_OWNED) !=
            FILESYSTEM_STATUS_OK) {
        return FILESYSTEM_STATUS_SESSION_STATE;
    }
    nvme_status = nvme_filesystem_session_view(session, ordinal,
        block, block_length);
    if (nvme_status != NVME_STATUS_OK) {
        return FILESYSTEM_STATUS_OWNERSHIP;
    }
    result.ordinal = ordinal;
    result.byte_count = (uint32_t)*block_length;
    result.msix_delta = session->msix_completion_count - msix_before;
    result.completion_identity = session->ignored_completions == 0U;
    result.guard_clean = session->guard_pages_clean;
    result.changed_while_controller_owned =
        session->last_read_changed_while_controller_owned;
    result.cpu_owned = session->state ==
        NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED;
    return block_result_valid(&result, ordinal) ? FILESYSTEM_STATUS_OK :
        FILESYSTEM_STATUS_BLOCK_RESULT;
}

enum filesystem_status filesystem_file_prove(
    struct filesystem_file_proof *proof
)
{
    static const uint8_t canonical_name[FAT16_CANONICAL_NAME_BYTES] =
        {'S', 'A', 'P', 'O', 'T', 'E', ' ', ' ', 'B', 'I', 'N'};
    struct nvme_filesystem_read_session session = {0};
    struct filesystem_validated_volume volume = {0};
    struct filesystem_cpu_file_content content = {0};
    struct fat16_root_query query;
    struct fat16_root_entry entry;
    struct fat16_fat_state fat;
    struct fat16_extent extent;
    const uint8_t *block = NULL;
    size_t block_length = 0U;
    enum filesystem_state state = FILESYSTEM_UNOPENED;
    enum filesystem_status result = FILESYSTEM_STATUS_OK;
    enum nvme_status nvme_status;
    bool session_open = false;

    if (proof == NULL) {
        return FILESYSTEM_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(&installed_proof, sizeof(installed_proof));
    zero_bytes(proof, sizeof(*proof));
    zero_bytes(&query, sizeof(query));
    zero_bytes(&entry, sizeof(entry));
    zero_bytes(&fat, sizeof(fat));
    zero_bytes(&extent, sizeof(extent));
    if (sapote_fat16_make_query(canonical_name, sizeof(canonical_name),
            &query) != FAT16_STATUS_OK) {
        return FILESYSTEM_STATUS_PARSER_FAILURE;
    }
    nvme_status = nvme_filesystem_session_open(&session);
    if (nvme_status == NVME_STATUS_ABSENT) {
        return FILESYSTEM_STATUS_ABSENT;
    }
    if (nvme_status != NVME_STATUS_OK) {
        return FILESYSTEM_STATUS_NVME_FAILURE;
    }
    session_open = true;
    filesystem_proof_active = true;
    if (transition(&state, FILESYSTEM_SESSION_READY) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto cleanup;
    }
    volume.candidate.namespace_blocks = session.namespace_blocks;
    volume.candidate.logical_block_bytes = session.logical_block_bytes;
    volume.candidate.active = true;

    result = read_block(&session, &state, 0U, 1U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_parse_bpb(block, block_length,
            volume.candidate.namespace_blocks,
            volume.candidate.logical_block_bytes,
            &volume.geometry) != FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto cleanup;
    }
    if (transition(&state, FILESYSTEM_VOLUME_VALIDATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto cleanup;
    }
    volume.state = state;
    volume.active = true;
    block = NULL;
    block_length = 0U;

    result = read_block(&session, &state,
        volume.geometry.first_fat_sector, 2U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_parse_fat(block, block_length, &volume.geometry,
            &fat) != FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto cleanup;
    }
    block = NULL;
    block_length = 0U;

    result = read_block(&session, &state,
        volume.geometry.first_root_sector, 3U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_find_root(block, block_length, &volume.geometry,
            &query, sizeof(content.bytes), &entry) != FAT16_STATUS_OK ||
        sapote_fat16_validate_extent(&volume.geometry, &entry, &fat,
            &extent) != FAT16_STATUS_OK ||
        !equal_bytes(entry.canonical_name, canonical_name,
            sizeof(canonical_name))) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto cleanup;
    }
    if (transition(&state, FILESYSTEM_FILE_LOCATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto cleanup;
    }
    block = NULL;
    block_length = 0U;

    result = read_block(&session, &state, extent.lba, 4U,
        &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK || block_length < entry.file_size ||
        sapote_fat16_validate_payload(block, entry.file_size,
            &content.payload) != FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_CONTENT : result;
        goto cleanup;
    }
    for (size_t index = 0U; index < entry.file_size; ++index) {
        content.bytes[index] = block[index];
    }
    content.state = FILESYSTEM_FILE_READ;
    content.cpu_owned = true;
    if (transition(&state, FILESYSTEM_FILE_READ) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto cleanup;
    }
    block = NULL;
    block_length = 0U;
    if (session.read_count != 4U) {
        result = FILESYSTEM_STATUS_READ_COUNT;
        goto cleanup;
    }
    if (session.msix_completion_count != 4U ||
        session.ignored_completions != 0U) {
        result = FILESYSTEM_STATUS_INTERRUPT_COUNT;
        goto cleanup;
    }

cleanup:
    if (session_open) {
        if (transition(&state, FILESYSTEM_STOPPING) !=
                FILESYSTEM_STATUS_OK && result == FILESYSTEM_STATUS_OK) {
            result = FILESYSTEM_STATUS_SESSION_STATE;
        }
        nvme_status = nvme_filesystem_session_close(&session);
        if (nvme_status != NVME_STATUS_OK) {
            result = FILESYSTEM_STATUS_TEARDOWN_FAILURE;
        } else if (transition(&state, FILESYSTEM_RELEASED) !=
                FILESYSTEM_STATUS_OK) {
            result = FILESYSTEM_STATUS_SESSION_STATE;
        } else {
            filesystem_proof_active = false;
        }
    }
    if (result != FILESYSTEM_STATUS_OK) {
        zero_bytes(&content, sizeof(content));
        zero_bytes(proof, sizeof(*proof));
        return result;
    }
    if (!content.cpu_owned || !session.teardown_complete ||
        state != FILESYSTEM_RELEASED) {
        zero_bytes(&content, sizeof(content));
        return FILESYSTEM_STATUS_TEARDOWN_FAILURE;
    }
    for (size_t index = 0U; index < sizeof(proof->canonical_name); ++index) {
        proof->canonical_name[index] = entry.canonical_name[index];
    }
    for (size_t index = 0U; index < sizeof(proof->payload_sha256); ++index) {
        proof->payload_sha256[index] = content.payload.sha256[index];
    }
    proof->file_bytes = content.payload.byte_count;
    proof->read_count = session.read_count;
    proof->msix_completion_count = session.msix_completion_count;
    proof->ignored_completions = session.ignored_completions;
    proof->robustness_tests = FILESYSTEM_CONTROLLED_ROBUSTNESS_TESTS;
    proof->fat16_ready = volume.active;
    proof->file_located = entry.file_size == FAT16_FILE_BYTES;
    proof->contents_valid = content.payload.deterministic == 1U;
    proof->sentinel_valid = session.guard_pages_clean;
    proof->changed_while_controller_owned =
        session.changed_while_controller_owned;
    proof->ownership_complete = content.cpu_owned;
    proof->teardown_complete = session.teardown_complete;
    installed_proof = *proof;
    zero_bytes(&content, sizeof(content));
    return FILESYSTEM_STATUS_OK;
}

static void zero_private_file(struct filesystem_private_file *file)
{
    zero_bytes(file, sizeof(*file));
}

static enum filesystem_status private_read_cleanup(void)
{
    enum filesystem_status result = FILESYSTEM_STATUS_OK;

    if (!private_read_runtime.owned) {
        return FILESYSTEM_STATUS_OK;
    }
    if (private_read_runtime.state != FILESYSTEM_STOPPING &&
        transition(&private_read_runtime.state, FILESYSTEM_STOPPING) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
    }
    if (nvme_filesystem_session_close(&private_read_runtime.session) !=
            NVME_STATUS_OK) {
        /* The lower layer still owns the session; retain its retry token. */
        return FILESYSTEM_STATUS_TEARDOWN_FAILURE;
    } else if (transition(&private_read_runtime.state, FILESYSTEM_RELEASED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
    }
    zero_bytes(&private_read_runtime, sizeof(private_read_runtime));
    return result;
}

enum filesystem_status filesystem_private_read_open(
    struct filesystem_private_file *file,
    uint8_t *destination,
    size_t destination_bytes
)
{
    static const uint8_t canonical_name[FAT16_CANONICAL_NAME_BYTES] =
        {'S', 'A', 'P', 'O', 'T', 'E', ' ', ' ', 'B', 'I', 'N'};
    struct fat16_geometry geometry;
    struct fat16_root_query query;
    struct fat16_root_entry entry;
    struct fat16_fat_state fat;
    struct fat16_extent extent;
    const uint8_t *block = NULL;
    size_t block_length = 0U;
    enum filesystem_status result;
    enum nvme_status nvme_status;

    if (file == NULL || destination == NULL) {
        return FILESYSTEM_STATUS_NULL_ARGUMENT;
    }
    zero_private_file(file);
    if (destination_bytes != FAT16_FILE_BYTES) {
        return FILESYSTEM_STATUS_PRIVATE_BAD_BUFFER;
    }
    zero_bytes(destination, destination_bytes);
    if (private_read_runtime.owned) {
        return FILESYSTEM_STATUS_PRIVATE_BUSY;
    }
    zero_bytes(&private_read_runtime, sizeof(private_read_runtime));
    zero_bytes(&geometry, sizeof(geometry));
    zero_bytes(&query, sizeof(query));
    zero_bytes(&entry, sizeof(entry));
    zero_bytes(&fat, sizeof(fat));
    zero_bytes(&extent, sizeof(extent));
    if (sapote_fat16_make_query(canonical_name, sizeof(canonical_name),
            &query) != FAT16_STATUS_OK) {
        return FILESYSTEM_STATUS_PARSER_FAILURE;
    }
    nvme_status = nvme_filesystem_session_open(&private_read_runtime.session);
    if (nvme_status == NVME_STATUS_ABSENT) {
        return FILESYSTEM_STATUS_ABSENT;
    }
    if (nvme_status != NVME_STATUS_OK) {
        return FILESYSTEM_STATUS_NVME_FAILURE;
    }
    private_read_runtime.owned = true;
    private_read_runtime.state = FILESYSTEM_UNOPENED;
    private_read_runtime.generation = next_private_read_generation++;
    private_read_runtime.kind = PRIVATE_READ_PROCESS;
    if (next_private_read_generation == 0U) {
        next_private_read_generation = 1U;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_SESSION_READY) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, 0U, 1U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_parse_bpb(block, block_length,
            private_read_runtime.session.namespace_blocks,
            private_read_runtime.session.logical_block_bytes,
            &geometry) != FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto fail;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_VOLUME_VALIDATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    block = NULL;
    block_length = 0U;
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, geometry.first_fat_sector, 2U,
        &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_parse_fat(block, block_length, &geometry, &fat) !=
            FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto fail;
    }
    block = NULL;
    block_length = 0U;
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, geometry.first_root_sector, 3U,
        &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_find_root(block, block_length, &geometry, &query,
            (uint32_t)destination_bytes, &entry) != FAT16_STATUS_OK ||
        sapote_fat16_validate_extent(&geometry, &entry, &fat, &extent) !=
            FAT16_STATUS_OK ||
        !equal_bytes(entry.canonical_name, canonical_name,
            sizeof(canonical_name))) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto fail;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_FILE_LOCATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    block = NULL;
    block_length = 0U;
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, extent.lba, 4U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK || block_length < entry.file_size ||
        entry.file_size != FAT16_FILE_BYTES) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_CONTENT : result;
        goto fail;
    }
    for (size_t index = 0U; index < FAT16_FILE_BYTES; ++index) {
        destination[index] = block[index];
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_FILE_READ) !=
            FILESYSTEM_STATUS_OK ||
        private_read_runtime.session.read_count != 4U ||
        private_read_runtime.session.msix_completion_count != 4U ||
        private_read_runtime.session.ignored_completions != 0U ||
        private_read_runtime.session.state !=
            NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED) {
        result = FILESYSTEM_STATUS_OWNERSHIP;
        goto fail;
    }
    file->generation = private_read_runtime.generation;
    file->msix_completion_count =
        private_read_runtime.session.msix_completion_count;
    file->file_bytes = entry.file_size;
    file->read_count = private_read_runtime.session.read_count;
    file->cpu_owned = true;
    file->active = true;
    return FILESYSTEM_STATUS_OK;

fail:
    zero_bytes(destination, destination_bytes);
    if (private_read_cleanup() != FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_TEARDOWN_FAILURE;
    }
    return result;
}

enum filesystem_status filesystem_private_read_close(
    struct filesystem_private_file *file
)
{
    enum filesystem_status status;

    if (file == NULL) {
        return FILESYSTEM_STATUS_NULL_ARGUMENT;
    }
    if (!file->active || !private_read_runtime.owned ||
        private_read_runtime.kind != PRIVATE_READ_PROCESS ||
        file->generation != private_read_runtime.generation) {
        return FILESYSTEM_STATUS_PRIVATE_BAD_TOKEN;
    }
    status = private_read_cleanup();
    if (status == FILESYSTEM_STATUS_OK) {
        zero_private_file(file);
    }
    return status;
}

static void zero_linux_file(struct filesystem_linux_file *file)
{
    zero_bytes(file, sizeof(*file));
}

enum filesystem_status filesystem_linux_read_open(
    struct filesystem_linux_file *file,
    uint8_t *destination,
    size_t destination_bytes
)
{
    struct fat16_geometry geometry;
    struct fat16_root_query query;
    struct fat16_root_entry entry;
    struct linux_fat16_chain chain;
    struct linux_fat16_payload payload;
    const uint8_t *block = NULL;
    size_t block_length = 0U;
    size_t copied = 0U;
    enum filesystem_status result;
    enum nvme_status nvme_status;

    if (file == NULL || destination == NULL) {
        return FILESYSTEM_STATUS_NULL_ARGUMENT;
    }
    zero_linux_file(file);
    if (destination_bytes != LINUX_FAT16_FILE_BYTES) {
        return FILESYSTEM_STATUS_PRIVATE_BAD_BUFFER;
    }
    zero_bytes(destination, destination_bytes);
    if (private_read_runtime.owned) {
        return FILESYSTEM_STATUS_PRIVATE_BUSY;
    }
    zero_bytes(&private_read_runtime, sizeof(private_read_runtime));
    zero_bytes(&geometry, sizeof(geometry));
    zero_bytes(&query, sizeof(query));
    zero_bytes(&entry, sizeof(entry));
    zero_bytes(&chain, sizeof(chain));
    zero_bytes(&payload, sizeof(payload));
    if (sapote_linux_fat16_make_query(&query) != LINUX_FAT16_STATUS_OK) {
        return FILESYSTEM_STATUS_PARSER_FAILURE;
    }
    nvme_status = nvme_filesystem_session_open(&private_read_runtime.session);
    if (nvme_status == NVME_STATUS_ABSENT) {
        return FILESYSTEM_STATUS_ABSENT;
    }
    if (nvme_status != NVME_STATUS_OK) {
        return FILESYSTEM_STATUS_NVME_FAILURE;
    }
    private_read_runtime.owned = true;
    private_read_runtime.state = FILESYSTEM_UNOPENED;
    private_read_runtime.generation = next_private_read_generation++;
    private_read_runtime.kind = PRIVATE_READ_LINUX;
    if (next_private_read_generation == 0U) {
        next_private_read_generation = 1U;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_SESSION_READY) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, 0U, 1U, &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_fat16_parse_bpb(block, block_length,
            private_read_runtime.session.namespace_blocks,
            private_read_runtime.session.logical_block_bytes,
            &geometry) != FAT16_STATUS_OK) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_PARSER_FAILURE : result;
        goto fail;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_VOLUME_VALIDATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    block = NULL;
    block_length = 0U;
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, geometry.first_fat_sector, 2U,
        &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK || block_length != FAT16_BLOCK_BYTES) {
        goto fail;
    }
    for (size_t index = 0U; index < FAT16_BLOCK_BYTES; ++index) {
        private_read_runtime.fat_block[index] = block[index];
    }
    block = NULL;
    block_length = 0U;
    result = read_block(&private_read_runtime.session,
        &private_read_runtime.state, geometry.first_root_sector, 3U,
        &block, &block_length);
    if (result != FILESYSTEM_STATUS_OK ||
        sapote_linux_fat16_find_root(block, block_length, &geometry, &query,
            (uint32_t)destination_bytes, &entry) !=
                LINUX_FAT16_STATUS_OK ||
        sapote_linux_fat16_build_chain(private_read_runtime.fat_block,
            sizeof(private_read_runtime.fat_block), &geometry, &entry,
            &chain) != LINUX_FAT16_STATUS_OK ||
        chain.valid != 1U ||
        chain.cluster_count != LINUX_FAT16_FILE_CLUSTERS) {
        result = result == FILESYSTEM_STATUS_OK ?
            FILESYSTEM_STATUS_LINUX_CHAIN : result;
        goto fail;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_FILE_LOCATED) !=
            FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_SESSION_STATE;
        goto fail;
    }
    for (size_t index = 0U; index < chain.cluster_count; ++index) {
        size_t bytes = destination_bytes - copied;

        block = NULL;
        block_length = 0U;
        result = read_block(&private_read_runtime.session,
            &private_read_runtime.state, chain.lbas[index],
            (uint32_t)(4U + index), &block, &block_length);
        if (result != FILESYSTEM_STATUS_OK ||
            block_length != FAT16_BLOCK_BYTES) {
            goto fail;
        }
        if (bytes > FAT16_BLOCK_BYTES) {
            bytes = FAT16_BLOCK_BYTES;
        }
        for (size_t byte = 0U; byte < bytes; ++byte) {
            destination[copied + byte] = block[byte];
        }
        if (index + 1U == chain.cluster_count) {
            for (size_t byte = bytes; byte < FAT16_BLOCK_BYTES; ++byte) {
                if (block[byte] != 0U) {
                    result = FILESYSTEM_STATUS_CONTENT;
                    goto fail;
                }
            }
        }
        copied += bytes;
    }
    if (copied != destination_bytes ||
        sapote_linux_fat16_validate_payload(destination, destination_bytes,
            &payload) != LINUX_FAT16_STATUS_OK ||
        payload.deterministic != 1U ||
        payload.byte_count != LINUX_FAT16_FILE_BYTES) {
        result = FILESYSTEM_STATUS_LINUX_PAYLOAD;
        goto fail;
    }
    if (transition(&private_read_runtime.state, FILESYSTEM_FILE_READ) !=
            FILESYSTEM_STATUS_OK ||
        private_read_runtime.session.read_count !=
            3U + LINUX_FAT16_FILE_CLUSTERS ||
        private_read_runtime.session.msix_completion_count !=
            3U + LINUX_FAT16_FILE_CLUSTERS ||
        private_read_runtime.session.ignored_completions != 0U ||
        private_read_runtime.session.state !=
            NVME_FILESYSTEM_SESSION_BLOCK_CPU_OWNED) {
        result = FILESYSTEM_STATUS_OWNERSHIP;
        goto fail;
    }
    file->generation = private_read_runtime.generation;
    file->msix_completion_count =
        private_read_runtime.session.msix_completion_count;
    file->file_bytes = entry.file_size;
    file->read_count = private_read_runtime.session.read_count;
    file->cluster_count = chain.cluster_count;
    file->cpu_owned = true;
    file->active = true;
    zero_bytes(private_read_runtime.fat_block,
        sizeof(private_read_runtime.fat_block));
    return FILESYSTEM_STATUS_OK;

fail:
    zero_bytes(destination, destination_bytes);
    if (private_read_cleanup() != FILESYSTEM_STATUS_OK) {
        result = FILESYSTEM_STATUS_TEARDOWN_FAILURE;
    }
    return result;
}

enum filesystem_status filesystem_linux_read_close(
    struct filesystem_linux_file *file
)
{
    enum filesystem_status status;

    if (file == NULL) {
        return FILESYSTEM_STATUS_NULL_ARGUMENT;
    }
    if (!file->active || !private_read_runtime.owned ||
        private_read_runtime.kind != PRIVATE_READ_LINUX ||
        file->generation != private_read_runtime.generation) {
        return FILESYSTEM_STATUS_PRIVATE_BAD_TOKEN;
    }
    status = private_read_cleanup();
    if (status == FILESYSTEM_STATUS_OK) {
        zero_linux_file(file);
    }
    return status;
}

struct filesystem_file_proof filesystem_get_file_proof(void)
{
    return installed_proof;
}

bool filesystem_resources_released(void)
{
    return !filesystem_proof_active && !private_read_runtime.owned &&
        nvme_filesystem_session_resources_released();
}

const char *filesystem_status_string(enum filesystem_status status)
{
    switch (status) {
    case FILESYSTEM_STATUS_OK: return "ok";
    case FILESYSTEM_STATUS_ABSENT: return "filesystem proof fixture is absent";
    case FILESYSTEM_STATUS_NULL_ARGUMENT: return "null filesystem argument";
    case FILESYSTEM_STATUS_PARSER_SELF_TEST:
        return "FAT16 parser self-test failed";
    case FILESYSTEM_STATUS_PARSER_FAILURE:
        return "FAT16 metadata parser refused the volume";
    case FILESYSTEM_STATUS_NVME_FAILURE:
        return "private NVMe filesystem session failed";
    case FILESYSTEM_STATUS_SESSION_STATE:
        return "filesystem session state is invalid";
    case FILESYSTEM_STATUS_BLOCK_RESULT:
        return "filesystem block result is invalid";
    case FILESYSTEM_STATUS_OWNERSHIP:
        return "filesystem inspected a non-CPU-owned block";
    case FILESYSTEM_STATUS_CONTENT:
        return "filesystem file content is invalid";
    case FILESYSTEM_STATUS_READ_COUNT:
        return "filesystem read count is not exactly four";
    case FILESYSTEM_STATUS_INTERRUPT_COUNT:
        return "filesystem MSI-X completion count is not exactly four";
    case FILESYSTEM_STATUS_TRANSITION_REPEATED:
        return "filesystem transition was repeated";
    case FILESYSTEM_STATUS_TRANSITION_REVERSED:
        return "filesystem transition was reversed";
    case FILESYSTEM_STATUS_TRANSITION_INVALID:
        return "filesystem transition is invalid";
    case FILESYSTEM_STATUS_TEARDOWN_FAILURE:
        return "filesystem teardown leaked or failed";
    case FILESYSTEM_STATUS_PRIVATE_BUSY:
        return "private filesystem read session is already active";
    case FILESYSTEM_STATUS_PRIVATE_BAD_TOKEN:
        return "private filesystem read token is stale";
    case FILESYSTEM_STATUS_PRIVATE_BAD_BUFFER:
        return "private filesystem destination is not exactly 128 bytes";
    case FILESYSTEM_STATUS_LINUX_CHAIN:
        return "bounded BusyBox FAT16 cluster chain is invalid";
    case FILESYSTEM_STATUS_LINUX_PAYLOAD:
        return "BusyBox payload SHA-256 is invalid";
    case FILESYSTEM_STATUS_COUNT:
    default: return "unknown filesystem status";
    }
}
