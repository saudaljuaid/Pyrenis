/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_PAGING_H
#define SAPOTE_PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/acpi.h>

/*
 * Intel SDM volume 3A section 4.5 splits a canonical 48-bit virtual address
 * into four nine-bit table indices and a twelve-bit offset. Level 4 is the
 * PML4, level 1 the page table; a leaf at level 2 is a 2 MiB page and a leaf at
 * level 3 a 1 GiB page.
 */
#define PAGING_LEVEL_COUNT 4U
#define PAGING_ENTRIES_PER_TABLE 512U
#define PAGING_PAGE_SIZE UINT64_C(4096)
#define PAGING_HUGE_PAGE_SIZE UINT64_C(0x200000)

/* The one user layout admitted by the v0.7.0 process proof. */
#define PAGING_PROCESS_IMAGE_ADDRESS UINT64_C(0x0000400000000000)
#define PAGING_PROCESS_STACK_GUARD UINT64_C(0x0000400000200000)
#define PAGING_PROCESS_STACK_PAGES 4U
#define PAGING_PROCESS_STACK_BASE \
    (PAGING_PROCESS_STACK_GUARD + PAGING_PAGE_SIZE)
#define PAGING_PROCESS_STACK_END \
    (PAGING_PROCESS_STACK_BASE + \
        PAGING_PROCESS_STACK_PAGES * PAGING_PAGE_SIZE)

/* The fixed high-user layout admitted only by the v0.8.0 BusyBox proof. */
#define PAGING_LINUX_IMAGE_BASE UINT64_C(0x0000400001000000)
#define PAGING_LINUX_IMAGE_PAGES 9U
#define PAGING_LINUX_IMAGE_END \
    (PAGING_LINUX_IMAGE_BASE + PAGING_LINUX_IMAGE_PAGES * PAGING_PAGE_SIZE)
#define PAGING_LINUX_HEAP_BASE UINT64_C(0x0000400001100000)
#define PAGING_LINUX_HEAP_PAGES 2U
#define PAGING_LINUX_ANON_ADDRESS UINT64_C(0x0000400001110000)
#define PAGING_LINUX_STACK_GUARD UINT64_C(0x0000400001200000)
#define PAGING_LINUX_STACK_PAGES 4U
#define PAGING_LINUX_STACK_BASE \
    (PAGING_LINUX_STACK_GUARD + PAGING_PAGE_SIZE)
#define PAGING_LINUX_STACK_END \
    (PAGING_LINUX_STACK_BASE + PAGING_LINUX_STACK_PAGES * PAGING_PAGE_SIZE)
#define PAGING_PROCESS_ALIAS_MAX_PAGES 16U
#define PAGING_PROCESS_EXPECTED_MAX_PAGES 24U

/* console.c's fallback output page, described here once for every owner. */
#define PAGING_VGA_TEXT_BUFFER_BASE UINT64_C(0x000B8000)

/*
 * The virtual page the paging scenario and the boot lifecycle proof map a
 * freshly allocated frame at. It sits above the identity window so a mapping
 * made here cannot collide with one, and clear of 0x100000000, which the
 * page-fault scenario needs to stay absent.
 */
#define PAGING_PROBE_ADDRESS UINT64_C(0x0000000200000000)

/*
 * How much of a firmware-declared PCI Express configuration window Sapote makes
 * uncacheable and therefore readable as configuration space. One 2 MiB region,
 * because that is the unit the identity map is carved in: a device window is a
 * whole such region turned into 4 KiB pages, and one region is two buses of
 * configuration space, which is every bus any machine Sapote is tested on
 * populates. Reaching further needs the window mapped somewhere of its own, and
 * that is a later increment; src/kernel/pci.c reads no further than this.
 */
#define PAGING_ECAM_WINDOW_SIZE PAGING_HUGE_PAGE_SIZE

/*
 * How many 2 MiB regions of the identity map a framebuffer may claim. Eight is
 * 16 MiB, which covers 1920x1080 at 32 bits with room to spare; a loader that
 * sets a mode larger than this gets no framebuffer rather than a partly mapped
 * one. Unlike the configuration window a framebuffer is several regions wide,
 * which is the whole reason this bound exists.
 */
#define PAGING_MAX_FRAMEBUFFER_REGIONS 8U

/*
 * One VGA window, one local APIC, every bounded I/O APIC, one optional ECAM
 * span, and one optional framebuffer span. The registry stays fixed storage;
 * a new kind has to justify changing this public policy bound.
 */
#define PAGING_DEVICE_WINDOW_CAPACITY (4U + ACPI_MAX_IO_APICS)
#define PAGING_DEVICE_WINDOW_MAX_LENGTH \
    (PAGING_MAX_FRAMEBUFFER_REGIONS * PAGING_HUGE_PAGE_SIZE)
#define PAGING_DEVICE_WINDOW_NONE SIZE_MAX

enum paging_status {
    PAGING_STATUS_OK = 0,
    PAGING_STATUS_NULL_ARGUMENT,
    PAGING_STATUS_ALREADY_INITIALIZED,
    PAGING_STATUS_NOT_INITIALIZED,
    PAGING_STATUS_INTERRUPTS_ENABLED,
    PAGING_STATUS_NO_EXECUTE_UNSUPPORTED,
    PAGING_STATUS_NO_EXECUTE_INACTIVE,
    PAGING_STATUS_WRITE_PROTECT_INACTIVE,
    PAGING_STATUS_FIVE_LEVEL_PAGING,
    PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED,
    PAGING_STATUS_PAT_UNSUPPORTED,
    PAGING_STATUS_PAT_LAYOUT_UNSAFE,
    PAGING_STATUS_PAT_READBACK_MISMATCH,
    PAGING_STATUS_BAD_KERNEL_LAYOUT,
    PAGING_STATUS_BAD_DEVICE_WINDOW_KIND,
    PAGING_STATUS_BAD_DEVICE_WINDOW_INSTANCE,
    PAGING_STATUS_UNSUPPORTED_DEVICE_WINDOW_MEMORY_TYPE,
    PAGING_STATUS_BAD_DEVICE_WINDOW_PERMISSIONS,
    PAGING_STATUS_ZERO_LENGTH_DEVICE_WINDOW,
    PAGING_STATUS_UNALIGNED_DEVICE_WINDOW,
    PAGING_STATUS_DEVICE_WINDOW_RANGE_OVERFLOW,
    PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE,
    PAGING_STATUS_TOO_MANY_DEVICE_WINDOWS,
    PAGING_STATUS_CONFLICTING_DEVICE_WINDOW_OVERLAP,
    PAGING_STATUS_OVERLAPPING_DEVICE_WINDOWS,
    PAGING_STATUS_DUPLICATE_DEVICE_WINDOW,
    PAGING_STATUS_REQUIRED_DEVICE_WINDOW_MISSING,
    PAGING_STATUS_DEVICE_WINDOW_KERNEL_OVERLAP,
    PAGING_STATUS_INSTALLED_DEVICE_WINDOW_MISMATCH,
    PAGING_STATUS_ZERO_LENGTH,
    PAGING_STATUS_UNALIGNED_ADDRESS,
    PAGING_STATUS_NONCANONICAL_ADDRESS,
    PAGING_STATUS_RANGE_OVERFLOW,
    PAGING_STATUS_PHYSICAL_TOO_WIDE,
    PAGING_STATUS_BAD_PERMISSIONS,
    PAGING_STATUS_CONFLICTING_MEMORY_TYPES,
    PAGING_STATUS_MEMORY_TYPE_CHANGE_UNSAFE,
    PAGING_STATUS_WRITABLE_AND_EXECUTABLE,
    PAGING_STATUS_ALREADY_MAPPED,
    PAGING_STATUS_NOT_MAPPED,
    PAGING_STATUS_HUGE_PAGE_PRESENT,
    PAGING_STATUS_OUT_OF_FRAMES,
    PAGING_STATUS_VALIDATION_FAILURE,
    PAGING_STATUS_PROCESS_BUSY,
    PAGING_STATUS_PROCESS_BAD_TOKEN,
    PAGING_STATUS_PROCESS_BAD_STATE,
    PAGING_STATUS_PROCESS_BAD_MAPPING,
    PAGING_STATUS_PROCESS_ALIAS_STATE,
    PAGING_STATUS_SUPERVISOR_INTENT_FULL,
    PAGING_STATUS_COUNT
};

/*
 * The memory type decoded from the leaf's PAT, PCD, and PWT bits and the
 * installed IA32_PAT value. INVALID is a real reportable outcome: translation
 * must not guess when a PAT byte contains an architecturally reserved value.
 */
enum paging_memory_type {
    PAGING_MEMORY_WRITE_BACK = 0,
    PAGING_MEMORY_WRITE_COMBINING,
    PAGING_MEMORY_UNCACHEABLE,
    PAGING_MEMORY_WRITE_THROUGH,
    PAGING_MEMORY_WRITE_PROTECTED,
    PAGING_MEMORY_UNCACHED_MINUS,
    PAGING_MEMORY_INVALID,
    PAGING_MEMORY_TYPE_COUNT
};

enum paging_device_window_kind {
    PAGING_DEVICE_WINDOW_VGA_TEXT = 0,
    PAGING_DEVICE_WINDOW_LOCAL_APIC,
    PAGING_DEVICE_WINDOW_IO_APIC,
    PAGING_DEVICE_WINDOW_PCI_ECAM,
    PAGING_DEVICE_WINDOW_FRAMEBUFFER,
    PAGING_DEVICE_WINDOW_KIND_COUNT
};

/*
 * Device access is semantic too. Execute and user access are deliberately not
 * representable, so neither can enter the hierarchy through this registry.
 */
enum paging_device_window_permissions {
    PAGING_DEVICE_WINDOW_READ = 0U,
    PAGING_DEVICE_WINDOW_WRITE = 1U << 0
};

struct paging_device_window {
    enum paging_device_window_kind kind;
    uint32_t instance;
    uint64_t physical_base;
    uint64_t length;
    enum paging_memory_type memory_type;
    uint32_t permissions;
};

struct paging_device_windows {
    size_t count;
    struct paging_device_window entries[PAGING_DEVICE_WINDOW_CAPACITY];
};

/*
 * A mapping is described by what it permits, never by raw entry bits. Read
 * access is implied by presence, so the absence of every flag is a read-only,
 * non-executable, write-back page. UNCACHED and WRITE_COMBINING are mutually
 * exclusive memory-type requests; naming both is refused rather than resolved
 * by precedence. There is deliberately no user flag: every entry Sapote writes
 * is supervisor-only, and a request naming an unknown bit is refused rather
 * than masked.
 */
enum paging_permissions {
    PAGING_READ = 0U,
    PAGING_WRITE = 1U << 0,
    PAGING_EXECUTE = 1U << 1,
    PAGING_UNCACHED = 1U << 2,
    PAGING_WRITE_COMBINING = 1U << 3
};

struct paging_state {
    uint64_t root_physical_address;
    size_t table_frames;
    size_t fine_regions;
    uint64_t pat_before;
    uint64_t pat_after;
    unsigned int write_combining_pat_entry;
    bool no_execute_active;
    bool write_protect_active;
    bool active;
};

struct paging_translation {
    uint64_t physical_address;
    uint32_t permissions;
    enum paging_memory_type memory_type;
    unsigned int level;
    bool user;
};

enum paging_process_space_state {
    PAGING_PROCESS_SPACE_INVALID = 0,
    PAGING_PROCESS_SPACE_BUILDING,
    PAGING_PROCESS_SPACE_INSTALLED,
    PAGING_PROCESS_SPACE_ACTIVE,
    PAGING_PROCESS_SPACE_RELEASED,
    PAGING_PROCESS_SPACE_STATE_COUNT
};

enum paging_process_mapping_kind {
    PAGING_PROCESS_MAPPING_IMAGE = 0,
    PAGING_PROCESS_MAPPING_STACK,
    PAGING_PROCESS_MAPPING_LINUX_IMAGE,
    PAGING_PROCESS_MAPPING_LINUX_STACK,
    PAGING_PROCESS_MAPPING_LINUX_HEAP,
    PAGING_PROCESS_MAPPING_LINUX_ANON,
    PAGING_PROCESS_MAPPING_KIND_COUNT
};

/* Public tokens contain identity only; all table ownership stays in paging.c. */
struct paging_process_space {
    uint64_t root_physical_address;
    uint64_t generation;
    size_t table_frames;
    enum paging_process_space_state state;
};

struct paging_process_image_alias {
    uint64_t physical_address;
    uint64_t generation;
    bool active;
};

struct paging_process_alias_set {
    uint64_t physical_addresses[PAGING_PROCESS_ALIAS_MAX_PAGES];
    uint64_t generation;
    size_t count;
    bool active;
};

struct paging_process_expected_page {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint32_t permissions;
};

/*
 * The result of walking the installed hierarchy in software. This is the check
 * `make verify` cannot make: the ELF W^X assertion describes the file, and this
 * describes the machine.
 */
struct paging_audit {
    size_t leaf_count;
    size_t huge_leaves;
    size_t writable_leaves;
    size_t executable_leaves;
    size_t write_execute_leaves;
    size_t user_leaves;
};

void paging_device_windows_reset(struct paging_device_windows *windows);
enum paging_status paging_device_windows_add(
    struct paging_device_windows *windows,
    enum paging_device_window_kind kind,
    uint32_t instance,
    uint64_t physical_base,
    uint64_t length,
    enum paging_memory_type memory_type,
    uint32_t permissions
);
enum paging_status paging_device_windows_validate(
    const struct paging_device_windows *windows,
    struct paging_device_windows *validated
);
enum paging_status paging_initialize(const struct paging_device_windows *windows);
enum paging_status paging_map(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t length,
    uint32_t permissions
);
enum paging_status paging_unmap(uint64_t virtual_address, uint64_t length);
enum paging_status paging_protect(
    uint64_t virtual_address,
    uint64_t length,
    uint32_t permissions
);
enum paging_status paging_translate(
    uint64_t virtual_address,
    struct paging_translation *translation
);
enum paging_status paging_audit_hierarchy(struct paging_audit *audit);
enum paging_status paging_process_space_build(
    struct paging_process_space *space
);
enum paging_status paging_process_image_alias_narrow(
    const struct paging_process_space *space,
    uint64_t physical_address,
    struct paging_process_image_alias *alias
);
enum paging_status paging_process_alias_set_narrow(
    const struct paging_process_space *space,
    const uint64_t *physical_addresses,
    size_t count,
    struct paging_process_alias_set *alias
);
enum paging_status paging_process_map_user_page(
    struct paging_process_space *space,
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
);
enum paging_status paging_process_unmap_user_page(
    struct paging_process_space *space,
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address
);
enum paging_status paging_process_validate(
    struct paging_process_space *space,
    uint64_t image_physical_address,
    const uintptr_t stack_frames[PAGING_PROCESS_STACK_PAGES]
);
enum paging_status paging_process_validate_linux(
    struct paging_process_space *space,
    const struct paging_process_expected_page *pages,
    size_t page_count
);
enum paging_status paging_process_translate(
    const struct paging_process_space *space,
    uint64_t virtual_address,
    struct paging_translation *translation
);
enum paging_status paging_process_activate(struct paging_process_space *space);
enum paging_status paging_process_restore_kernel(
    struct paging_process_space *space
);
enum paging_status paging_process_image_alias_restore(
    const struct paging_process_space *space,
    struct paging_process_image_alias *alias
);
enum paging_status paging_process_alias_set_restore(
    const struct paging_process_space *space,
    struct paging_process_alias_set *alias
);
enum paging_status paging_process_space_release(
    struct paging_process_space *space
);
bool paging_process_resources_released(void);
enum paging_status paging_verify_device_windows(
    const struct paging_device_windows *expected,
    size_t *failed_index
);
enum paging_status paging_verify(void);
struct paging_state paging_get_state(void);
const struct paging_device_windows *paging_get_device_windows(void);
bool paging_is_active(void);
bool paging_self_test(void);
const char *paging_status_string(enum paging_status status);
const char *paging_memory_type_string(enum paging_memory_type memory_type);
const char *paging_device_window_kind_string(
    enum paging_device_window_kind kind
);

/*
 * Store one byte through a supervisor pointer at an instruction address a test
 * can name. Written in assembly for the same reason interrupts.S owns the other
 * fault probes: the scenario matches the faulting RIP exactly, and a compiler
 * is free to move, duplicate, or delete an equivalent C store.
 */
void paging_probe_write(volatile uint8_t *target, uint8_t value);
extern const uint8_t paging_probe_write_site[];

#endif
