/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_LINUX_ELF64_H
#define SAPOTE_LINUX_ELF64_H

#include <stddef.h>
#include <stdint.h>

#define LINUX_ELF64_FILE_BYTES 33584U
#define LINUX_ELF64_MAX_PROGRAM_HEADERS 8U
#define LINUX_ELF64_PROGRAM_HEADERS 5U
#define LINUX_ELF64_MAX_LOAD_SEGMENTS 4U
#define LINUX_ELF64_LOAD_SEGMENTS 4U
#define LINUX_ELF64_IMAGE_PAGES 9U
#define LINUX_ELF64_PAGE_BYTES UINT64_C(4096)
#define LINUX_ELF64_ENTRY UINT64_C(0x000040000100107A)
#define LINUX_ELF64_PARSER_ROBUSTNESS_CONTROLS 24U

enum linux_elf64_status {
    LINUX_ELF64_STATUS_OK = 0,
    LINUX_ELF64_STATUS_NULL_ARGUMENT,
    LINUX_ELF64_STATUS_TRUNCATED,
    LINUX_ELF64_STATUS_FILE_LENGTH,
    LINUX_ELF64_STATUS_MAGIC,
    LINUX_ELF64_STATUS_CLASS,
    LINUX_ELF64_STATUS_DATA,
    LINUX_ELF64_STATUS_IDENT_VERSION,
    LINUX_ELF64_STATUS_ABI,
    LINUX_ELF64_STATUS_IDENT_PADDING,
    LINUX_ELF64_STATUS_TYPE,
    LINUX_ELF64_STATUS_MACHINE,
    LINUX_ELF64_STATUS_HEADER_VERSION,
    LINUX_ELF64_STATUS_HEADER_FLAGS,
    LINUX_ELF64_STATUS_HEADER_SIZE,
    LINUX_ELF64_STATUS_PROGRAM_OFFSET,
    LINUX_ELF64_STATUS_PROGRAM_SIZE,
    LINUX_ELF64_STATUS_PROGRAM_COUNT,
    LINUX_ELF64_STATUS_PROGRAM_TABLE,
    LINUX_ELF64_STATUS_SEGMENT_TYPE,
    LINUX_ELF64_STATUS_SEGMENT_FLAGS,
    LINUX_ELF64_STATUS_FILE_RANGE,
    LINUX_ELF64_STATUS_LOAD_SIZE,
    LINUX_ELF64_STATUS_ALIGNMENT,
    LINUX_ELF64_STATUS_VIRTUAL_ADDRESS,
    LINUX_ELF64_STATUS_ADDRESS_OVERFLOW,
    LINUX_ELF64_STATUS_OVERLAP,
    LINUX_ELF64_STATUS_ENTRY,
    LINUX_ELF64_STATUS_STACK,
    LINUX_ELF64_STATUS_MEASURED_CONJUNCTION,
    LINUX_ELF64_STATUS_COUNT
};

struct linux_elf64_segment {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t mapping_start;
    uint64_t mapping_end;
    uint32_t flags;
    uint32_t reserved;
};

struct linux_elf64_validated_image {
    uint32_t valid;
    uint32_t program_header_count;
    uint32_t segment_count;
    uint32_t non_load_count;
    uint64_t entry;
    struct linux_elf64_segment segments[LINUX_ELF64_MAX_LOAD_SEGMENTS];
};

uint32_t sapote_linux_elf64_self_test(void);
enum linux_elf64_status sapote_linux_elf64_parse(
    const uint8_t *input,
    size_t input_len,
    struct linux_elf64_validated_image *out
);

#endif
