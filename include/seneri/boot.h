/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_BOOT_H
#define SENERI_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_BOOT_MAGIC UINT32_C(0x36D76289)
#define MULTIBOOT2_TAG_ALIGNMENT 8U
#define MULTIBOOT2_MAX_INFORMATION_SIZE (16U * 1024U * 1024U)

/*
 * The promise that every physical address below 4 GiB is reachable at the same
 * virtual address. src/arch/x86_64/boot.S makes it before long mode and
 * src/kernel/paging.c keeps it afterwards, which is why the constant survived
 * the kernel taking ownership of its own page tables.
 *
 * Three things depend on it: the frame allocator addresses exactly this range,
 * acpi_span_is_early_mapped gates every firmware table read against it, and
 * paging.c reads each of its own tables through the table's physical address.
 * Narrowing it means re-pointing all three, so it is deliberately unchanged
 * here; docs/VIRTUAL_MEMORY.md records why.
 */
#define SENERI_EARLY_PHYSICAL_LIMIT UINT64_C(0x100000000)

enum multiboot2_tag_type {
    MULTIBOOT2_TAG_END = 0,
    MULTIBOOT2_TAG_COMMAND_LINE = 1,
    MULTIBOOT2_TAG_BOOT_LOADER_NAME = 2,
    MULTIBOOT2_TAG_MODULE = 3,
    MULTIBOOT2_TAG_BASIC_MEMORY = 4,
    MULTIBOOT2_TAG_BOOT_DEVICE = 5,
    MULTIBOOT2_TAG_MEMORY_MAP = 6,
    MULTIBOOT2_TAG_VBE = 7,
    MULTIBOOT2_TAG_FRAMEBUFFER = 8,
    MULTIBOOT2_TAG_ELF_SECTIONS = 9,
    MULTIBOOT2_TAG_APM = 10,
    MULTIBOOT2_TAG_EFI32 = 11,
    MULTIBOOT2_TAG_EFI64 = 12,
    MULTIBOOT2_TAG_SMBIOS = 13,
    MULTIBOOT2_TAG_ACPI_OLD = 14,
    MULTIBOOT2_TAG_ACPI_NEW = 15,
    MULTIBOOT2_TAG_NETWORK = 16,
    MULTIBOOT2_TAG_EFI_MEMORY_MAP = 17,
    MULTIBOOT2_TAG_EFI_BOOT_SERVICES = 18,
    MULTIBOOT2_TAG_EFI32_IMAGE_HANDLE = 19,
    MULTIBOOT2_TAG_EFI64_IMAGE_HANDLE = 20,
    MULTIBOOT2_TAG_LOAD_BASE_ADDRESS = 21
};

enum multiboot2_memory_type {
    MULTIBOOT2_MEMORY_AVAILABLE = 1,
    MULTIBOOT2_MEMORY_RESERVED = 2,
    MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE = 3,
    MULTIBOOT2_MEMORY_NVS = 4,
    MULTIBOOT2_MEMORY_BAD_RAM = 5
};

enum boot_status {
    BOOT_STATUS_OK = 0,
    BOOT_STATUS_NULL_CONTEXT,
    BOOT_STATUS_BAD_MAGIC,
    BOOT_STATUS_NULL_INFORMATION,
    BOOT_STATUS_MISALIGNED_INFORMATION,
    BOOT_STATUS_INFORMATION_TOO_SMALL,
    BOOT_STATUS_INFORMATION_TOO_LARGE,
    BOOT_STATUS_INFORMATION_SIZE_MISALIGNED,
    BOOT_STATUS_INFORMATION_OVERFLOW,
    BOOT_STATUS_RESERVED_FIELD_NONZERO,
    BOOT_STATUS_TRUNCATED_TAG,
    BOOT_STATUS_TAG_TOO_SMALL,
    BOOT_STATUS_TAG_OVERFLOW,
    BOOT_STATUS_DUPLICATE_MEMORY_MAP,
    BOOT_STATUS_MEMORY_MAP_TOO_SMALL,
    BOOT_STATUS_MEMORY_ENTRY_TOO_SMALL,
    BOOT_STATUS_MEMORY_ENTRY_VERSION,
    BOOT_STATUS_MEMORY_MAP_REMAINDER,
    BOOT_STATUS_MEMORY_REGION_OVERFLOW,
    BOOT_STATUS_UNSUPPORTED_MODULE,
    BOOT_STATUS_STRING_NOT_TERMINATED,
    BOOT_STATUS_ACPI_TAG_TOO_SMALL,
    BOOT_STATUS_DUPLICATE_ACPI_TAG,
    BOOT_STATUS_BAD_END_TAG,
    BOOT_STATUS_MISSING_END_TAG,
    BOOT_STATUS_MISSING_MEMORY_MAP,
    BOOT_STATUS_DUPLICATE_FRAMEBUFFER,
    BOOT_STATUS_FRAMEBUFFER_TAG_TOO_SMALL,
    BOOT_STATUS_FRAMEBUFFER_NOT_DIRECT_COLOUR,
    BOOT_STATUS_BAD_FRAMEBUFFER_DEPTH,
    BOOT_STATUS_BAD_FRAMEBUFFER_GEOMETRY,
    BOOT_STATUS_BAD_FRAMEBUFFER_PITCH,
    BOOT_STATUS_BAD_FRAMEBUFFER_ADDRESS,
    BOOT_STATUS_BAD_FRAMEBUFFER_CHANNEL,
    BOOT_STATUS_FRAMEBUFFER_OUTSIDE_EARLY_MAP
};

/*
 * Multiboot2 3.6.12 describes a framebuffer with a 32-byte common part and a
 * colour description whose shape depends on the type. Only direct RGB is
 * modelled: an indexed palette and EGA text are different things wearing the
 * same tag, and Seneri refuses each by name rather than guessing.
 */
#define MULTIBOOT2_FRAMEBUFFER_COMMON_SIZE 32U
#define MULTIBOOT2_FRAMEBUFFER_RGB_SIZE 38U
#define MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED 0U
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB 1U
#define MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT 2U

/* The only depth this kernel packs a pixel for. Anything else is refused. */
#define BOOT_FRAMEBUFFER_BITS_PER_PIXEL 32U
#define BOOT_FRAMEBUFFER_BYTES_PER_PIXEL (BOOT_FRAMEBUFFER_BITS_PER_PIXEL / 8U)

/*
 * What the loader actually set, which need not be what the header asked for.
 * Multiboot2 3.1.10 makes the request a preference, so every number here is
 * read back rather than assumed, and the geometry is validated before anything
 * computes an address from it.
 */
struct boot_framebuffer {
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint64_t size;
    uint8_t bits_per_pixel;
    uint8_t red_position;
    uint8_t green_position;
    uint8_t blue_position;
    bool present;
};

struct multiboot2_information_header {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed));

struct multiboot2_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct multiboot2_string_tag {
    struct multiboot2_tag tag;
    char string[];
} __attribute__((packed));

struct multiboot2_memory_map_tag {
    struct multiboot2_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    uint8_t entries[];
} __attribute__((packed));

struct multiboot2_memory_map_entry {
    uint64_t base_address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct multiboot2_acpi_tag {
    struct multiboot2_tag tag;
    uint8_t rsdp[];
} __attribute__((packed));

struct boot_memory_region {
    uint64_t base_address;
    uint64_t length;
    uint32_t type;
};

struct boot_context {
    uintptr_t information_start;
    uintptr_t information_end;
    const struct multiboot2_memory_map_tag *memory_map;
    const struct multiboot2_acpi_tag *acpi_old;
    const struct multiboot2_acpi_tag *acpi_new;
    const char *boot_loader_name;
    const char *command_line;
    size_t boot_loader_name_length;
    size_t command_line_length;
    size_t memory_map_entry_count;
    uint64_t reported_usable_bytes;
    uint64_t highest_reported_address;
    struct boot_framebuffer framebuffer;
};

enum boot_status boot_context_parse(
    uint32_t magic,
    uintptr_t information_address,
    struct boot_context *context
);

bool boot_memory_region_at(
    const struct boot_context *context,
    size_t index,
    struct boot_memory_region *region
);

const char *boot_status_string(enum boot_status status);

#endif
