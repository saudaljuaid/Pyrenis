/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/cpu.h>
#include <sapote/memory.h>
#include <sapote/paging.h>

/*
 * Intel SDM volume 3A section 4.5, tables 4-14 through 4-19, define the entry
 * bits shared by every level of a 4-level hierarchy.
 */
#define PAGE_PRESENT (UINT64_C(1) << 0)
#define PAGE_WRITABLE (UINT64_C(1) << 1)
#define PAGE_USER (UINT64_C(1) << 2)
#define PAGE_WRITE_THROUGH (UINT64_C(1) << 3)
#define PAGE_CACHE_DISABLE (UINT64_C(1) << 4)
#define PAGE_ACCESSED (UINT64_C(1) << 5)
#define PAGE_DIRTY (UINT64_C(1) << 6)
#define PAGE_HUGE (UINT64_C(1) << 7)
#define PAGE_GLOBAL (UINT64_C(1) << 8)
#define PAGE_LARGE_PAT (UINT64_C(1) << 12)
#define PAGE_NO_EXECUTE (UINT64_C(1) << 63)

/*
 * Intel SDM volume 3A section 4.5: bits 51:12 of an entry hold the physical
 * address of the next table or of the mapped page.
 */
#define PAGE_FRAME_MASK UINT64_C(0x000FFFFFFFFFF000)

/*
 * Bits 51:12 hold a frame number, so the highest page an entry can name starts
 * at PAGE_FRAME_MASK and ends one byte below this. The bound is on the address
 * rather than on the mask: the mask's own low twelve bits are zero, so
 * comparing a last byte against it would reject the highest legal page.
 */
#define PAGE_PHYSICAL_LIMIT (UINT64_C(1) << 52)

/*
 * Intel SDM volume 3A section 2.2.1 and table 2-1: EFER is MSR 0xC0000080 and
 * bit 11 is NXE. Until it is set, bit 63 of an entry is reserved and using such
 * an entry raises a reserved-bit page fault instead of enforcing no-execute.
 */
#define IA32_EFER_MSR UINT32_C(0xC0000080)
#define EFER_NO_EXECUTE_ENABLE (UINT64_C(1) << 11)

/*
 * Intel SDM volume 3A section 2.5: CR0.WP is bit 16. With it clear, supervisor
 * writes ignore the read-only bit entirely, so every permission this subsystem
 * installs would be advisory for ring 0 - which is the only ring Sapote has.
 */
#define CR0_WRITE_PROTECT (UINT64_C(1) << 16)

/*
 * Intel SDM volume 3A sections 2.5 and 4.5: CR4.PAE is bit 5 and CR4.LA57 is
 * bit 12. LA57 selects a five-level hierarchy, which would make every index
 * this file computes wrong by one level.
 */
#define CR4_PHYSICAL_ADDRESS_EXTENSION (UINT64_C(1) << 5)
#define CR4_FIVE_LEVEL_PAGING (UINT64_C(1) << 12)

/*
 * Intel SDM volume 3A section 14.8: IA32_PAT is MSR 0x277 and holds eight
 * one-byte memory types. The bootstrap hierarchy selects only entry 0. Existing
 * Sapote mappings select entry 0 for RAM and entry 3 for device registers, so
 * entry 1 can be changed without retyping a live mapping. PWT alone selects it
 * at every leaf size, avoiding the PAT bit whose position differs between 4 KiB
 * and large leaves. Entry 0 remains write-back and entry 3 uncacheable.
 */
#define IA32_PAT_MSR UINT32_C(0x00000277)
#define PAT_TYPE_UNCACHEABLE UINT8_C(0x00)
#define PAT_TYPE_WRITE_COMBINING UINT8_C(0x01)
#define PAT_TYPE_WRITE_THROUGH UINT8_C(0x04)
#define PAT_TYPE_WRITE_PROTECTED UINT8_C(0x05)
#define PAT_TYPE_WRITE_BACK UINT8_C(0x06)
#define PAT_TYPE_UNCACHED_MINUS UINT8_C(0x07)
#define PAT_WRITE_BACK_ENTRY 0U
#define PAT_WRITE_COMBINING_ENTRY 1U
#define PAT_DEVICE_ENTRY 3U

/* Intel SDM volume 2A CPUID: CPUID.01H:EDX[16] reports IA32_PAT. */
#define CPUID_BASIC_FEATURES UINT32_C(0x00000001)
#define CPUID_PAT UINT32_C(0x00010000)

/*
 * Intel SDM volume 3A section 4.1.4 and AMD APM volume 2 section 5.4.1: the
 * no-execute bit is reported by CPUID.80000001H:EDX[20], and leaf 0x80000000
 * reports how far the extended leaves go, so it must be asked first.
 */
#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_NO_EXECUTE UINT32_C(0x00100000)

/*
 * linker.ld refuses to link an image that ends above this, so the kernel can
 * never span more 2 MiB regions than the identity builder's measured bound.
 * The runtime layout check is what the self-test drives; this assertion keeps
 * the linked and runtime policies together.
 */
#define PAGING_KERNEL_IMAGE_LIMIT UINT64_C(0x800000)
#define PAGING_MAX_KERNEL_FINE_REGIONS 4U
#define PAGING_MAX_SECTIONS 4U

_Static_assert(
    PAGING_KERNEL_IMAGE_LIMIT <=
        PAGING_MAX_KERNEL_FINE_REGIONS * PAGING_HUGE_PAGE_SIZE,
    "the linked kernel bound needs more fine regions than paging reserves"
);

_Static_assert(
    PAGING_DEVICE_WINDOW_CAPACITY == 4U + ACPI_MAX_IO_APICS,
    "device-window capacity no longer covers discovered hardware"
);
_Static_assert(
    PAGING_ECAM_WINDOW_SIZE == PAGING_HUGE_PAGE_SIZE,
    "the configuration window is no longer exactly one identity map region"
);
_Static_assert(
    SAPOTE_EARLY_PHYSICAL_LIMIT % PAGING_HUGE_PAGE_SIZE == 0U,
    "the identity window is not a whole number of 2 MiB pages"
);
_Static_assert(
    PAGING_PROBE_ADDRESS >= SAPOTE_EARLY_PHYSICAL_LIMIT,
    "the paging probe page would collide with the identity window"
);

/* The private hierarchy paging_self_test builds: a root and five tables. */
#define PAGING_TEST_ARENA_PAGES 6U
#define PAGING_TEST_ENTRIES \
    (PAGING_TEST_ARENA_PAGES * (PAGING_PAGE_SIZE / sizeof(uint64_t)))
#define PAGING_TEST_PAT UINT64_C(0x0007040600070106)
#define PAGING_SUPERVISOR_INTENT_CAPACITY 8192U

extern uint8_t __kernel_start[];
extern uint8_t __kernel_end[];
extern uint8_t __text_start[];
extern uint8_t __text_end[];
extern uint8_t __rodata_start[];
extern uint8_t __rodata_end[];
extern uint8_t __data_start[];

/*
 * Where a hierarchy's tables come from. The live hierarchy draws them from the
 * frame allocator; the self-test draws them from a private arena so it can
 * prove every rejection, including exhaustion, without a frame allocator and
 * without touching the tables the processor is running on.
 */
struct page_hierarchy {
    uint64_t root;
    uint64_t arena_base;
    size_t arena_capacity;
    size_t arena_used;
    size_t table_frames;
    bool live;
};

struct paging_section {
    uint64_t start;
    uint64_t end;
    uint32_t permissions;
};

static struct paging_state state;
static struct page_hierarchy live_hierarchy;
static struct paging_section kernel_sections[PAGING_MAX_SECTIONS];
static struct paging_device_windows installed_device_windows;
static size_t fine_region_count;
static size_t kernel_section_count;
static uint64_t test_arena[PAGING_TEST_ENTRIES] __attribute__((aligned(4096)));

struct process_space_runtime {
    struct page_hierarchy hierarchy;
    uint64_t generation;
    enum paging_process_space_state state;
    bool owned;
};

struct process_alias_page_runtime {
    uint64_t physical_address;
    uint64_t saved_entry;
    uint64_t split_table;
    uint64_t private_saved_entry;
    uint64_t private_split_table;
    bool split;
    bool private_split;
};

struct process_alias_runtime {
    uint64_t generation;
    struct process_alias_page_runtime pages[PAGING_PROCESS_ALIAS_MAX_PAGES];
    size_t count;
    bool owned;
};

struct supervisor_mapping_intent {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t length;
    uint32_t permissions;
};

static struct process_space_runtime process_space_runtime;
static struct process_alias_runtime process_alias_runtime;
static uint64_t next_process_generation = UINT64_C(1);
static uint64_t next_alias_generation = UINT64_C(1);
static struct supervisor_mapping_intent supervisor_intents[
    PAGING_SUPERVISOR_INTENT_CAPACITY];
static size_t supervisor_intent_count;

static uint8_t pat_entry(uint64_t pat, unsigned int index)
{
    return (uint8_t)(pat >> (index * 8U));
}

static uint64_t pat_replace_entry(
    uint64_t pat,
    unsigned int index,
    uint8_t memory_type
)
{
    const unsigned int shift = index * 8U;
    const uint64_t mask = UINT64_C(0xFF) << shift;

    return (pat & ~mask) | ((uint64_t)memory_type << shift);
}

static enum paging_memory_type decode_pat_type(uint8_t memory_type)
{
    switch (memory_type) {
    case PAT_TYPE_WRITE_BACK:
        return PAGING_MEMORY_WRITE_BACK;
    case PAT_TYPE_WRITE_COMBINING:
        return PAGING_MEMORY_WRITE_COMBINING;
    case PAT_TYPE_UNCACHEABLE:
        return PAGING_MEMORY_UNCACHEABLE;
    case PAT_TYPE_WRITE_THROUGH:
        return PAGING_MEMORY_WRITE_THROUGH;
    case PAT_TYPE_WRITE_PROTECTED:
        return PAGING_MEMORY_WRITE_PROTECTED;
    case PAT_TYPE_UNCACHED_MINUS:
        return PAGING_MEMORY_UNCACHED_MINUS;
    default:
        return PAGING_MEMORY_INVALID;
    }
}

/*
 * Intel SDM volume 3A table 14-11: PWT is index bit 0, PCD bit 1, and PAT
 * bit 2. The PAT bit is leaf bit 7 for a 4 KiB PTE and bit 12 for a large leaf.
 */
static unsigned int leaf_pat_index(uint64_t entry, unsigned int level)
{
    const uint64_t pat_bit = level == 1U ? PAGE_HUGE : PAGE_LARGE_PAT;
    unsigned int index = 0U;

    if ((entry & PAGE_WRITE_THROUGH) != 0U) {
        index |= 1U;
    }

    if ((entry & PAGE_CACHE_DISABLE) != 0U) {
        index |= 2U;
    }

    if ((entry & pat_bit) != 0U) {
        index |= 4U;
    }

    return index;
}

static enum paging_memory_type leaf_memory_type(
    uint64_t entry,
    unsigned int level,
    uint64_t pat
)
{
    return decode_pat_type(pat_entry(pat, leaf_pat_index(entry, level)));
}

/*
 * Every table frame is below SAPOTE_EARLY_PHYSICAL_LIMIT and that whole range
 * is mapped to itself, so a table's physical address is also the address this
 * code reads and writes it through. allocate_table refuses any frame that would
 * break that, which is the one assumption the entire walk rests on.
 */
static uint64_t *table_at(uint64_t physical_address)
{
    return (uint64_t *)(uintptr_t)physical_address;
}

static void zero_table(uint64_t *table)
{
    for (size_t index = 0; index < PAGING_ENTRIES_PER_TABLE; ++index) {
        table[index] = 0U;
    }
}

/*
 * Intel SDM volume 3A section 4.5: level 4 indexes bits 47:39, level 3 bits
 * 38:30, level 2 bits 29:21, and level 1 bits 20:12.
 */
static size_t table_index(uint64_t virtual_address, unsigned int level)
{
    const unsigned int shift = 12U + 9U * (level - 1U);

    return (size_t)((virtual_address >> shift) &
        (PAGING_ENTRIES_PER_TABLE - 1U));
}

/* A leaf at level 1 spans 4 KiB, at level 2 spans 2 MiB, at level 3 1 GiB. */
static uint64_t level_page_size(unsigned int level)
{
    return UINT64_C(1) << (12U + 9U * (level - 1U));
}

/*
 * Intel SDM volume 3A section 3.3.7.1: an address is canonical when bits 63:48
 * all repeat bit 47. Shifting right by 47 leaves those seventeen bits, which
 * must therefore be all zero or all one.
 */
static bool address_is_canonical(uint64_t virtual_address)
{
    const uint64_t high = virtual_address >> 47U;

    return high == 0U || high == UINT64_C(0x1FFFF);
}

static uint64_t permissions_to_flags(uint32_t permissions)
{
    uint64_t flags = PAGE_PRESENT;

    if ((permissions & PAGING_WRITE) != 0U) {
        flags |= PAGE_WRITABLE;
    }

    if ((permissions & PAGING_EXECUTE) == 0U) {
        flags |= PAGE_NO_EXECUTE;
    }

    if ((permissions & PAGING_UNCACHED) != 0U) {
        flags |= PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH;
    }

    if ((permissions & PAGING_WRITE_COMBINING) != 0U) {
        flags |= PAGE_WRITE_THROUGH;
    }

    return flags;
}

static uint32_t flags_to_permissions(
    uint64_t entry,
    unsigned int level,
    uint64_t pat
)
{
    uint32_t permissions = PAGING_READ;
    const enum paging_memory_type memory_type =
        leaf_memory_type(entry, level, pat);

    if ((entry & PAGE_WRITABLE) != 0U) {
        permissions |= PAGING_WRITE;
    }

    if ((entry & PAGE_NO_EXECUTE) == 0U) {
        permissions |= PAGING_EXECUTE;
    }

    if (memory_type == PAGING_MEMORY_UNCACHEABLE) {
        permissions |= PAGING_UNCACHED;
    } else if (memory_type == PAGING_MEMORY_WRITE_COMBINING) {
        permissions |= PAGING_WRITE_COMBINING;
    }

    return permissions;
}

static enum paging_status validate_permissions(uint32_t permissions)
{
    const uint32_t known = PAGING_WRITE | PAGING_EXECUTE | PAGING_UNCACHED |
        PAGING_WRITE_COMBINING;

    /*
     * There is no user flag to request. Refusing an unknown bit rather than
     * masking it keeps a caller that meant PAGE_USER from getting a supervisor
     * mapping it believes is a user one.
     */
    if ((permissions & ~known) != 0U) {
        return PAGING_STATUS_BAD_PERMISSIONS;
    }

    if ((permissions & PAGING_WRITE) != 0U &&
        (permissions & PAGING_EXECUTE) != 0U) {
        return PAGING_STATUS_WRITABLE_AND_EXECUTABLE;
    }

    if ((permissions & PAGING_UNCACHED) != 0U &&
        (permissions & PAGING_WRITE_COMBINING) != 0U) {
        return PAGING_STATUS_CONFLICTING_MEMORY_TYPES;
    }

    return PAGING_STATUS_OK;
}

static enum paging_status validate_virtual_range(
    uint64_t virtual_address,
    uint64_t length,
    uint64_t page_size
)
{
    if (length == 0U) {
        return PAGING_STATUS_ZERO_LENGTH;
    }

    if ((virtual_address & (page_size - 1U)) != 0U ||
        (length & (page_size - 1U)) != 0U) {
        return PAGING_STATUS_UNALIGNED_ADDRESS;
    }

    if (!address_is_canonical(virtual_address)) {
        return PAGING_STATUS_NONCANONICAL_ADDRESS;
    }

    if (length > UINT64_MAX - virtual_address) {
        return PAGING_STATUS_RANGE_OVERFLOW;
    }

    /*
     * A range may start canonical and run into the hole between the two halves
     * of the address space. Checking the last byte catches that; checking only
     * the base would let the walk index a table from a truncated address.
     */
    if (!address_is_canonical(virtual_address + length - 1U)) {
        return PAGING_STATUS_NONCANONICAL_ADDRESS;
    }

    return PAGING_STATUS_OK;
}

static enum paging_status validate_physical_range(
    uint64_t physical_address,
    uint64_t length,
    uint64_t page_size
)
{
    if ((physical_address & (page_size - 1U)) != 0U) {
        return PAGING_STATUS_UNALIGNED_ADDRESS;
    }

    if (length > UINT64_MAX - physical_address) {
        return PAGING_STATUS_RANGE_OVERFLOW;
    }

    if (physical_address + length > PAGE_PHYSICAL_LIMIT) {
        return PAGING_STATUS_PHYSICAL_TOO_WIDE;
    }

    return PAGING_STATUS_OK;
}

void paging_device_windows_reset(struct paging_device_windows *windows)
{
    if (windows == NULL) {
        return;
    }

    windows->count = 0U;
}

static bool device_memory_type_supported(enum paging_memory_type memory_type)
{
    return memory_type == PAGING_MEMORY_WRITE_BACK ||
        memory_type == PAGING_MEMORY_WRITE_COMBINING ||
        memory_type == PAGING_MEMORY_UNCACHEABLE;
}

static enum paging_status validate_device_window_entry(
    const struct paging_device_window *window
)
{
    if (window->kind < PAGING_DEVICE_WINDOW_VGA_TEXT ||
        window->kind >= PAGING_DEVICE_WINDOW_KIND_COUNT) {
        return PAGING_STATUS_BAD_DEVICE_WINDOW_KIND;
    }

    if ((window->kind != PAGING_DEVICE_WINDOW_IO_APIC &&
            window->instance != 0U) ||
        (window->kind == PAGING_DEVICE_WINDOW_IO_APIC &&
            window->instance >= ACPI_MAX_IO_APICS)) {
        return PAGING_STATUS_BAD_DEVICE_WINDOW_INSTANCE;
    }

    if (!device_memory_type_supported(window->memory_type)) {
        return PAGING_STATUS_UNSUPPORTED_DEVICE_WINDOW_MEMORY_TYPE;
    }

    if ((window->permissions & ~(uint32_t)PAGING_DEVICE_WINDOW_WRITE) != 0U) {
        return PAGING_STATUS_BAD_DEVICE_WINDOW_PERMISSIONS;
    }

    if (window->length == 0U) {
        return PAGING_STATUS_ZERO_LENGTH_DEVICE_WINDOW;
    }

    /* Check addition before alignment so a malformed high range has one cause. */
    if (window->length > UINT64_MAX - window->physical_base) {
        return PAGING_STATUS_DEVICE_WINDOW_RANGE_OVERFLOW;
    }

    if ((window->physical_base & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (window->length & (PAGING_PAGE_SIZE - 1U)) != 0U) {
        return PAGING_STATUS_UNALIGNED_DEVICE_WINDOW;
    }

    if (window->kind == PAGING_DEVICE_WINDOW_PCI_ECAM &&
        (window->physical_base & (PAGING_HUGE_PAGE_SIZE - 1U)) != 0U) {
        return PAGING_STATUS_UNALIGNED_DEVICE_WINDOW;
    }

    if ((window->kind == PAGING_DEVICE_WINDOW_VGA_TEXT &&
            window->physical_base != PAGING_VGA_TEXT_BUFFER_BASE) ||
        ((window->kind == PAGING_DEVICE_WINDOW_VGA_TEXT ||
            window->kind == PAGING_DEVICE_WINDOW_LOCAL_APIC ||
            window->kind == PAGING_DEVICE_WINDOW_IO_APIC) &&
            window->length != PAGING_PAGE_SIZE) ||
        (window->kind == PAGING_DEVICE_WINDOW_PCI_ECAM &&
            window->length != PAGING_ECAM_WINDOW_SIZE)) {
        return PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE;
    }

    if (window->physical_base == 0U ||
        window->length > PAGING_DEVICE_WINDOW_MAX_LENGTH ||
        window->physical_base + window->length >
            SAPOTE_EARLY_PHYSICAL_LIMIT) {
        return PAGING_STATUS_DEVICE_WINDOW_UNSUPPORTED_RANGE;
    }

    return PAGING_STATUS_OK;
}

enum paging_status paging_device_windows_add(
    struct paging_device_windows *windows,
    enum paging_device_window_kind kind,
    uint32_t instance,
    uint64_t physical_base,
    uint64_t length,
    enum paging_memory_type memory_type,
    uint32_t permissions
)
{
    struct paging_device_window window;
    enum paging_status status;

    if (windows == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    if (windows->count >= PAGING_DEVICE_WINDOW_CAPACITY) {
        return PAGING_STATUS_TOO_MANY_DEVICE_WINDOWS;
    }

    window.kind = kind;
    window.instance = instance;
    window.physical_base = physical_base;
    window.length = length;
    window.memory_type = memory_type;
    window.permissions = permissions;
    status = validate_device_window_entry(&window);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    windows->entries[windows->count] = window;
    ++windows->count;
    return PAGING_STATUS_OK;
}

static int compare_device_windows(
    const struct paging_device_window *left,
    const struct paging_device_window *right
)
{
    if (left->physical_base != right->physical_base) {
        return left->physical_base < right->physical_base ? -1 : 1;
    }

    if (left->length != right->length) {
        return left->length < right->length ? -1 : 1;
    }

    if (left->kind != right->kind) {
        return left->kind < right->kind ? -1 : 1;
    }

    if (left->instance != right->instance) {
        return left->instance < right->instance ? -1 : 1;
    }

    if (left->memory_type != right->memory_type) {
        return left->memory_type < right->memory_type ? -1 : 1;
    }

    if (left->permissions != right->permissions) {
        return left->permissions < right->permissions ? -1 : 1;
    }

    return 0;
}

static bool ranges_overlap(
    const struct paging_device_window *left,
    const struct paging_device_window *right
)
{
    const uint64_t left_end = left->physical_base + left->length;
    const uint64_t right_end = right->physical_base + right->length;

    return left->physical_base < right_end &&
        right->physical_base < left_end;
}

enum paging_status paging_device_windows_validate(
    const struct paging_device_windows *windows,
    struct paging_device_windows *validated
)
{
    struct paging_device_windows normalized;
    bool found_vga = false;
    bool found_local_apic = false;
    bool found_io_apic = false;
    bool conflicting_overlap = false;
    bool duplicate = false;
    bool overlap = false;

    if (windows == NULL || validated == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    if (windows->count > PAGING_DEVICE_WINDOW_CAPACITY) {
        return PAGING_STATUS_TOO_MANY_DEVICE_WINDOWS;
    }

    normalized.count = windows->count;

    for (size_t index = 0U; index < windows->count; ++index) {
        const struct paging_device_window *window = &windows->entries[index];
        enum paging_status status = validate_device_window_entry(window);

        if (status != PAGING_STATUS_OK) {
            return status;
        }

        if (window->kind == PAGING_DEVICE_WINDOW_VGA_TEXT) {
            found_vga = true;
        } else if (window->kind == PAGING_DEVICE_WINDOW_LOCAL_APIC) {
            found_local_apic = true;
        } else if (window->kind == PAGING_DEVICE_WINDOW_IO_APIC) {
            found_io_apic = true;
        }

        normalized.entries[index] = *window;
    }

    /* Collect every fault before applying fixed, insertion-independent order. */
    for (size_t left = 0U; left < windows->count; ++left) {
        for (size_t right = left + 1U; right < windows->count; ++right) {
            const struct paging_device_window *first =
                &windows->entries[left];
            const struct paging_device_window *second =
                &windows->entries[right];
            const bool pair_overlaps = ranges_overlap(first, second);

            if (pair_overlaps &&
                first->memory_type != second->memory_type) {
                conflicting_overlap = true;
            }

            if (compare_device_windows(first, second) == 0 ||
                (first->kind == second->kind &&
                    first->instance == second->instance)) {
                duplicate = true;
            } else if (pair_overlaps) {
                overlap = true;
            }
        }
    }

    if (conflicting_overlap) {
        return PAGING_STATUS_CONFLICTING_DEVICE_WINDOW_OVERLAP;
    }

    if (duplicate) {
        return PAGING_STATUS_DUPLICATE_DEVICE_WINDOW;
    }

    if (overlap) {
        return PAGING_STATUS_OVERLAPPING_DEVICE_WINDOWS;
    }

    if (!found_vga || !found_local_apic || !found_io_apic) {
        return PAGING_STATUS_REQUIRED_DEVICE_WINDOW_MISSING;
    }

    /* A bounded insertion sort produces the canonical installed order. */
    for (size_t index = 1U; index < normalized.count; ++index) {
        const struct paging_device_window current = normalized.entries[index];
        size_t destination = index;

        while (destination > 0U &&
            compare_device_windows(&current,
                &normalized.entries[destination - 1U]) < 0) {
            normalized.entries[destination] =
                normalized.entries[destination - 1U];
            --destination;
        }

        normalized.entries[destination] = current;
    }

    *validated = normalized;
    return PAGING_STATUS_OK;
}

static enum paging_status allocate_table(
    struct page_hierarchy *hierarchy,
    uint64_t *physical_address
)
{
    uintptr_t frame = 0U;

    if (hierarchy->arena_capacity != 0U) {
        if (hierarchy->arena_used == hierarchy->arena_capacity) {
            return PAGING_STATUS_OUT_OF_FRAMES;
        }

        frame = (uintptr_t)(hierarchy->arena_base +
            (uint64_t)hierarchy->arena_used * PAGING_PAGE_SIZE);
        ++hierarchy->arena_used;
    } else if (frame_allocate(&frame) != FRAME_STATUS_OK) {
        return PAGING_STATUS_OUT_OF_FRAMES;
    }

    /*
     * Checked before the frame is touched: a table outside the identity window
     * has no address this code could write it through, so zeroing it first
     * would be the fault rather than the diagnosis.
     */
    if ((uint64_t)frame >= SAPOTE_EARLY_PHYSICAL_LIMIT) {
        return PAGING_STATUS_PHYSICAL_TOO_WIDE;
    }

    zero_table(table_at((uint64_t)frame));
    ++hierarchy->table_frames;
    *physical_address = (uint64_t)frame;
    return PAGING_STATUS_OK;
}

/*
 * Only the installed hierarchy has translations the processor could have
 * cached. A private hierarchy is never in CR3, so flushing on its behalf would
 * evict an unrelated live entry and prove nothing.
 */
static void invalidate(
    const struct page_hierarchy *hierarchy,
    uint64_t virtual_address
)
{
    if (hierarchy->live) {
        cpu_invalidate_page((uintptr_t)virtual_address);
    }
}

/*
 * Walk down to the entry that would describe virtual_address at target_level,
 * creating the interior tables above it when asked to. Returns NOT_MAPPED when
 * an interior table is absent and creation was not requested, and
 * HUGE_PAGE_PRESENT when a larger leaf already covers the address.
 */
static enum paging_status entry_for(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    unsigned int target_level,
    bool create,
    uint64_t **entry
)
{
    uint64_t table = hierarchy->root;

    for (unsigned int level = PAGING_LEVEL_COUNT; level > target_level;
         --level) {
        uint64_t *slot = &table_at(table)[table_index(virtual_address, level)];

        if ((*slot & PAGE_PRESENT) == 0U) {
            uint64_t next = 0U;
            enum paging_status status;

            if (!create) {
                return PAGING_STATUS_NOT_MAPPED;
            }

            status = allocate_table(hierarchy, &next);

            if (status != PAGING_STATUS_OK) {
                return status;
            }

            /*
             * Interior entries are deliberately the most permissive the
             * architecture allows. Effective permissions are the conjunction
             * down the whole path, so a read-only or non-executable interior
             * entry would silently override every leaf beneath it and there
             * would be two places a permission is decided instead of one.
             */
            *slot = next | PAGE_PRESENT | PAGE_WRITABLE;
            table = next;
            continue;
        }

        if ((*slot & PAGE_HUGE) != 0U) {
            return PAGING_STATUS_HUGE_PAGE_PRESENT;
        }

        table = *slot & PAGE_FRAME_MASK;
    }

    *entry = &table_at(table)[table_index(virtual_address, target_level)];
    return PAGING_STATUS_OK;
}

static bool table_is_empty(uint64_t table)
{
    const uint64_t *entries = table_at(table);

    for (size_t index = 0; index < PAGING_ENTRIES_PER_TABLE; ++index) {
        if ((entries[index] & PAGE_PRESENT) != 0U) {
            return false;
        }
    }

    return true;
}

/*
 * Hand a table frame back. The live hierarchy returns it to the frame
 * allocator; the private hierarchy the self-test builds draws from a
 * bump-allocated arena that has nowhere to return it to, so there the frame is
 * simply dropped and only the count moves.
 */
static void release_table(struct page_hierarchy *hierarchy, uint64_t physical)
{
    if (hierarchy->arena_capacity == 0U) {
        (void)frame_release((uintptr_t)physical);
    }

    --hierarchy->table_frames;
}

/*
 * Give back any interior table an unmap has just emptied, and then the table
 * above it if removing that one emptied it in turn.
 *
 * Without this an unmap leaves a fully empty page table mapped forever. That
 * was tolerable while the only unmap in the kernel ran once at boot; the heap's
 * growth rollback unmaps pages in ordinary operation, so a repeated
 * grow-and-fail cycle would leak a table frame at a time with nothing to
 * notice. The frame allocator would run dry and blame whoever asked last.
 *
 * The root is never reclaimed: CR3 points at it, and a hierarchy with no root
 * is not a hierarchy.
 */
static void reclaim_empty_tables(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address
)
{
    /* Indexed by level, so entry zero is unused and the root sits at four. */
    uint64_t tables[PAGING_LEVEL_COUNT + 1U] = {0U, 0U, 0U, 0U, 0U};

    tables[PAGING_LEVEL_COUNT] = hierarchy->root;

    for (unsigned int level = PAGING_LEVEL_COUNT; level > 1U; --level) {
        const uint64_t entry =
            table_at(tables[level])[table_index(virtual_address, level)];

        /*
         * A larger leaf on the path means there is no page table below it to
         * reclaim, and an absent entry means it has already gone.
         */
        if ((entry & PAGE_PRESENT) == 0U || (entry & PAGE_HUGE) != 0U) {
            return;
        }

        tables[level - 1U] = entry & PAGE_FRAME_MASK;
    }

    /*
     * Walk back up. The first table that still holds an entry stops the climb,
     * because everything above it is reachable through that entry.
     */
    for (unsigned int level = 1U; level < PAGING_LEVEL_COUNT; ++level) {
        uint64_t *parent;

        if (!table_is_empty(tables[level])) {
            return;
        }

        parent = &table_at(tables[level + 1U])
            [table_index(virtual_address, level + 1U)];
        *parent = 0U;
        release_table(hierarchy, tables[level]);
    }
}

static void clear_range(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t length,
    unsigned int level
)
{
    const uint64_t page_size = level_page_size(level);

    for (uint64_t offset = 0U; offset < length; offset += page_size) {
        uint64_t *entry = NULL;

        if (entry_for(hierarchy, virtual_address + offset, level, false,
                &entry) != PAGING_STATUS_OK) {
            continue;
        }

        *entry = 0U;
        invalidate(hierarchy, virtual_address + offset);

        /*
         * Only a 4 KiB leaf can empty a page table. A 2 MiB leaf lives in a
         * page directory that the identity map keeps populated, and this
         * kernel never unmaps one.
         */
        if (level == 1U) {
            reclaim_empty_tables(hierarchy, virtual_address + offset);
        }
    }
}

/*
 * Every range operation validates the whole range before it writes any of it,
 * so a refusal leaves the hierarchy exactly as it was found. The only failure
 * the second pass can still hit is running out of table frames, and that one is
 * rolled back explicitly.
 */
static enum paging_status map_range(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t length,
    uint32_t permissions,
    unsigned int level
)
{
    const uint64_t page_size = level_page_size(level);
    enum paging_status status = validate_permissions(permissions);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = validate_virtual_range(virtual_address, length, page_size);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = validate_physical_range(physical_address, length, page_size);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    for (uint64_t offset = 0U; offset < length; offset += page_size) {
        uint64_t *entry = NULL;

        status = entry_for(hierarchy, virtual_address + offset, level, false,
            &entry);

        if (status == PAGING_STATUS_OK) {
            if ((*entry & PAGE_PRESENT) != 0U) {
                return PAGING_STATUS_ALREADY_MAPPED;
            }
        } else if (status != PAGING_STATUS_NOT_MAPPED) {
            return status;
        }
    }

    for (uint64_t offset = 0U; offset < length; offset += page_size) {
        uint64_t *entry = NULL;

        status = entry_for(hierarchy, virtual_address + offset, level, true,
            &entry);

        if (status != PAGING_STATUS_OK) {
            clear_range(hierarchy, virtual_address, offset, level);
            return status;
        }

        *entry = (physical_address + offset) |
            permissions_to_flags(permissions);

        if (level > 1U) {
            *entry |= PAGE_HUGE;
        }

        invalidate(hierarchy, virtual_address + offset);
    }

    return PAGING_STATUS_OK;
}

static enum paging_status require_mapped_range(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t length
)
{
    for (uint64_t offset = 0U; offset < length; offset += PAGING_PAGE_SIZE) {
        uint64_t *entry = NULL;
        const enum paging_status status = entry_for(hierarchy,
            virtual_address + offset, 1U, false, &entry);

        if (status != PAGING_STATUS_OK) {
            return status;
        }

        if ((*entry & PAGE_PRESENT) == 0U) {
            return PAGING_STATUS_NOT_MAPPED;
        }
    }

    return PAGING_STATUS_OK;
}

static enum paging_status unmap_range(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t length
)
{
    enum paging_status status = validate_virtual_range(virtual_address, length,
        PAGING_PAGE_SIZE);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = require_mapped_range(hierarchy, virtual_address, length);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    clear_range(hierarchy, virtual_address, length, 1U);
    return PAGING_STATUS_OK;
}

static void apply_permissions(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t length,
    uint32_t permissions
)
{
    const uint64_t flags = permissions_to_flags(permissions);

    for (uint64_t offset = 0U; offset < length; offset += PAGING_PAGE_SIZE) {
        uint64_t *entry = NULL;

        /*
         * require_mapped_range already proved every entry in this range
         * resolves at level 1. The guard keeps the pointer provably non-null
         * rather than validating anything a second time.
         */
        if (entry_for(hierarchy, virtual_address + offset, 1U, false, &entry) !=
            PAGING_STATUS_OK) {
            continue;
        }

        *entry = (*entry & PAGE_FRAME_MASK) | flags;
        invalidate(hierarchy, virtual_address + offset);
    }
}

static enum paging_status protect_range(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t length,
    uint32_t permissions,
    uint64_t pat
)
{
    enum paging_status status = validate_permissions(permissions);
    enum paging_memory_type requested_type;

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    requested_type = leaf_memory_type(permissions_to_flags(permissions), 1U,
        pat);

    status = validate_virtual_range(virtual_address, length, PAGING_PAGE_SIZE);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = require_mapped_range(hierarchy, virtual_address, length);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    /*
     * Changing a live physical range's memory type needs the processor's full
     * cache-transition protocol, not only INVLPG. This protection API changes
     * access rights; it refuses to smuggle a cache-policy transition into that
     * operation. Boot establishes framebuffer WC before the hierarchy is live.
     */
    for (uint64_t offset = 0U; offset < length; offset += PAGING_PAGE_SIZE) {
        uint64_t *entry = NULL;

        status = entry_for(hierarchy, virtual_address + offset, 1U, false,
            &entry);

        if (status != PAGING_STATUS_OK ||
            leaf_memory_type(*entry, 1U, pat) != requested_type) {
            return status == PAGING_STATUS_OK ?
                PAGING_STATUS_MEMORY_TYPE_CHANGE_UNSAFE : status;
        }
    }

    apply_permissions(hierarchy, virtual_address, length, permissions);
    return PAGING_STATUS_OK;
}

static void fill_translation(
    struct paging_translation *translation,
    uint64_t entry,
    uint64_t virtual_address,
    unsigned int level,
    bool writable,
    bool executable,
    bool user,
    uint64_t pat
)
{
    const uint64_t page_size = level_page_size(level);
    const enum paging_memory_type memory_type =
        leaf_memory_type(entry, level, pat);
    uint32_t permissions = PAGING_READ;

    if (writable) {
        permissions |= PAGING_WRITE;
    }

    if (executable) {
        permissions |= PAGING_EXECUTE;
    }

    if (memory_type == PAGING_MEMORY_UNCACHEABLE) {
        permissions |= PAGING_UNCACHED;
    } else if (memory_type == PAGING_MEMORY_WRITE_COMBINING) {
        permissions |= PAGING_WRITE_COMBINING;
    }

    translation->physical_address =
        (entry & PAGE_FRAME_MASK & ~(page_size - 1U)) |
        (virtual_address & (page_size - 1U));
    translation->permissions = permissions;
    translation->memory_type = memory_type;
    translation->level = level;
    translation->user = user;
}

/*
 * Resolve one address the way the processor would, accumulating the conjunction
 * of write and execute permission down the path rather than reporting the
 * leaf's own bits. Reporting only the leaf would describe a mapping that does
 * not exist.
 */
static enum paging_status translate_address(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    struct paging_translation *translation,
    uint64_t pat
)
{
    uint64_t table = hierarchy->root;
    uint64_t entry;
    bool writable = true;
    bool executable = true;
    bool user = true;

    translation->physical_address = 0U;
    translation->permissions = PAGING_READ;
    translation->memory_type = PAGING_MEMORY_INVALID;
    translation->level = 0U;
    translation->user = false;

    if (!address_is_canonical(virtual_address)) {
        return PAGING_STATUS_NONCANONICAL_ADDRESS;
    }

    for (unsigned int level = PAGING_LEVEL_COUNT; level > 1U; --level) {
        entry = table_at(table)[table_index(virtual_address, level)];

        if ((entry & PAGE_PRESENT) == 0U) {
            return PAGING_STATUS_NOT_MAPPED;
        }

        writable = writable && (entry & PAGE_WRITABLE) != 0U;
        executable = executable && (entry & PAGE_NO_EXECUTE) == 0U;
        user = user && (entry & PAGE_USER) != 0U;

        if ((entry & PAGE_HUGE) != 0U) {
            fill_translation(translation, entry, virtual_address, level,
                writable, executable, user, pat);
            return PAGING_STATUS_OK;
        }

        table = entry & PAGE_FRAME_MASK;
    }

    entry = table_at(table)[table_index(virtual_address, 1U)];

    if ((entry & PAGE_PRESENT) == 0U) {
        return PAGING_STATUS_NOT_MAPPED;
    }

    writable = writable && (entry & PAGE_WRITABLE) != 0U;
    executable = executable && (entry & PAGE_NO_EXECUTE) == 0U;
    user = user && (entry & PAGE_USER) != 0U;
    fill_translation(translation, entry, virtual_address, 1U, writable,
        executable, user, pat);
    return PAGING_STATUS_OK;
}

/*
 * Visit every present leaf and classify it by its effective permissions. The
 * recursion is bounded by construction: level only ever decreases and starts at
 * PAGING_LEVEL_COUNT, so at most four frames are live at once.
 */
static void audit_table(
    uint64_t table,
    unsigned int level,
    bool writable,
    bool executable,
    bool user,
    struct paging_audit *audit
)
{
    const uint64_t *entries = table_at(table);

    for (size_t index = 0; index < PAGING_ENTRIES_PER_TABLE; ++index) {
        const uint64_t entry = entries[index];
        bool leaf_writable;
        bool leaf_executable;
        bool leaf_user;

        if ((entry & PAGE_PRESENT) == 0U) {
            continue;
        }

        leaf_writable = writable && (entry & PAGE_WRITABLE) != 0U;
        leaf_executable = executable && (entry & PAGE_NO_EXECUTE) == 0U;
        leaf_user = user && (entry & PAGE_USER) != 0U;

        if (level > 1U && (entry & PAGE_HUGE) == 0U) {
            audit_table(entry & PAGE_FRAME_MASK, level - 1U, leaf_writable,
                leaf_executable, leaf_user, audit);
            continue;
        }

        ++audit->leaf_count;

        if (level > 1U) {
            ++audit->huge_leaves;
        }

        if (leaf_writable) {
            ++audit->writable_leaves;
        }

        if (leaf_executable) {
            ++audit->executable_leaves;
        }

        if (leaf_writable && leaf_executable) {
            ++audit->write_execute_leaves;
        }

        if (leaf_user) {
            ++audit->user_leaves;
        }
    }
}

static void audit_hierarchy(
    struct page_hierarchy *hierarchy,
    struct paging_audit *audit
)
{
    audit->leaf_count = 0U;
    audit->huge_leaves = 0U;
    audit->writable_leaves = 0U;
    audit->executable_leaves = 0U;
    audit->write_execute_leaves = 0U;
    audit->user_leaves = 0U;
    audit_table(hierarchy->root, PAGING_LEVEL_COUNT, true, true, true, audit);
}

/*
 * Refuse a processor this hierarchy would be a lie on. Kept pure so every
 * rejection is driven by a synthetic value in paging_self_test rather than by a
 * machine nobody can arrange.
 */
static enum paging_status decode_processor_support(
    uint32_t basic_features_edx,
    uint32_t extended_root_eax,
    uint32_t extended_features_edx,
    uint64_t cr4,
    uint64_t pat
)
{
    if (extended_root_eax < CPUID_EXTENDED_FEATURES ||
        (extended_features_edx & CPUID_NO_EXECUTE) == 0U) {
        return PAGING_STATUS_NO_EXECUTE_UNSUPPORTED;
    }

    if ((basic_features_edx & CPUID_PAT) == 0U) {
        return PAGING_STATUS_PAT_UNSUPPORTED;
    }

    if ((cr4 & CR4_FIVE_LEVEL_PAGING) != 0U) {
        return PAGING_STATUS_FIVE_LEVEL_PAGING;
    }

    if ((cr4 & CR4_PHYSICAL_ADDRESS_EXTENSION) == 0U) {
        return PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED;
    }

    for (unsigned int index = 0U; index < 8U; ++index) {
        if (decode_pat_type(pat_entry(pat, index)) == PAGING_MEMORY_INVALID) {
            return PAGING_STATUS_PAT_LAYOUT_UNSAFE;
        }
    }

    if (pat_entry(pat, PAT_WRITE_BACK_ENTRY) != PAT_TYPE_WRITE_BACK ||
        pat_entry(pat, PAT_DEVICE_ENTRY) != PAT_TYPE_UNCACHEABLE) {
        return PAGING_STATUS_PAT_LAYOUT_UNSAFE;
    }

    return PAGING_STATUS_OK;
}

static enum paging_status validate_kernel_layout(
    uint64_t kernel_start,
    uint64_t kernel_end
)
{
    if (kernel_start >= kernel_end ||
        (kernel_start & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (kernel_end & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        kernel_end > PAGING_KERNEL_IMAGE_LIMIT) {
        return PAGING_STATUS_BAD_KERNEL_LAYOUT;
    }

    return PAGING_STATUS_OK;
}

static void add_section(
    struct paging_section *sections,
    size_t *count,
    uint64_t start,
    uint64_t end,
    uint32_t permissions
)
{
    if (start >= end) {
        return;
    }

    sections[*count].start = start;
    sections[*count].end = end;
    sections[*count].permissions = permissions;
    ++*count;
}

static void collect_kernel_sections(
    struct paging_section *sections,
    size_t *count
)
{
    const uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t text_start = (uint64_t)(uintptr_t)__text_start;

    *count = 0U;

    /* The Multiboot2 header: read by the boot loader, never by the kernel. */
    add_section(sections, count, kernel_start, text_start, PAGING_READ);
    add_section(sections, count, text_start, (uint64_t)(uintptr_t)__text_end,
        PAGING_EXECUTE);
    add_section(sections, count, (uint64_t)(uintptr_t)__rodata_start,
        (uint64_t)(uintptr_t)__rodata_end, PAGING_READ);
    add_section(sections, count, (uint64_t)(uintptr_t)__data_start,
        (uint64_t)(uintptr_t)__kernel_end, PAGING_WRITE);
}

static const struct paging_device_window *device_window_at(
    const struct paging_device_windows *windows,
    uint64_t address
)
{
    for (size_t index = 0U; index < windows->count; ++index) {
        const struct paging_device_window *window = &windows->entries[index];

        if (address >= window->physical_base &&
            address < window->physical_base + window->length) {
            return window;
        }
    }

    return NULL;
}

static bool region_needs_page_table(
    const struct paging_device_windows *windows,
    uint64_t base,
    uint64_t kernel_start,
    uint64_t kernel_end
)
{
    const uint64_t end = base + PAGING_HUGE_PAGE_SIZE;

    if (base < kernel_end && kernel_start < end) {
        return true;
    }

    for (size_t index = 0U; index < windows->count; ++index) {
        const struct paging_device_window *window = &windows->entries[index];

        if (base < window->physical_base + window->length &&
            window->physical_base < end) {
            return true;
        }
    }

    return false;
}

static uint32_t device_window_mapping_permissions(
    const struct paging_device_window *window
)
{
    uint32_t permissions = PAGING_READ;

    if ((window->permissions & PAGING_DEVICE_WINDOW_WRITE) != 0U) {
        permissions |= PAGING_WRITE;
    }

    if (window->memory_type == PAGING_MEMORY_UNCACHEABLE) {
        permissions |= PAGING_UNCACHED;
    } else if (window->memory_type == PAGING_MEMORY_WRITE_COMBINING) {
        permissions |= PAGING_WRITE_COMBINING;
    }

    return permissions;
}

/*
 * Anything inside a fine region that is not a named section is ordinary memory:
 * writable and never executable. That default is what makes the build
 * order-independent - a table frame the allocator hands out mid-build lands in
 * this case whether its own entry has been written yet or not.
 */
static uint32_t section_permissions(
    const struct paging_section *sections,
    size_t count,
    const struct paging_device_windows *windows,
    uint64_t address
)
{
    const struct paging_device_window *window =
        device_window_at(windows, address);

    if (window != NULL) {
        return device_window_mapping_permissions(window);
    }

    for (size_t index = 0; index < count; ++index) {
        if (address >= sections[index].start && address < sections[index].end) {
            return sections[index].permissions;
        }
    }

    return PAGING_WRITE;
}

static enum paging_status build_identity_map(
    struct page_hierarchy *hierarchy,
    const struct paging_device_windows *windows
)
{
    const uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;

    fine_region_count = 0U;

    for (uint64_t base = 0U; base < SAPOTE_EARLY_PHYSICAL_LIMIT;
         base += PAGING_HUGE_PAGE_SIZE) {
        enum paging_status status;

        if (!region_needs_page_table(windows, base, kernel_start, kernel_end)) {
            status = map_range(hierarchy, base, base, PAGING_HUGE_PAGE_SIZE,
                PAGING_WRITE, 2U);

            if (status != PAGING_STATUS_OK) {
                return status;
            }

            continue;
        }

        ++fine_region_count;

        for (uint64_t offset = 0U; offset < PAGING_HUGE_PAGE_SIZE;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = base + offset;

            /*
             * The null page stays absent. Nothing in Sapote reads physical
             * address zero, and leaving it unmapped turns a null dereference
             * into a page fault naming CR2 = 0 instead of a silent read of the
             * real-mode interrupt vector table.
             */
            if (address == 0U) {
                continue;
            }

            status = map_range(hierarchy, address, address, PAGING_PAGE_SIZE,
                section_permissions(kernel_sections, kernel_section_count,
                    windows, address), 1U);

            if (status != PAGING_STATUS_OK) {
                return status;
            }
        }
    }

    return PAGING_STATUS_OK;
}

/*
 * Read the finished hierarchy back and compare it against the intent, one page
 * at a time, before CR3 is allowed to point at it. Building a table and then
 * trusting it is how a kernel discovers its mistake as a triple fault with
 * nothing on the serial line.
 */
static enum paging_status validate_identity_map(
    struct page_hierarchy *hierarchy,
    const struct paging_device_windows *windows,
    uint64_t pat
)
{
    const uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;
    struct paging_translation translation;

    for (uint64_t base = 0U; base < SAPOTE_EARLY_PHYSICAL_LIMIT;
         base += PAGING_HUGE_PAGE_SIZE) {
        const bool fine = region_needs_page_table(windows, base, kernel_start,
            kernel_end);

        if (!fine) {
            if (translate_address(hierarchy, base, &translation, pat) !=
                    PAGING_STATUS_OK ||
                translation.physical_address != base ||
                translation.permissions != PAGING_WRITE ||
                translation.memory_type != PAGING_MEMORY_WRITE_BACK ||
                translation.level != 2U) {
                return PAGING_STATUS_VALIDATION_FAILURE;
            }

            continue;
        }

        for (uint64_t offset = 0U; offset < PAGING_HUGE_PAGE_SIZE;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = base + offset;
            const struct paging_device_window *window =
                device_window_at(windows, address);
            uint32_t expected;

            if (address == 0U) {
                if (translate_address(hierarchy, address, &translation, pat) !=
                    PAGING_STATUS_NOT_MAPPED) {
                    return PAGING_STATUS_VALIDATION_FAILURE;
                }

                continue;
            }

            expected = section_permissions(kernel_sections,
                kernel_section_count, windows, address);

            if (translate_address(hierarchy, address, &translation, pat) !=
                    PAGING_STATUS_OK ||
                translation.physical_address != address ||
                translation.permissions != expected ||
                (window != NULL &&
                    translation.memory_type != window->memory_type) ||
                (window == NULL &&
                    translation.memory_type != PAGING_MEMORY_WRITE_BACK) ||
                translation.level != 1U) {
                return PAGING_STATUS_VALIDATION_FAILURE;
            }
        }
    }

    return PAGING_STATUS_OK;
}

static size_t supervisor_intent_find(uint64_t address, uint64_t length)
{
    for (size_t index = 0U; index < supervisor_intent_count; ++index) {
        if (supervisor_intents[index].virtual_address == address &&
            supervisor_intents[index].length == length) {
            return index;
        }
    }
    return SIZE_MAX;
}

static enum paging_status supervisor_intent_add(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t length,
    uint32_t permissions
)
{
    if (supervisor_intent_count == PAGING_SUPERVISOR_INTENT_CAPACITY) {
        return PAGING_STATUS_SUPERVISOR_INTENT_FULL;
    }
    supervisor_intents[supervisor_intent_count].virtual_address =
        virtual_address;
    supervisor_intents[supervisor_intent_count].physical_address =
        physical_address;
    supervisor_intents[supervisor_intent_count].length = length;
    supervisor_intents[supervisor_intent_count].permissions = permissions;
    ++supervisor_intent_count;
    return PAGING_STATUS_OK;
}

static void supervisor_intent_remove(size_t index)
{
    if (index >= supervisor_intent_count) {
        return;
    }
    for (size_t move = index + 1U; move < supervisor_intent_count; ++move) {
        supervisor_intents[move - 1U] = supervisor_intents[move];
    }
    --supervisor_intent_count;
}

static enum paging_status replay_supervisor_intent(
    struct page_hierarchy *hierarchy
)
{
    for (size_t index = 0U; index < supervisor_intent_count; ++index) {
        const struct supervisor_mapping_intent *intent =
            &supervisor_intents[index];
        enum paging_status status = map_range(hierarchy,
            intent->virtual_address, intent->physical_address, intent->length,
            intent->permissions, 1U);

        if (status != PAGING_STATUS_OK) {
            return status;
        }
    }
    return PAGING_STATUS_OK;
}

static void reset_state(void)
{
    state.root_physical_address = 0U;
    state.table_frames = 0U;
    state.fine_regions = 0U;
    state.pat_before = 0U;
    state.pat_after = 0U;
    state.write_combining_pat_entry = 0U;
    state.no_execute_active = false;
    state.write_protect_active = false;
    state.active = false;
    installed_device_windows.count = 0U;
    supervisor_intent_count = 0U;
}

static enum paging_status reject_kernel_device_overlap(
    const struct paging_device_windows *windows
)
{
    const uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t kernel_end = (uint64_t)(uintptr_t)__kernel_end;

    for (size_t index = 0U; index < windows->count; ++index) {
        const struct paging_device_window *window = &windows->entries[index];

        if (window->physical_base < kernel_end &&
            kernel_start < window->physical_base + window->length) {
            return PAGING_STATUS_DEVICE_WINDOW_KERNEL_OVERLAP;
        }
    }

    return PAGING_STATUS_OK;
}

static enum paging_status enable_processor_features(void)
{
    const uint64_t efer = cpu_read_msr(IA32_EFER_MSR);

    /*
     * Both bits are turned on while the boot hierarchy still marks every page
     * writable and executable, so neither can revoke a permission from an
     * instruction already in flight. Each is read back rather than assumed: a
     * write that did not take is exactly the case where the kernel would go on
     * to claim a guarantee it cannot enforce.
     */
    cpu_write_msr(IA32_EFER_MSR, efer | EFER_NO_EXECUTE_ENABLE);

    if ((cpu_read_msr(IA32_EFER_MSR) & EFER_NO_EXECUTE_ENABLE) == 0U) {
        return PAGING_STATUS_NO_EXECUTE_INACTIVE;
    }

    cpu_write_cr0(cpu_read_cr0() | CR0_WRITE_PROTECT);

    if ((cpu_read_cr0() & CR0_WRITE_PROTECT) == 0U) {
        return PAGING_STATUS_WRITE_PROTECT_INACTIVE;
    }

    return PAGING_STATUS_OK;
}

enum paging_status paging_initialize(const struct paging_device_windows *windows)
{
    struct paging_device_windows validated_windows;
    struct cpuid_result basic;
    struct cpuid_result extended;
    struct paging_audit audit;
    uint32_t extended_features_edx = 0U;
    uint32_t extended_root_eax;
    uint64_t previous_root;
    uint64_t pat_before = 0U;
    uint64_t pat_after;
    uint64_t root = 0U;
    enum paging_status status;

    if (windows == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    if (state.active) {
        return PAGING_STATUS_ALREADY_INITIALIZED;
    }

    status = paging_device_windows_validate(windows, &validated_windows);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    status = validate_kernel_layout((uint64_t)(uintptr_t)__kernel_start,
        (uint64_t)(uintptr_t)__kernel_end);

    if (status == PAGING_STATUS_OK) {
        status = reject_kernel_device_overlap(&validated_windows);
    }

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    /*
     * A page fault taken between the CR3 load and the validation that follows
     * it has nowhere to report from, so nothing may interrupt the switch.
     */
    if (cpu_interrupts_enabled()) {
        return PAGING_STATUS_INTERRUPTS_ENABLED;
    }

    cpu_cpuid(CPUID_BASIC_FEATURES, 0U, &basic);
    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &extended);
    extended_root_eax = extended.eax;

    if (extended_root_eax >= CPUID_EXTENDED_FEATURES) {
        cpu_cpuid(CPUID_EXTENDED_FEATURES, 0U, &extended);
        extended_features_edx = extended.edx;
    }

    /* RDMSR itself is unsafe until CPUID has established that IA32_PAT exists. */
    if ((basic.edx & CPUID_PAT) != 0U) {
        pat_before = cpu_read_msr(IA32_PAT_MSR);
    }

    status = decode_processor_support(basic.edx, extended_root_eax,
        extended_features_edx, cpu_read_cr4(), pat_before);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    pat_after = pat_replace_entry(pat_before, PAT_WRITE_COMBINING_ENTRY,
        PAT_TYPE_WRITE_COMBINING);

    status = enable_processor_features();

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    collect_kernel_sections(kernel_sections, &kernel_section_count);

    live_hierarchy.arena_base = 0U;
    live_hierarchy.arena_capacity = 0U;
    live_hierarchy.arena_used = 0U;
    live_hierarchy.table_frames = 0U;
    live_hierarchy.live = false;
    status = allocate_table(&live_hierarchy, &root);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    live_hierarchy.root = root;
    status = build_identity_map(&live_hierarchy, &validated_windows);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    status = validate_identity_map(&live_hierarchy, &validated_windows,
        pat_after);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    /*
     * The W^X walk runs before the switch as well as after it. Installing a
     * hierarchy that violates the invariant and only then noticing would mean
     * the machine had already executed on it.
     */
    audit_hierarchy(&live_hierarchy, &audit);

    if (audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        reset_state();
        return PAGING_STATUS_VALIDATION_FAILURE;
    }

    /*
     * Entry 1 is unused by the bootstrap mapping, so changing it cannot retype
     * an active translation. Program it only after the inactive hierarchy has
     * passed its complete walk, and require an exact readback before CR3 can
     * select it for framebuffer leaves.
     */
    cpu_write_msr(IA32_PAT_MSR, pat_after);

    if (cpu_read_msr(IA32_PAT_MSR) != pat_after) {
        cpu_write_msr(IA32_PAT_MSR, pat_before);
        reset_state();
        return PAGING_STATUS_PAT_READBACK_MISMATCH;
    }

    previous_root = cpu_read_cr3();
    state.root_physical_address = live_hierarchy.root;
    state.table_frames = live_hierarchy.table_frames;
    state.fine_regions = fine_region_count;
    state.pat_before = pat_before;
    state.pat_after = pat_after;
    state.write_combining_pat_entry = PAT_WRITE_COMBINING_ENTRY;
    state.no_execute_active = true;
    state.write_protect_active = true;
    state.active = true;
    installed_device_windows = validated_windows;
    live_hierarchy.live = true;

    /*
     * The bootstrap hierarchy may have named the framebuffer with a different
     * type. Drain every old cache line before CR3 changes that physical span's
     * type; the second WBINVD completes the conservative transition sequence.
     */
    cpu_write_back_and_invalidate_cache();
    cpu_write_cr3(live_hierarchy.root);
    cpu_write_back_and_invalidate_cache();
    status = paging_verify();

    if (status != PAGING_STATUS_OK) {
        /*
         * The boot hierarchy is still intact and still maps everything, so
         * going back to it is the one recovery available. Reporting a status
         * from the old address space beats halting inside the new one.
         */
        cpu_write_back_and_invalidate_cache();
        cpu_write_cr3(previous_root);
        cpu_write_back_and_invalidate_cache();
        cpu_write_msr(IA32_PAT_MSR, pat_before);
        live_hierarchy.live = false;
        reset_state();
        return status;
    }

    return PAGING_STATUS_OK;
}

static void zero_process_space(struct paging_process_space *space)
{
    space->root_physical_address = 0U;
    space->generation = 0U;
    space->table_frames = 0U;
    space->state = PAGING_PROCESS_SPACE_INVALID;
}

static void zero_process_alias(struct paging_process_image_alias *alias)
{
    alias->physical_address = 0U;
    alias->generation = 0U;
    alias->active = false;
}

static void zero_process_alias_set(struct paging_process_alias_set *alias)
{
    for (size_t index = 0U; index < PAGING_PROCESS_ALIAS_MAX_PAGES; ++index) {
        alias->physical_addresses[index] = 0U;
    }
    alias->generation = 0U;
    alias->count = 0U;
    alias->active = false;
}

static bool process_space_matches(const struct paging_process_space *space)
{
    return space != NULL && process_space_runtime.owned &&
        space->root_physical_address == process_space_runtime.hierarchy.root &&
        space->generation == process_space_runtime.generation &&
        space->state == process_space_runtime.state;
}

static void sync_process_space(struct paging_process_space *space)
{
    space->root_physical_address = process_space_runtime.hierarchy.root;
    space->generation = process_space_runtime.generation;
    space->table_frames = process_space_runtime.hierarchy.table_frames;
    space->state = process_space_runtime.state;
}

static void release_table_tree(
    struct page_hierarchy *hierarchy,
    uint64_t table,
    unsigned int level
)
{
    if (level > 1U) {
        const uint64_t *entries = table_at(table);

        for (size_t index = 0U; index < PAGING_ENTRIES_PER_TABLE; ++index) {
            const uint64_t entry = entries[index];

            if ((entry & PAGE_PRESENT) != 0U &&
                (entry & PAGE_HUGE) == 0U) {
                release_table_tree(hierarchy, entry & PAGE_FRAME_MASK,
                    level - 1U);
            }
        }
    }
    release_table(hierarchy, table);
}

static enum paging_status entry_for_user(
    struct page_hierarchy *hierarchy,
    uint64_t virtual_address,
    uint64_t **entry
)
{
    uint64_t table = hierarchy->root;

    for (unsigned int level = PAGING_LEVEL_COUNT; level > 1U; --level) {
        uint64_t *slot = &table_at(table)[table_index(virtual_address, level)];

        if ((*slot & PAGE_PRESENT) == 0U) {
            uint64_t next = 0U;
            const enum paging_status status = allocate_table(hierarchy, &next);

            if (status != PAGING_STATUS_OK) {
                return status;
            }
            *slot = next | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
            table = next;
            continue;
        }
        if ((*slot & PAGE_HUGE) != 0U || (*slot & PAGE_USER) == 0U) {
            return PAGING_STATUS_PROCESS_BAD_MAPPING;
        }
        table = *slot & PAGE_FRAME_MASK;
    }
    *entry = &table_at(table)[table_index(virtual_address, 1U)];
    return PAGING_STATUS_OK;
}

static bool process_mapping_request_valid(
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address,
    uint32_t permissions
)
{
    if (kind == PAGING_PROCESS_MAPPING_IMAGE) {
        return virtual_address == PAGING_PROCESS_IMAGE_ADDRESS &&
            permissions == PAGING_EXECUTE;
    }
    if (kind == PAGING_PROCESS_MAPPING_STACK) {
        return virtual_address >= PAGING_PROCESS_STACK_BASE &&
            virtual_address < PAGING_PROCESS_STACK_END &&
            (virtual_address & (PAGING_PAGE_SIZE - 1U)) == 0U &&
            permissions == PAGING_WRITE;
    }
    if (kind == PAGING_PROCESS_MAPPING_LINUX_IMAGE) {
        if (virtual_address < PAGING_LINUX_IMAGE_BASE ||
            virtual_address >= PAGING_LINUX_IMAGE_END ||
            (virtual_address & (PAGING_PAGE_SIZE - 1U)) != 0U) {
            return false;
        }
        const size_t page = (size_t)((virtual_address -
            PAGING_LINUX_IMAGE_BASE) / PAGING_PAGE_SIZE);

        if (page == 0U || page == 7U) {
            return permissions == PAGING_READ;
        }
        if (page >= 1U && page <= 6U) {
            return permissions == PAGING_EXECUTE;
        }
        return page == 8U && permissions == PAGING_WRITE;
    }
    if (kind == PAGING_PROCESS_MAPPING_LINUX_STACK) {
        return virtual_address >= PAGING_LINUX_STACK_BASE &&
            virtual_address < PAGING_LINUX_STACK_END &&
            (virtual_address & (PAGING_PAGE_SIZE - 1U)) == 0U &&
            permissions == PAGING_WRITE;
    }
    if (kind == PAGING_PROCESS_MAPPING_LINUX_HEAP) {
        return virtual_address >= PAGING_LINUX_HEAP_BASE &&
            virtual_address < PAGING_LINUX_HEAP_BASE +
                PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE &&
            (virtual_address & (PAGING_PAGE_SIZE - 1U)) == 0U &&
            permissions == PAGING_WRITE;
    }
    if (kind == PAGING_PROCESS_MAPPING_LINUX_ANON) {
        return virtual_address == PAGING_LINUX_ANON_ADDRESS &&
            permissions == PAGING_WRITE;
    }
    return false;
}

static bool process_alias_contains(uint64_t physical_address)
{
    if (!process_alias_runtime.owned) {
        return false;
    }
    for (size_t index = 0U; index < process_alias_runtime.count; ++index) {
        if (process_alias_runtime.pages[index].physical_address ==
                physical_address) {
            return true;
        }
    }
    return false;
}

static bool process_unmap_request_valid(
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address
)
{
    return process_mapping_request_valid(kind, virtual_address, PAGING_READ) ||
        process_mapping_request_valid(kind, virtual_address, PAGING_WRITE) ||
        process_mapping_request_valid(kind, virtual_address, PAGING_EXECUTE);
}

static bool no_user_bits_in_table(uint64_t table, unsigned int level)
{
    const uint64_t *entries = table_at(table);

    for (size_t index = 0U; index < PAGING_ENTRIES_PER_TABLE; ++index) {
        const uint64_t entry = entries[index];

        if ((entry & PAGE_PRESENT) == 0U) {
            continue;
        }
        if ((entry & PAGE_USER) != 0U) {
            return false;
        }
        if (level > 1U && (entry & PAGE_HUGE) == 0U &&
            !no_user_bits_in_table(entry & PAGE_FRAME_MASK, level - 1U)) {
            return false;
        }
    }
    return true;
}

static bool no_user_bits_in_kernel_branch(const struct page_hierarchy *hierarchy)
{
    const uint64_t entry = table_at(hierarchy->root)[0];

    return (entry & PAGE_PRESENT) != 0U && (entry & PAGE_HUGE) == 0U &&
        (entry & PAGE_USER) == 0U &&
        no_user_bits_in_table(entry & PAGE_FRAME_MASK,
            PAGING_LEVEL_COUNT - 1U);
}

static void flush_hierarchy(const struct page_hierarchy *hierarchy)
{
    if (hierarchy->live) {
        cpu_write_cr3(hierarchy->root);
    }
}

static enum paging_status narrow_identity_alias(
    struct page_hierarchy *hierarchy,
    uint64_t physical_address,
    uint64_t *saved_entry,
    uint64_t *split_table,
    bool *split
)
{
    struct paging_translation before;
    struct paging_translation after;
    uint64_t *directory_entry = NULL;
    uint64_t *leaf = NULL;
    enum paging_status status;

    *saved_entry = 0U;
    *split_table = 0U;
    *split = false;
    status = translate_address(hierarchy, physical_address, &before,
        state.pat_after);
    if (status != PAGING_STATUS_OK ||
        before.physical_address != physical_address ||
        before.permissions != PAGING_WRITE || before.user ||
        before.memory_type != PAGING_MEMORY_WRITE_BACK) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    status = entry_for(hierarchy, physical_address, 2U, false,
        &directory_entry);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    if ((*directory_entry & PAGE_PRESENT) != 0U &&
        (*directory_entry & PAGE_HUGE) != 0U) {
        const uint64_t original = *directory_entry;
        const uint64_t base = original & PAGE_FRAME_MASK &
            ~(PAGING_HUGE_PAGE_SIZE - 1U);
        const uint64_t leaf_flags = original &
            (PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER | PAGE_WRITE_THROUGH |
                PAGE_CACHE_DISABLE | PAGE_ACCESSED | PAGE_DIRTY |
                PAGE_GLOBAL | PAGE_NO_EXECUTE);
        uint64_t table = 0U;

        status = allocate_table(hierarchy, &table);
        if (status != PAGING_STATUS_OK) {
            return status;
        }
        for (size_t index = 0U; index < PAGING_ENTRIES_PER_TABLE; ++index) {
            table_at(table)[index] =
                (base + index * PAGING_PAGE_SIZE) | leaf_flags;
        }
        *saved_entry = original;
        *split_table = table;
        *split = true;
        *directory_entry = table | PAGE_PRESENT | PAGE_WRITABLE |
            (original & PAGE_USER);
        invalidate(hierarchy, physical_address);
        flush_hierarchy(hierarchy);
    }
    status = entry_for(hierarchy, physical_address, 1U, false, &leaf);
    if (status != PAGING_STATUS_OK || (*leaf & PAGE_PRESENT) == 0U) {
        return status == PAGING_STATUS_OK ?
            PAGING_STATUS_PROCESS_ALIAS_STATE : status;
    }
    if (!*split) {
        *saved_entry = *leaf;
    }
    *leaf = (*leaf & ~PAGE_WRITABLE) | PAGE_NO_EXECUTE;
    invalidate(hierarchy, physical_address);
    flush_hierarchy(hierarchy);
    status = translate_address(hierarchy, physical_address, &after,
        state.pat_after);
    if (status != PAGING_STATUS_OK || after.physical_address != physical_address ||
        after.permissions != PAGING_READ || after.user ||
        after.memory_type != PAGING_MEMORY_WRITE_BACK) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    return PAGING_STATUS_OK;
}

static enum paging_status restore_identity_alias(
    struct page_hierarchy *hierarchy,
    uint64_t physical_address,
    uint64_t saved_entry,
    uint64_t split_table,
    bool split
)
{
    uint64_t *entry = NULL;
    enum paging_status status;

    status = entry_for(hierarchy, physical_address, split ? 2U : 1U, false,
        &entry);
    if (status != PAGING_STATUS_OK || (*entry & PAGE_PRESENT) == 0U) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    if (split && ((*entry & PAGE_HUGE) != 0U ||
        (*entry & PAGE_FRAME_MASK) != split_table)) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    *entry = saved_entry;
    invalidate(hierarchy, physical_address);
    flush_hierarchy(hierarchy);
    if (split) {
        release_table(hierarchy, split_table);
    }
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_space_build(
    struct paging_process_space *space
)
{
    struct paging_audit audit;
    uint64_t root = 0U;
    enum paging_status status;

    if (space == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    zero_process_space(space);
    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }
    if (process_space_runtime.owned || process_alias_runtime.owned) {
        return PAGING_STATUS_PROCESS_BUSY;
    }
    if ((cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    process_space_runtime.hierarchy.root = 0U;
    process_space_runtime.hierarchy.arena_base = 0U;
    process_space_runtime.hierarchy.arena_capacity = 0U;
    process_space_runtime.hierarchy.arena_used = 0U;
    process_space_runtime.hierarchy.table_frames = 0U;
    process_space_runtime.hierarchy.live = false;
    status = allocate_table(&process_space_runtime.hierarchy, &root);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    process_space_runtime.hierarchy.root = root;
    process_space_runtime.owned = true;
    process_space_runtime.generation = next_process_generation++;
    if (next_process_generation == 0U) {
        next_process_generation = 1U;
    }
    process_space_runtime.state = PAGING_PROCESS_SPACE_BUILDING;
    status = build_identity_map(&process_space_runtime.hierarchy,
        &installed_device_windows);
    if (status == PAGING_STATUS_OK) {
        status = validate_identity_map(&process_space_runtime.hierarchy,
            &installed_device_windows, state.pat_after);
    }
    if (status == PAGING_STATUS_OK) {
        status = replay_supervisor_intent(&process_space_runtime.hierarchy);
    }
    if (status == PAGING_STATUS_OK) {
        audit_hierarchy(&process_space_runtime.hierarchy, &audit);
        if (audit.write_execute_leaves != 0U || audit.user_leaves != 0U ||
            !no_user_bits_in_table(process_space_runtime.hierarchy.root,
                PAGING_LEVEL_COUNT)) {
            status = PAGING_STATUS_VALIDATION_FAILURE;
        }
    }
    if (status != PAGING_STATUS_OK) {
        release_table_tree(&process_space_runtime.hierarchy,
            process_space_runtime.hierarchy.root, PAGING_LEVEL_COUNT);
        process_space_runtime.owned = false;
        process_space_runtime.state = PAGING_PROCESS_SPACE_INVALID;
        return status;
    }
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

static enum paging_status restore_alias_page(
    struct process_alias_page_runtime *page,
    bool private_hierarchy
)
{
    if (private_hierarchy) {
        if (page->private_saved_entry == 0U) {
            return PAGING_STATUS_OK;
        }
        return restore_identity_alias(&process_space_runtime.hierarchy,
            page->physical_address, page->private_saved_entry,
            page->private_split_table, page->private_split);
    }
    if (page->saved_entry == 0U) {
        return PAGING_STATUS_OK;
    }
    return restore_identity_alias(&live_hierarchy, page->physical_address,
        page->saved_entry, page->split_table, page->split);
}

static enum paging_status rollback_alias_pages(size_t count)
{
    enum paging_status result = PAGING_STATUS_OK;

    for (size_t remaining = count; remaining > 0U; --remaining) {
        if (restore_alias_page(&process_alias_runtime.pages[remaining - 1U],
                true) != PAGING_STATUS_OK) {
            result = PAGING_STATUS_PROCESS_ALIAS_STATE;
        }
    }
    for (size_t remaining = count; remaining > 0U; --remaining) {
        if (restore_alias_page(&process_alias_runtime.pages[remaining - 1U],
                false) != PAGING_STATUS_OK) {
            result = PAGING_STATUS_PROCESS_ALIAS_STATE;
        }
    }
    state.table_frames = live_hierarchy.table_frames;
    return result;
}

static enum paging_status narrow_alias_pages(
    const struct paging_process_space *space,
    const uint64_t *physical_addresses,
    size_t count
)
{
    enum paging_status status;

    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if (physical_addresses == NULL || count == 0U ||
        count > PAGING_PROCESS_ALIAS_MAX_PAGES) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    if (process_space_runtime.state != PAGING_PROCESS_SPACE_BUILDING ||
        process_alias_runtime.owned || cpu_interrupts_enabled() ||
        (cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    process_alias_runtime.count = 0U;
    for (size_t index = 0U; index < count; ++index) {
        const uint64_t physical_address = physical_addresses[index];

        if ((physical_address & (PAGING_PAGE_SIZE - 1U)) != 0U ||
            !frame_range_overlaps_allocatable_memory(physical_address,
                PAGING_PAGE_SIZE)) {
            return PAGING_STATUS_PROCESS_ALIAS_STATE;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (physical_addresses[prior] == physical_address) {
                return PAGING_STATUS_PROCESS_ALIAS_STATE;
            }
        }
    }
    for (size_t index = 0U; index < count; ++index) {
        struct process_alias_page_runtime *page =
            &process_alias_runtime.pages[index];

        page->physical_address = physical_addresses[index];
        page->saved_entry = 0U;
        page->split_table = 0U;
        page->private_saved_entry = 0U;
        page->private_split_table = 0U;
        page->split = false;
        page->private_split = false;
        status = narrow_identity_alias(&live_hierarchy,
            page->physical_address, &page->saved_entry, &page->split_table,
            &page->split);
        if (status != PAGING_STATUS_OK) {
            (void)restore_alias_page(page, false);
            if (rollback_alias_pages(index) != PAGING_STATUS_OK) {
                return PAGING_STATUS_PROCESS_ALIAS_STATE;
            }
            return status;
        }
        state.table_frames = live_hierarchy.table_frames;
        status = narrow_identity_alias(&process_space_runtime.hierarchy,
            page->physical_address, &page->private_saved_entry,
            &page->private_split_table, &page->private_split);
        if (status != PAGING_STATUS_OK) {
            (void)restore_alias_page(page, true);
            (void)restore_alias_page(page, false);
            if (rollback_alias_pages(index) != PAGING_STATUS_OK) {
                return PAGING_STATUS_PROCESS_ALIAS_STATE;
            }
            state.table_frames = live_hierarchy.table_frames;
            return status;
        }
        process_alias_runtime.count = index + 1U;
    }
    process_alias_runtime.generation = next_alias_generation++;
    if (next_alias_generation == 0U) {
        next_alias_generation = 1U;
    }
    process_alias_runtime.owned = true;
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_image_alias_narrow(
    const struct paging_process_space *space,
    uint64_t physical_address,
    struct paging_process_image_alias *alias
)
{
    enum paging_status status;

    if (alias == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    zero_process_alias(alias);
    status = narrow_alias_pages(space, &physical_address, 1U);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    alias->physical_address = physical_address;
    alias->generation = process_alias_runtime.generation;
    alias->active = true;
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_alias_set_narrow(
    const struct paging_process_space *space,
    const uint64_t *physical_addresses,
    size_t count,
    struct paging_process_alias_set *alias
)
{
    enum paging_status status;

    if (alias == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    zero_process_alias_set(alias);
    status = narrow_alias_pages(space, physical_addresses, count);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    for (size_t index = 0U; index < count; ++index) {
        alias->physical_addresses[index] = physical_addresses[index];
    }
    alias->generation = process_alias_runtime.generation;
    alias->count = count;
    alias->active = true;
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_map_user_page(
    struct paging_process_space *space,
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint32_t permissions
)
{
    struct paging_translation supervisor;
    uint64_t *entry = NULL;
    const bool executable = (permissions & PAGING_EXECUTE) != 0U;
    const bool runtime_mapping =
        kind == PAGING_PROCESS_MAPPING_LINUX_HEAP ||
        kind == PAGING_PROCESS_MAPPING_LINUX_ANON;
    enum paging_status status;

    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if ((!runtime_mapping && process_space_runtime.state !=
            PAGING_PROCESS_SPACE_BUILDING) ||
        (runtime_mapping && process_space_runtime.state !=
            PAGING_PROCESS_SPACE_BUILDING &&
            process_space_runtime.state != PAGING_PROCESS_SPACE_INSTALLED &&
            process_space_runtime.state != PAGING_PROCESS_SPACE_ACTIVE) ||
        !process_mapping_request_valid(kind, virtual_address, permissions) ||
        (physical_address & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        !frame_range_overlaps_allocatable_memory(physical_address,
            PAGING_PAGE_SIZE)) {
        return PAGING_STATUS_PROCESS_BAD_MAPPING;
    }
    if (executable && !process_alias_contains(physical_address)) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    status = translate_address(&process_space_runtime.hierarchy,
        physical_address, &supervisor, state.pat_after);
    if (status != PAGING_STATUS_OK || supervisor.user ||
        supervisor.physical_address != physical_address ||
        supervisor.memory_type != PAGING_MEMORY_WRITE_BACK ||
        (executable && supervisor.permissions != PAGING_READ) ||
        (!executable && supervisor.permissions != PAGING_WRITE)) {
        return PAGING_STATUS_PROCESS_BAD_MAPPING;
    }
    status = entry_for_user(&process_space_runtime.hierarchy, virtual_address,
        &entry);
    if (status != PAGING_STATUS_OK) {
        sync_process_space(space);
        return status;
    }
    if ((*entry & PAGE_PRESENT) != 0U) {
        return PAGING_STATUS_ALREADY_MAPPED;
    }
    *entry = physical_address | permissions_to_flags(permissions) | PAGE_USER;
    invalidate(&process_space_runtime.hierarchy, virtual_address);
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_unmap_user_page(
    struct paging_process_space *space,
    enum paging_process_mapping_kind kind,
    uint64_t virtual_address
)
{
    uint64_t *entry = NULL;
    const bool runtime_mapping =
        kind == PAGING_PROCESS_MAPPING_LINUX_HEAP ||
        kind == PAGING_PROCESS_MAPPING_LINUX_ANON;
    enum paging_status status;

    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if ((!runtime_mapping && process_space_runtime.state ==
            PAGING_PROCESS_SPACE_ACTIVE) ||
        !process_unmap_request_valid(kind, virtual_address)) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    status = entry_for(&process_space_runtime.hierarchy, virtual_address, 1U,
        false, &entry);
    if (status != PAGING_STATUS_OK || (*entry & PAGE_PRESENT) == 0U ||
        (*entry & PAGE_USER) == 0U) {
        return PAGING_STATUS_NOT_MAPPED;
    }
    *entry = 0U;
    invalidate(&process_space_runtime.hierarchy, virtual_address);
    reclaim_empty_tables(&process_space_runtime.hierarchy, virtual_address);
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_translate(
    const struct paging_process_space *space,
    uint64_t virtual_address,
    struct paging_translation *translation
)
{
    if (translation == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    translation->physical_address = 0U;
    translation->permissions = PAGING_READ;
    translation->memory_type = PAGING_MEMORY_INVALID;
    translation->level = 0U;
    translation->user = false;
    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    return translate_address(&process_space_runtime.hierarchy, virtual_address,
        translation, state.pat_after);
}

enum paging_status paging_process_validate(
    struct paging_process_space *space,
    uint64_t image_physical_address,
    const uintptr_t stack_frames[PAGING_PROCESS_STACK_PAGES]
)
{
    struct paging_translation translation;
    struct paging_audit audit;

    if (stack_frames == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if (process_space_runtime.state != PAGING_PROCESS_SPACE_BUILDING ||
        !process_alias_runtime.owned ||
        process_alias_runtime.count != 1U ||
        process_alias_runtime.pages[0].physical_address !=
            image_physical_address ||
        !no_user_bits_in_kernel_branch(&process_space_runtime.hierarchy)) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    if (translate_address(&process_space_runtime.hierarchy, 0U, &translation,
            state.pat_after) != PAGING_STATUS_NOT_MAPPED ||
        translate_address(&process_space_runtime.hierarchy,
            PAGING_PROCESS_STACK_GUARD, &translation, state.pat_after) !=
            PAGING_STATUS_NOT_MAPPED) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    if (translate_address(&process_space_runtime.hierarchy,
            PAGING_PROCESS_IMAGE_ADDRESS, &translation, state.pat_after) !=
            PAGING_STATUS_OK || !translation.user ||
        translation.permissions != PAGING_EXECUTE || translation.level != 1U ||
        translation.physical_address != image_physical_address ||
        translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    if (translate_address(&process_space_runtime.hierarchy,
            image_physical_address, &translation, state.pat_after) !=
            PAGING_STATUS_OK || translation.user ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != image_physical_address) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    for (size_t index = 0U; index < PAGING_PROCESS_STACK_PAGES; ++index) {
        const uint64_t address = PAGING_PROCESS_STACK_BASE +
            index * PAGING_PAGE_SIZE;

        if (translate_address(&process_space_runtime.hierarchy, address,
                &translation, state.pat_after) != PAGING_STATUS_OK ||
            !translation.user || translation.permissions != PAGING_WRITE ||
            translation.level != 1U ||
            translation.physical_address != (uint64_t)stack_frames[index]) {
            return PAGING_STATUS_VALIDATION_FAILURE;
        }
    }
    audit_hierarchy(&process_space_runtime.hierarchy, &audit);
    if (audit.write_execute_leaves != 0U ||
        audit.user_leaves != 1U + PAGING_PROCESS_STACK_PAGES ||
        (table_at(process_space_runtime.hierarchy.root)[0] & PAGE_USER) != 0U ||
        (table_at(process_space_runtime.hierarchy.root)
            [table_index(PAGING_PROCESS_IMAGE_ADDRESS, 4U)] & PAGE_USER) == 0U) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    process_space_runtime.state = PAGING_PROCESS_SPACE_INSTALLED;
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_validate_linux(
    struct paging_process_space *space,
    const struct paging_process_expected_page *pages,
    size_t page_count
)
{
    struct paging_translation translation;
    struct paging_audit audit;
    size_t executable_pages = 0U;

    if (pages == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if (process_space_runtime.state != PAGING_PROCESS_SPACE_BUILDING ||
        !process_alias_runtime.owned || page_count == 0U ||
        page_count > PAGING_PROCESS_EXPECTED_MAX_PAGES ||
        !no_user_bits_in_kernel_branch(&process_space_runtime.hierarchy)) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    if (translate_address(&process_space_runtime.hierarchy, 0U, &translation,
            state.pat_after) != PAGING_STATUS_NOT_MAPPED ||
        translate_address(&process_space_runtime.hierarchy,
            PAGING_LINUX_STACK_GUARD, &translation, state.pat_after) !=
            PAGING_STATUS_NOT_MAPPED ||
        translate_address(&process_space_runtime.hierarchy,
            PAGING_LINUX_HEAP_BASE, &translation, state.pat_after) !=
            PAGING_STATUS_NOT_MAPPED ||
        translate_address(&process_space_runtime.hierarchy,
            PAGING_LINUX_ANON_ADDRESS, &translation, state.pat_after) !=
            PAGING_STATUS_NOT_MAPPED) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    for (size_t index = 0U; index < page_count; ++index) {
        const struct paging_process_expected_page *expected = &pages[index];
        const bool image = process_mapping_request_valid(
            PAGING_PROCESS_MAPPING_LINUX_IMAGE, expected->virtual_address,
            expected->permissions);
        const bool stack = process_mapping_request_valid(
            PAGING_PROCESS_MAPPING_LINUX_STACK, expected->virtual_address,
            expected->permissions);

        if ((!image && !stack) ||
            translate_address(&process_space_runtime.hierarchy,
                expected->virtual_address, &translation, state.pat_after) !=
                PAGING_STATUS_OK || !translation.user ||
            translation.permissions != expected->permissions ||
            translation.level != 1U ||
            translation.physical_address != expected->physical_address ||
            translation.memory_type != PAGING_MEMORY_WRITE_BACK) {
            return PAGING_STATUS_VALIDATION_FAILURE;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (pages[prior].virtual_address == expected->virtual_address ||
                pages[prior].physical_address == expected->physical_address) {
                return PAGING_STATUS_VALIDATION_FAILURE;
            }
        }
        if ((expected->permissions & PAGING_EXECUTE) != 0U) {
            ++executable_pages;
            if (!process_alias_contains(expected->physical_address) ||
                translate_address(&process_space_runtime.hierarchy,
                    expected->physical_address, &translation,
                    state.pat_after) != PAGING_STATUS_OK ||
                translation.user || translation.permissions != PAGING_READ ||
                translation.physical_address != expected->physical_address) {
                return PAGING_STATUS_VALIDATION_FAILURE;
            }
        }
    }
    audit_hierarchy(&process_space_runtime.hierarchy, &audit);
    if (executable_pages != process_alias_runtime.count ||
        audit.write_execute_leaves != 0U ||
        audit.user_leaves != page_count ||
        (table_at(process_space_runtime.hierarchy.root)[0] & PAGE_USER) != 0U ||
        (table_at(process_space_runtime.hierarchy.root)
            [table_index(PAGING_LINUX_IMAGE_BASE, 4U)] & PAGE_USER) == 0U ||
        (table_at(process_space_runtime.hierarchy.root)
            [table_index(PAGING_LINUX_STACK_BASE, 4U)] & PAGE_USER) == 0U) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    process_space_runtime.state = PAGING_PROCESS_SPACE_INSTALLED;
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_activate(struct paging_process_space *space)
{
    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if (process_space_runtime.state != PAGING_PROCESS_SPACE_INSTALLED ||
        cpu_interrupts_enabled() || !process_alias_runtime.owned ||
        (cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    cpu_write_cr3(process_space_runtime.hierarchy.root);
    if ((cpu_read_cr3() & PAGE_FRAME_MASK) !=
            process_space_runtime.hierarchy.root) {
        cpu_write_cr3(live_hierarchy.root);
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    process_space_runtime.state = PAGING_PROCESS_SPACE_ACTIVE;
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_restore_kernel(
    struct paging_process_space *space
)
{
    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    if (process_space_runtime.state != PAGING_PROCESS_SPACE_ACTIVE ||
        cpu_interrupts_enabled() ||
        (cpu_read_cr3() & PAGE_FRAME_MASK) !=
            process_space_runtime.hierarchy.root) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    cpu_write_cr3(live_hierarchy.root);
    if ((cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    process_space_runtime.state = PAGING_PROCESS_SPACE_INSTALLED;
    sync_process_space(space);
    return PAGING_STATUS_OK;
}

static enum paging_status restore_active_aliases(
    const struct paging_process_space *space
)
{
    struct paging_audit audit;

    if (!process_space_matches(space) || !process_alias_runtime.owned) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    audit_hierarchy(&process_space_runtime.hierarchy, &audit);
    if (process_space_runtime.state == PAGING_PROCESS_SPACE_ACTIVE ||
        cpu_interrupts_enabled() || audit.user_leaves != 0U ||
        (cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_PROCESS_ALIAS_STATE;
    }
    for (size_t remaining = process_alias_runtime.count;
         remaining > 0U; --remaining) {
        if (restore_alias_page(
                &process_alias_runtime.pages[remaining - 1U], false) !=
                PAGING_STATUS_OK) {
            state.table_frames = live_hierarchy.table_frames;
            return PAGING_STATUS_PROCESS_ALIAS_STATE;
        }
    }
    state.table_frames = live_hierarchy.table_frames;
    process_alias_runtime.count = 0U;
    process_alias_runtime.owned = false;
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_image_alias_restore(
    const struct paging_process_space *space,
    struct paging_process_image_alias *alias
)
{
    enum paging_status status;

    if (alias == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    if (!alias->active || !process_alias_runtime.owned ||
        process_alias_runtime.count != 1U ||
        alias->generation != process_alias_runtime.generation ||
        alias->physical_address !=
            process_alias_runtime.pages[0].physical_address) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    status = restore_active_aliases(space);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    zero_process_alias(alias);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_alias_set_restore(
    const struct paging_process_space *space,
    struct paging_process_alias_set *alias
)
{
    enum paging_status status;

    if (alias == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }
    if (!alias->active || !process_alias_runtime.owned ||
        alias->generation != process_alias_runtime.generation ||
        alias->count != process_alias_runtime.count) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    for (size_t index = 0U; index < alias->count; ++index) {
        if (alias->physical_addresses[index] !=
                process_alias_runtime.pages[index].physical_address) {
            return PAGING_STATUS_PROCESS_BAD_TOKEN;
        }
    }
    status = restore_active_aliases(space);
    if (status != PAGING_STATUS_OK) {
        return status;
    }
    zero_process_alias_set(alias);
    return PAGING_STATUS_OK;
}

enum paging_status paging_process_space_release(
    struct paging_process_space *space
)
{
    struct paging_audit audit;

    if (!process_space_matches(space)) {
        return PAGING_STATUS_PROCESS_BAD_TOKEN;
    }
    audit_hierarchy(&process_space_runtime.hierarchy, &audit);
    if (process_space_runtime.state == PAGING_PROCESS_SPACE_ACTIVE ||
        process_alias_runtime.owned || audit.user_leaves != 0U ||
        (cpu_read_cr3() & PAGE_FRAME_MASK) != live_hierarchy.root) {
        return PAGING_STATUS_PROCESS_BAD_STATE;
    }
    release_table_tree(&process_space_runtime.hierarchy,
        process_space_runtime.hierarchy.root, PAGING_LEVEL_COUNT);
    if (process_space_runtime.hierarchy.table_frames != 0U) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }
    process_space_runtime.owned = false;
    process_space_runtime.state = PAGING_PROCESS_SPACE_RELEASED;
    space->root_physical_address = 0U;
    space->table_frames = 0U;
    space->state = PAGING_PROCESS_SPACE_RELEASED;
    return PAGING_STATUS_OK;
}

bool paging_process_resources_released(void)
{
    return !process_space_runtime.owned && !process_alias_runtime.owned &&
        (!state.active ||
            (cpu_read_cr3() & PAGE_FRAME_MASK) == live_hierarchy.root);
}

enum paging_status paging_map(
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t length,
    uint32_t permissions
)
{
    enum paging_status status;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    status = map_range(&live_hierarchy, virtual_address, physical_address,
        length, permissions, 1U);
    if (status == PAGING_STATUS_OK) {
        status = supervisor_intent_add(virtual_address, physical_address,
            length, permissions);
        if (status != PAGING_STATUS_OK) {
            (void)unmap_range(&live_hierarchy, virtual_address, length);
        }
    }
    state.table_frames = live_hierarchy.table_frames;
    return status;
}

enum paging_status paging_unmap(uint64_t virtual_address, uint64_t length)
{
    enum paging_status status;
    size_t intent;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    intent = supervisor_intent_find(virtual_address, length);
    status = unmap_range(&live_hierarchy, virtual_address, length);
    if (status == PAGING_STATUS_OK && intent != SIZE_MAX) {
        supervisor_intent_remove(intent);
    }

    /*
     * An unmap can hand interior tables back, so the reported count is taken
     * from the hierarchy after every mutation rather than fixed at install.
     */
    state.table_frames = live_hierarchy.table_frames;
    return status;
}

enum paging_status paging_protect(
    uint64_t virtual_address,
    uint64_t length,
    uint32_t permissions
)
{
    enum paging_status status;
    const size_t intent = supervisor_intent_find(virtual_address, length);

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    status = protect_range(&live_hierarchy, virtual_address, length,
        permissions, state.pat_after);
    if (status == PAGING_STATUS_OK && intent != SIZE_MAX) {
        supervisor_intents[intent].permissions = permissions;
    }
    return status;
}

enum paging_status paging_translate(
    uint64_t virtual_address,
    struct paging_translation *translation
)
{
    if (translation == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    translation->physical_address = 0U;
    translation->permissions = PAGING_READ;
    translation->memory_type = PAGING_MEMORY_INVALID;
    translation->level = 0U;
    translation->user = false;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    return translate_address(&live_hierarchy, virtual_address, translation,
        state.pat_after);
}

enum paging_status paging_audit_hierarchy(struct paging_audit *audit)
{
    if (audit == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    audit->leaf_count = 0U;
    audit->huge_leaves = 0U;
    audit->writable_leaves = 0U;
    audit->executable_leaves = 0U;
    audit->write_execute_leaves = 0U;
    audit->user_leaves = 0U;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    audit_hierarchy(&live_hierarchy, audit);
    return PAGING_STATUS_OK;
}

static bool device_windows_equal(
    const struct paging_device_window *left,
    const struct paging_device_window *right
)
{
    return left->kind == right->kind &&
        left->instance == right->instance &&
        left->physical_base == right->physical_base &&
        left->length == right->length &&
        left->memory_type == right->memory_type &&
        left->permissions == right->permissions;
}

enum paging_status paging_verify_device_windows(
    const struct paging_device_windows *expected,
    size_t *failed_index
)
{
    struct paging_device_windows validated;
    enum paging_status status;

    if (failed_index == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    *failed_index = PAGING_DEVICE_WINDOW_NONE;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    status = paging_device_windows_validate(expected, &validated);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    if (validated.count != installed_device_windows.count) {
        return PAGING_STATUS_INSTALLED_DEVICE_WINDOW_MISMATCH;
    }

    for (size_t index = 0U; index < validated.count; ++index) {
        const struct paging_device_window *window = &validated.entries[index];
        const struct paging_device_window *installed =
            &installed_device_windows.entries[index];
        const uint32_t permissions =
            device_window_mapping_permissions(window);

        if (!device_windows_equal(window, installed) ||
            window->permissions != PAGING_DEVICE_WINDOW_WRITE ||
            (window->kind == PAGING_DEVICE_WINDOW_VGA_TEXT &&
                window->physical_base != PAGING_VGA_TEXT_BUFFER_BASE) ||
            (window->kind != PAGING_DEVICE_WINDOW_FRAMEBUFFER &&
                window->memory_type != PAGING_MEMORY_UNCACHEABLE)) {
            *failed_index = index;
            return PAGING_STATUS_INSTALLED_DEVICE_WINDOW_MISMATCH;
        }

        for (uint64_t offset = 0U; offset < window->length;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = window->physical_base + offset;
            struct paging_translation translation;

            if (translate_address(&live_hierarchy, address, &translation,
                    state.pat_after) != PAGING_STATUS_OK ||
                translation.physical_address != address ||
                translation.permissions != permissions ||
                translation.memory_type != window->memory_type ||
                translation.level != 1U) {
                *failed_index = index;
                return PAGING_STATUS_INSTALLED_DEVICE_WINDOW_MISMATCH;
            }
        }
    }

    return PAGING_STATUS_OK;
}

/*
 * What `make verify` cannot check. The ELF assertion proves no load segment is
 * RWX in the file; this proves no page is writable and executable on the
 * machine, that the processor is still using the hierarchy that was audited,
 * and that the two bits which make the permissions mean anything are still on.
 */
enum paging_status paging_verify(void)
{
    static const struct {
        const uint8_t *symbol;
        uint32_t permissions;
    } expected[] = {
        {__text_start, PAGING_EXECUTE},
        {__rodata_start, PAGING_READ},
        {__data_start, PAGING_WRITE}
    };

    struct paging_translation translation;
    struct paging_audit audit;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    if ((cpu_read_cr3() & PAGE_FRAME_MASK) != state.root_physical_address ||
        state.table_frames != live_hierarchy.table_frames ||
        live_hierarchy.table_frames == 0U) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }

    if (cpu_read_msr(IA32_PAT_MSR) != state.pat_after ||
        pat_entry(state.pat_after, state.write_combining_pat_entry) !=
            PAT_TYPE_WRITE_COMBINING) {
        return PAGING_STATUS_PAT_READBACK_MISMATCH;
    }

    if ((cpu_read_msr(IA32_EFER_MSR) & EFER_NO_EXECUTE_ENABLE) == 0U) {
        return PAGING_STATUS_NO_EXECUTE_INACTIVE;
    }

    if ((cpu_read_cr0() & CR0_WRITE_PROTECT) == 0U) {
        return PAGING_STATUS_WRITE_PROTECT_INACTIVE;
    }

    audit_hierarchy(&live_hierarchy, &audit);

    if (audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }

    for (size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        const uint64_t address = (uint64_t)(uintptr_t)expected[index].symbol;

        if (translate_address(&live_hierarchy, address, &translation,
                state.pat_after) !=
                PAGING_STATUS_OK ||
            translation.physical_address != address ||
            translation.permissions != expected[index].permissions) {
            return PAGING_STATUS_VALIDATION_FAILURE;
        }
    }

    if (translate_address(&live_hierarchy, 0U, &translation, state.pat_after) !=
        PAGING_STATUS_NOT_MAPPED) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }

    return PAGING_STATUS_OK;
}

struct paging_state paging_get_state(void)
{
    return state;
}

const struct paging_device_windows *paging_get_device_windows(void)
{
    return &installed_device_windows;
}

bool paging_is_active(void)
{
    return state.active;
}

static void reset_test_hierarchy(
    struct page_hierarchy *hierarchy,
    size_t capacity
)
{
    for (size_t index = 0; index < PAGING_TEST_ENTRIES; ++index) {
        test_arena[index] = 0U;
    }

    hierarchy->root = 0U;
    hierarchy->arena_base = (uint64_t)(uintptr_t)test_arena;
    hierarchy->arena_capacity = capacity;
    hierarchy->arena_used = 0U;
    hierarchy->table_frames = 0U;
    hierarchy->live = false;
}

static bool test_indices_and_canonical_form(void)
{
    /*
     * One synthetic address with a different index at every level, so a shift
     * that is wrong by one level cannot still produce the expected answer. The
     * top index stays below 256 to keep bit 47 clear and the address canonical.
     */
    const uint64_t address = ((uint64_t)163U << 39U) | ((uint64_t)199U << 30U) |
        ((uint64_t)341U << 21U) | ((uint64_t)238U << 12U) | UINT64_C(0x123);
    const uint64_t high = UINT64_C(0xFFFF000000000000) |
        ((uint64_t)419U << 39U) | ((uint64_t)7U << 30U);

    if (table_index(address, 4U) != 163U || table_index(address, 3U) != 199U ||
        table_index(address, 2U) != 341U || table_index(address, 1U) != 238U) {
        return false;
    }

    if (table_index(high, 4U) != 419U || table_index(high, 3U) != 7U) {
        return false;
    }

    if (level_page_size(1U) != PAGING_PAGE_SIZE ||
        level_page_size(2U) != PAGING_HUGE_PAGE_SIZE ||
        level_page_size(3U) != UINT64_C(0x40000000) ||
        level_page_size(4U) != UINT64_C(0x8000000000)) {
        return false;
    }

    /* The two halves of the address space, and the hole between them. */
    if (!address_is_canonical(0U) ||
        !address_is_canonical(UINT64_C(0x00007FFFFFFFFFFF)) ||
        !address_is_canonical(UINT64_C(0xFFFF800000000000)) ||
        !address_is_canonical(UINT64_MAX)) {
        return false;
    }

    return !address_is_canonical(UINT64_C(0x0000800000000000)) &&
        !address_is_canonical(UINT64_C(0xFFFF7FFFFFFFFFFF)) &&
        !address_is_canonical(UINT64_C(0x0001000000000000)) &&
        !address_is_canonical(PAGE_FRAME_MASK);
}

static bool test_entry_composition(void)
{
    static const uint32_t cases[] = {
        PAGING_READ,
        PAGING_WRITE,
        PAGING_EXECUTE,
        PAGING_UNCACHED,
        PAGING_WRITE_COMBINING,
        PAGING_WRITE | PAGING_UNCACHED,
        PAGING_EXECUTE | PAGING_UNCACHED,
        PAGING_WRITE | PAGING_WRITE_COMBINING
    };

    const uint64_t frame = UINT64_C(0x000ABCDE12345000);

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint64_t entry = frame | permissions_to_flags(cases[index]);

        if ((entry & PAGE_FRAME_MASK) != frame ||
            (entry & PAGE_PRESENT) == 0U ||
            (entry & PAGE_USER) != 0U ||
            flags_to_permissions(entry, 1U, PAGING_TEST_PAT) != cases[index]) {
            return false;
        }
    }

    /* Read-only means no-execute is set, not merely that write is clear. */
    if ((permissions_to_flags(PAGING_READ) & PAGE_NO_EXECUTE) == 0U ||
        (permissions_to_flags(PAGING_EXECUTE) & PAGE_NO_EXECUTE) != 0U ||
        (permissions_to_flags(PAGING_UNCACHED) & PAGE_CACHE_DISABLE) == 0U ||
        (permissions_to_flags(PAGING_WRITE_COMBINING) & PAGE_WRITE_THROUGH) ==
            0U ||
        (permissions_to_flags(PAGING_WRITE_COMBINING) & PAGE_CACHE_DISABLE) !=
            0U) {
        return false;
    }

    return validate_permissions(PAGING_WRITE | PAGING_EXECUTE) ==
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE &&
        validate_permissions(1U << 4) == PAGING_STATUS_BAD_PERMISSIONS &&
        validate_permissions(PAGING_UNCACHED | PAGING_WRITE_COMBINING) ==
            PAGING_STATUS_CONFLICTING_MEMORY_TYPES &&
        validate_permissions(PAGING_WRITE | PAGING_UNCACHED) ==
            PAGING_STATUS_OK &&
        validate_permissions(PAGING_WRITE | PAGING_WRITE_COMBINING) ==
            PAGING_STATUS_OK;
}

static bool test_pat_model(void)
{
    const uint64_t pat = UINT64_C(0x0106070504000106);

    if (pat_entry(pat, 0U) != PAT_TYPE_WRITE_BACK ||
        pat_entry(pat, 1U) != PAT_TYPE_WRITE_COMBINING ||
        pat_entry(pat, 2U) != PAT_TYPE_UNCACHEABLE ||
        pat_entry(pat, 3U) != PAT_TYPE_WRITE_THROUGH ||
        pat_replace_entry(pat, 1U, PAT_TYPE_WRITE_THROUGH) !=
            UINT64_C(0x0106070504000406)) {
        return false;
    }

    if (leaf_pat_index(0U, 1U) != 0U ||
        leaf_pat_index(PAGE_WRITE_THROUGH, 1U) != 1U ||
        leaf_pat_index(PAGE_CACHE_DISABLE, 1U) != 2U ||
        leaf_pat_index(PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE, 1U) != 3U ||
        leaf_pat_index(PAGE_HUGE, 1U) != 4U ||
        leaf_pat_index(PAGE_HUGE, 2U) != 0U ||
        leaf_pat_index(PAGE_LARGE_PAT, 2U) != 4U ||
        leaf_pat_index(PAGE_LARGE_PAT | PAGE_WRITE_THROUGH, 2U) != 5U) {
        return false;
    }

    return leaf_memory_type(0U, 1U, pat) == PAGING_MEMORY_WRITE_BACK &&
        leaf_memory_type(PAGE_WRITE_THROUGH, 1U, pat) ==
            PAGING_MEMORY_WRITE_COMBINING &&
        leaf_memory_type(PAGE_CACHE_DISABLE, 1U, pat) ==
            PAGING_MEMORY_UNCACHEABLE &&
        leaf_memory_type(PAGE_WRITE_THROUGH | PAGE_CACHE_DISABLE, 1U, pat) ==
            PAGING_MEMORY_WRITE_THROUGH &&
        leaf_memory_type(PAGE_HUGE, 1U, pat) ==
            PAGING_MEMORY_WRITE_PROTECTED &&
        leaf_memory_type(PAGE_LARGE_PAT | PAGE_WRITE_THROUGH, 2U, pat) ==
            PAGING_MEMORY_UNCACHED_MINUS &&
        decode_pat_type(UINT8_C(0x02)) == PAGING_MEMORY_INVALID;
}

static bool test_range_validation(void)
{
    if (validate_virtual_range(0U, 0U, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_ZERO_LENGTH ||
        validate_virtual_range(1U, PAGING_PAGE_SIZE, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_UNALIGNED_ADDRESS ||
        validate_virtual_range(0U, 1U, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_UNALIGNED_ADDRESS) {
        return false;
    }

    /* 4 KiB alignment is not enough for a 2 MiB leaf. */
    if (validate_virtual_range(PAGING_PAGE_SIZE, PAGING_HUGE_PAGE_SIZE,
            PAGING_HUGE_PAGE_SIZE) != PAGING_STATUS_UNALIGNED_ADDRESS) {
        return false;
    }

    if (validate_virtual_range(UINT64_C(0x0000800000000000), PAGING_PAGE_SIZE,
            PAGING_PAGE_SIZE) != PAGING_STATUS_NONCANONICAL_ADDRESS) {
        return false;
    }

    /* A range that starts canonical and runs into the hole. */
    if (validate_virtual_range(UINT64_C(0x00007FFFFFFFF000),
            PAGING_PAGE_SIZE * 2U, PAGING_PAGE_SIZE) !=
        PAGING_STATUS_NONCANONICAL_ADDRESS) {
        return false;
    }

    if (validate_virtual_range(UINT64_C(0xFFFFFFFFFFFFF000),
            PAGING_PAGE_SIZE * 2U, PAGING_PAGE_SIZE) !=
        PAGING_STATUS_RANGE_OVERFLOW) {
        return false;
    }

    /*
     * The two sides of the width bound. PAGE_FRAME_MASK is itself the highest
     * page an entry can name, so it must be accepted; the first page above it
     * must not. Getting this off by one page in the accepting direction would
     * be silent, which is why both sides are pinned.
     */
    if (validate_physical_range(1U, PAGING_PAGE_SIZE, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_UNALIGNED_ADDRESS ||
        validate_physical_range(PAGE_FRAME_MASK, PAGING_PAGE_SIZE,
            PAGING_PAGE_SIZE) != PAGING_STATUS_OK ||
        validate_physical_range(PAGE_PHYSICAL_LIMIT, PAGING_PAGE_SIZE,
            PAGING_PAGE_SIZE) != PAGING_STATUS_PHYSICAL_TOO_WIDE ||
        validate_physical_range(PAGE_FRAME_MASK, PAGING_PAGE_SIZE * 2U,
            PAGING_PAGE_SIZE) != PAGING_STATUS_PHYSICAL_TOO_WIDE) {
        return false;
    }

    return validate_virtual_range(PAGING_PAGE_SIZE, PAGING_PAGE_SIZE,
        PAGING_PAGE_SIZE) == PAGING_STATUS_OK;
}

static bool test_hierarchy_operations(void)
{
    const uint64_t address = UINT64_C(0x0000000040200000);
    const uint64_t huge_address = address + PAGING_HUGE_PAGE_SIZE;
    const uint64_t frame = UINT64_C(0x0000000000123000);
    struct page_hierarchy hierarchy;
    struct paging_translation translation;
    struct paging_audit audit;
    uint64_t *ancestors[2] = {NULL, NULL};
    uint64_t *entry = NULL;

    reset_test_hierarchy(&hierarchy, PAGING_TEST_ARENA_PAGES);

    if (allocate_table(&hierarchy, &hierarchy.root) != PAGING_STATUS_OK) {
        return false;
    }

    /* Nothing is mapped yet, so every operation on the page must refuse. */
    if (translate_address(&hierarchy, address, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_NOT_MAPPED ||
        translation.level != 0U ||
        translation.physical_address != 0U ||
        unmap_range(&hierarchy, address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        protect_range(&hierarchy, address, PAGING_PAGE_SIZE, PAGING_WRITE,
            PAGING_TEST_PAT) !=
            PAGING_STATUS_NOT_MAPPED) {
        return false;
    }

    if (map_range(&hierarchy, address, frame, PAGING_PAGE_SIZE, PAGING_WRITE,
            1U) != PAGING_STATUS_OK) {
        return false;
    }

    if (translate_address(&hierarchy, address + 0x40U, &translation,
            PAGING_TEST_PAT) !=
            PAGING_STATUS_OK ||
        translation.physical_address != frame + 0x40U ||
        translation.permissions != PAGING_WRITE ||
        translation.level != 1U) {
        return false;
    }

    if (map_range(&hierarchy, address, frame, PAGING_PAGE_SIZE, PAGING_WRITE,
            1U) != PAGING_STATUS_ALREADY_MAPPED) {
        return false;
    }

    if (protect_range(&hierarchy, address, PAGING_PAGE_SIZE,
            PAGING_WRITE | PAGING_EXECUTE, PAGING_TEST_PAT) !=
        PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        return false;
    }

    if (protect_range(&hierarchy, address, PAGING_PAGE_SIZE,
            PAGING_WRITE_COMBINING, PAGING_TEST_PAT) !=
            PAGING_STATUS_MEMORY_TYPE_CHANGE_UNSAFE ||
        protect_range(&hierarchy, address, PAGING_PAGE_SIZE, PAGING_READ,
            PAGING_TEST_PAT) !=
            PAGING_STATUS_OK ||
        translate_address(&hierarchy, address, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_OK ||
        translation.permissions != PAGING_READ ||
        translation.physical_address != frame) {
        return false;
    }

    /*
     * A page directory entry that already points at a page table is a present
     * entry, so installing a 2 MiB leaf over it while a 4 KiB page still lives
     * under it would orphan that table, and is refused.
     */
    if (map_range(&hierarchy, address, PAGING_HUGE_PAGE_SIZE,
            PAGING_HUGE_PAGE_SIZE, PAGING_WRITE, 2U) !=
        PAGING_STATUS_ALREADY_MAPPED) {
        return false;
    }

    if (unmap_range(&hierarchy, address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_OK ||
        translate_address(&hierarchy, address, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_NOT_MAPPED ||
        unmap_range(&hierarchy, address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED) {
        return false;
    }

    if (map_range(&hierarchy, huge_address, PAGING_HUGE_PAGE_SIZE,
            PAGING_HUGE_PAGE_SIZE, PAGING_WRITE, 2U) != PAGING_STATUS_OK) {
        return false;
    }

    if (translate_address(&hierarchy, huge_address + 0x1000U, &translation,
            PAGING_TEST_PAT) !=
            PAGING_STATUS_OK ||
        translation.level != 2U ||
        translation.permissions != PAGING_WRITE ||
        translation.physical_address != PAGING_HUGE_PAGE_SIZE + 0x1000U) {
        return false;
    }

    /* A 2 MiB leaf covers the 4 KiB pages beneath it and refuses them. */
    if (map_range(&hierarchy, huge_address, frame, PAGING_PAGE_SIZE,
            PAGING_WRITE, 1U) != PAGING_STATUS_HUGE_PAGE_PRESENT ||
        unmap_range(&hierarchy, huge_address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_HUGE_PAGE_PRESENT ||
        protect_range(&hierarchy, huge_address, PAGING_PAGE_SIZE,
            PAGING_READ, PAGING_TEST_PAT) != PAGING_STATUS_HUGE_PAGE_PRESENT) {
        return false;
    }

    /*
     * The audit must be able to see a violation, or its silence at boot proves
     * nothing at all. The entries below are corrupted by hand because no path
     * through this file can produce them, and repaired immediately afterwards.
     */
    audit_hierarchy(&hierarchy, &audit);

    if (audit.leaf_count != 1U || audit.huge_leaves != 1U ||
        audit.writable_leaves != 1U || audit.executable_leaves != 0U ||
        audit.write_execute_leaves != 0U || audit.user_leaves != 0U) {
        return false;
    }

    if (entry_for(&hierarchy, huge_address, 4U, false, &ancestors[0]) !=
            PAGING_STATUS_OK ||
        entry_for(&hierarchy, huge_address, 3U, false, &ancestors[1]) !=
            PAGING_STATUS_OK ||
        entry_for(&hierarchy, huge_address, 2U, false, &entry) !=
            PAGING_STATUS_OK) {
        return false;
    }

    /* A writable leaf with its no-execute bit cleared is the violation. */
    *entry &= ~PAGE_NO_EXECUTE;
    audit_hierarchy(&hierarchy, &audit);

    if (audit.write_execute_leaves != 1U || audit.executable_leaves != 1U) {
        return false;
    }

    /*
     * Effective permission is the conjunction down the whole path, so an
     * ancestor carrying no-execute must hide a leaf that does not. An audit
     * that only read the leaf would report this page executable.
     */
    *ancestors[1] |= PAGE_NO_EXECUTE;
    audit_hierarchy(&hierarchy, &audit);

    if (audit.write_execute_leaves != 0U || audit.executable_leaves != 0U) {
        return false;
    }

    *ancestors[1] &= ~PAGE_NO_EXECUTE;
    *entry |= PAGE_NO_EXECUTE;

    /* The same conjunction for user reachability: the leaf alone is not it. */
    *entry |= PAGE_USER;
    audit_hierarchy(&hierarchy, &audit);

    if (audit.user_leaves != 0U) {
        return false;
    }

    *ancestors[0] |= PAGE_USER;
    *ancestors[1] |= PAGE_USER;
    audit_hierarchy(&hierarchy, &audit);

    if (audit.user_leaves != 1U) {
        return false;
    }

    *ancestors[0] &= ~PAGE_USER;
    *ancestors[1] &= ~PAGE_USER;
    *entry &= ~PAGE_USER;
    audit_hierarchy(&hierarchy, &audit);

    return audit.leaf_count == 1U && audit.write_execute_leaves == 0U &&
        audit.executable_leaves == 0U && audit.user_leaves == 0U;
}

/*
 * An unmap must give back the interior tables it emptied, and must give back
 * nothing while an entry remains. Getting the first half wrong leaks a table
 * frame per unmap; getting the second half wrong frees a table that other
 * mappings are still reached through, which is unrecoverable.
 */
static bool test_table_reclamation(void)
{
    const uint64_t first = UINT64_C(0x0000000040000000);
    const uint64_t second = first + PAGING_PAGE_SIZE;
    const uint64_t far = first + PAGING_HUGE_PAGE_SIZE;
    struct page_hierarchy hierarchy;
    struct paging_translation translation;

    reset_test_hierarchy(&hierarchy, PAGING_TEST_ARENA_PAGES);

    if (allocate_table(&hierarchy, &hierarchy.root) != PAGING_STATUS_OK ||
        hierarchy.table_frames != 1U) {
        return false;
    }

    /* One 4 KiB page needs a PDPT, a page directory and a page table. */
    if (map_range(&hierarchy, first, 0U, PAGING_PAGE_SIZE, PAGING_WRITE, 1U) !=
            PAGING_STATUS_OK ||
        hierarchy.table_frames != 4U) {
        return false;
    }

    /* A second page in the same table needs no table of its own. */
    if (map_range(&hierarchy, second, PAGING_PAGE_SIZE, PAGING_PAGE_SIZE,
            PAGING_WRITE, 1U) != PAGING_STATUS_OK ||
        hierarchy.table_frames != 4U) {
        return false;
    }

    /* Emptying half a table reclaims nothing. */
    if (unmap_range(&hierarchy, first, PAGING_PAGE_SIZE) != PAGING_STATUS_OK ||
        hierarchy.table_frames != 4U ||
        translate_address(&hierarchy, second, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_OK) {
        return false;
    }

    /* Emptying the last entry reclaims the table, and everything above it. */
    if (unmap_range(&hierarchy, second, PAGING_PAGE_SIZE) != PAGING_STATUS_OK ||
        hierarchy.table_frames != 1U) {
        return false;
    }

    /* The root's own entry must be cleared, not left pointing at a free page. */
    if ((table_at(hierarchy.root)[table_index(first, PAGING_LEVEL_COUNT)] &
        PAGE_PRESENT) != 0U) {
        return false;
    }

    /*
     * And the slot is genuinely free again: a 2 MiB leaf now installs where a
     * page table used to be. Before tables were reclaimed the directory entry
     * stayed present forever and this was refused for the life of the kernel.
     */
    if (map_range(&hierarchy, first, PAGING_HUGE_PAGE_SIZE,
            PAGING_HUGE_PAGE_SIZE, PAGING_WRITE, 2U) != PAGING_STATUS_OK ||
        translate_address(&hierarchy, first, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_OK ||
        translation.level != 2U) {
        return false;
    }

    /*
     * Two pages under one page directory but in different page tables. Undoing
     * one must release its page table and stop there, because the directory
     * still holds the other.
     */
    reset_test_hierarchy(&hierarchy, PAGING_TEST_ARENA_PAGES);

    if (allocate_table(&hierarchy, &hierarchy.root) != PAGING_STATUS_OK ||
        map_range(&hierarchy, first, 0U, PAGING_PAGE_SIZE, PAGING_WRITE, 1U) !=
            PAGING_STATUS_OK ||
        map_range(&hierarchy, far, 0U, PAGING_PAGE_SIZE, PAGING_WRITE, 1U) !=
            PAGING_STATUS_OK ||
        hierarchy.table_frames != 5U) {
        return false;
    }

    if (unmap_range(&hierarchy, far, PAGING_PAGE_SIZE) != PAGING_STATUS_OK ||
        hierarchy.table_frames != 4U ||
        translate_address(&hierarchy, first, &translation, PAGING_TEST_PAT) !=
            PAGING_STATUS_OK) {
        return false;
    }

    /* And undoing the last one collapses the rest of the path. */
    return unmap_range(&hierarchy, first, PAGING_PAGE_SIZE) ==
            PAGING_STATUS_OK &&
        hierarchy.table_frames == 1U &&
        translate_address(&hierarchy, far, &translation, PAGING_TEST_PAT) ==
            PAGING_STATUS_NOT_MAPPED;
}

static bool test_table_supply(void)
{
    struct page_hierarchy hierarchy;

    /* A root and one interior table, where three interior tables are needed. */
    reset_test_hierarchy(&hierarchy, 2U);

    if (allocate_table(&hierarchy, &hierarchy.root) != PAGING_STATUS_OK ||
        map_range(&hierarchy, PAGING_HUGE_PAGE_SIZE, 0U, PAGING_PAGE_SIZE,
            PAGING_WRITE, 1U) != PAGING_STATUS_OUT_OF_FRAMES) {
        return false;
    }

    /*
     * A table the walk could not read back through its own physical address.
     * The arena is placed exactly at the identity window's edge, so the first
     * allocation from it is already outside.
     */
    reset_test_hierarchy(&hierarchy, 1U);
    hierarchy.arena_base = SAPOTE_EARLY_PHYSICAL_LIMIT;

    return allocate_table(&hierarchy, &hierarchy.root) ==
        PAGING_STATUS_PHYSICAL_TOO_WIDE;
}

static bool test_layout_and_processor_checks(void)
{
    if (validate_kernel_layout(UINT64_C(0x100000), UINT64_C(0x500000)) !=
            PAGING_STATUS_OK ||
        validate_kernel_layout(UINT64_C(0x200000), UINT64_C(0x100000)) !=
            PAGING_STATUS_BAD_KERNEL_LAYOUT ||
        validate_kernel_layout(UINT64_C(0x100800), UINT64_C(0x200000)) !=
            PAGING_STATUS_BAD_KERNEL_LAYOUT ||
        validate_kernel_layout(UINT64_C(0x100000), UINT64_C(0x200800)) !=
            PAGING_STATUS_BAD_KERNEL_LAYOUT) {
        return false;
    }

    /* An image past the linked bound would need more regions than exist. */
    if (validate_kernel_layout(UINT64_C(0x100000),
            PAGING_KERNEL_IMAGE_LIMIT + PAGING_PAGE_SIZE) !=
        PAGING_STATUS_BAD_KERNEL_LAYOUT) {
        return false;
    }

    /* Every reason this hierarchy would be a lie on the running processor. */
    if (decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE, CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040600070406)) != PAGING_STATUS_OK) {
        return false;
    }

    if (decode_processor_support(CPUID_PAT, CPUID_EXTENDED_ROOT,
            CPUID_NO_EXECUTE, CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_NO_EXECUTE_UNSUPPORTED ||
        decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES, 0U,
            CR4_PHYSICAL_ADDRESS_EXTENSION, UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_NO_EXECUTE_UNSUPPORTED) {
        return false;
    }

    if (decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE,
            CR4_PHYSICAL_ADDRESS_EXTENSION | CR4_FIVE_LEVEL_PAGING,
            UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_FIVE_LEVEL_PAGING ||
        decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE, 0U, UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED) {
        return false;
    }

    /* Both absence of PAT and an unsafe existing layout have named refusals. */
    return decode_processor_support(0U, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE, CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040600070406)) == PAGING_STATUS_PAT_UNSUPPORTED &&
        decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE, CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040606070406)) ==
                PAGING_STATUS_PAT_LAYOUT_UNSAFE &&
        decode_processor_support(CPUID_PAT, CPUID_EXTENDED_FEATURES,
            CPUID_NO_EXECUTE, CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040600070206)) ==
                PAGING_STATUS_PAT_LAYOUT_UNSAFE;
}

static bool add_test_device_window(
    struct paging_device_windows *windows,
    enum paging_device_window_kind kind,
    uint32_t instance,
    uint64_t base,
    uint64_t length,
    enum paging_memory_type memory_type
)
{
    return paging_device_windows_add(windows, kind, instance, base, length,
        memory_type, PAGING_DEVICE_WINDOW_WRITE) == PAGING_STATUS_OK;
}

static bool make_mixed_test_device_windows(
    struct paging_device_windows *windows
)
{
    paging_device_windows_reset(windows);

    return add_test_device_window(windows, PAGING_DEVICE_WINDOW_VGA_TEXT, 0U,
            PAGING_VGA_TEXT_BUFFER_BASE, PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) &&
        add_test_device_window(windows, PAGING_DEVICE_WINDOW_LOCAL_APIC, 0U,
            UINT64_C(0xFEE00000), PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) &&
        add_test_device_window(windows, PAGING_DEVICE_WINDOW_IO_APIC, 0U,
            UINT64_C(0xFEC00000), PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) &&
        add_test_device_window(windows, PAGING_DEVICE_WINDOW_IO_APIC, 1U,
            UINT64_C(0xFEC01000), PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) &&
        add_test_device_window(windows, PAGING_DEVICE_WINDOW_PCI_ECAM, 0U,
            UINT64_C(0xE0000000), PAGING_ECAM_WINDOW_SIZE,
            PAGING_MEMORY_WRITE_BACK) &&
        add_test_device_window(windows, PAGING_DEVICE_WINDOW_FRAMEBUFFER, 0U,
            UINT64_C(0xFD1FF000), PAGING_PAGE_SIZE * 3U,
            PAGING_MEMORY_WRITE_COMBINING);
}

static bool test_device_window_registry(void)
{
    struct paging_device_windows mixed;
    struct paging_device_windows reversed;
    struct paging_device_windows normalized;
    struct paging_device_windows normalized_reversed;
    struct paging_device_windows malformed;

    if (!make_mixed_test_device_windows(&mixed) ||
        paging_device_windows_validate(&mixed, &normalized) !=
            PAGING_STATUS_OK) {
        return false;
    }

    reversed.count = mixed.count;

    for (size_t index = 0U; index < mixed.count; ++index) {
        reversed.entries[index] = mixed.entries[mixed.count - index - 1U];
    }

    if (paging_device_windows_validate(&reversed, &normalized_reversed) !=
            PAGING_STATUS_OK ||
        normalized.count != normalized_reversed.count) {
        return false;
    }

    for (size_t index = 0U; index < normalized.count; ++index) {
        if (!device_windows_equal(&normalized.entries[index],
                &normalized_reversed.entries[index])) {
            return false;
        }
    }

    /* The framebuffer crosses a 2 MiB boundary and stays WC on every page. */
    for (size_t index = 0U; index < normalized.count; ++index) {
        const struct paging_device_window *window = &normalized.entries[index];

        if ((window->kind == PAGING_DEVICE_WINDOW_LOCAL_APIC ||
                window->kind == PAGING_DEVICE_WINDOW_IO_APIC) &&
            window->memory_type != PAGING_MEMORY_UNCACHEABLE) {
            return false;
        }

        if (window->kind == PAGING_DEVICE_WINDOW_FRAMEBUFFER) {
            if (window->memory_type != PAGING_MEMORY_WRITE_COMBINING ||
                !region_needs_page_table(&normalized,
                    UINT64_C(0xFD000000), 0U, 0U) ||
                !region_needs_page_table(&normalized,
                    UINT64_C(0xFD200000), 0U, 0U)) {
                return false;
            }

            for (uint64_t offset = 0U; offset < window->length;
                 offset += PAGING_PAGE_SIZE) {
                const struct paging_device_window *found =
                    device_window_at(&normalized,
                        window->physical_base + offset);

                if (found == NULL ||
                    found->memory_type != PAGING_MEMORY_WRITE_COMBINING) {
                    return false;
                }
            }
        }
    }

    malformed = mixed;
    malformed.entries[0].length = 0U;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_ZERO_LENGTH_DEVICE_WINDOW) {
        return false;
    }

    malformed = mixed;
    malformed.entries[0].physical_base =
        UINT64_MAX - PAGING_PAGE_SIZE + 2U;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_DEVICE_WINDOW_RANGE_OVERFLOW) {
        return false;
    }

    malformed = mixed;
    malformed.count = PAGING_DEVICE_WINDOW_CAPACITY + 1U;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_TOO_MANY_DEVICE_WINDOWS) {
        return false;
    }

    malformed = mixed;
    malformed.entries[0].kind = PAGING_DEVICE_WINDOW_KIND_COUNT;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_BAD_DEVICE_WINDOW_KIND) {
        return false;
    }

    malformed = mixed;
    malformed.entries[0].memory_type = PAGING_MEMORY_TYPE_COUNT;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_UNSUPPORTED_DEVICE_WINDOW_MEMORY_TYPE) {
        return false;
    }

    malformed = mixed;
    malformed.entries[5].physical_base = PAGING_VGA_TEXT_BUFFER_BASE;

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_CONFLICTING_DEVICE_WINDOW_OVERLAP) {
        return false;
    }

    malformed = mixed;
    malformed.entries[4].physical_base = UINT64_C(0xFD000000);

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_CONFLICTING_DEVICE_WINDOW_OVERLAP) {
        return false;
    }

    malformed = mixed;
    malformed.entries[5] = malformed.entries[0];

    if (paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_DUPLICATE_DEVICE_WINDOW) {
        return false;
    }

    /* ECAM and framebuffer are optional; the three boot register kinds are not. */
    paging_device_windows_reset(&malformed);

    if (!add_test_device_window(&malformed, PAGING_DEVICE_WINDOW_VGA_TEXT, 0U,
            PAGING_VGA_TEXT_BUFFER_BASE, PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) ||
        !add_test_device_window(&malformed, PAGING_DEVICE_WINDOW_LOCAL_APIC, 0U,
            UINT64_C(0xFEE00000), PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) ||
        !add_test_device_window(&malformed, PAGING_DEVICE_WINDOW_IO_APIC, 0U,
            UINT64_C(0xFEC00000), PAGING_PAGE_SIZE,
            PAGING_MEMORY_UNCACHEABLE) ||
        paging_device_windows_validate(&malformed, &normalized) !=
            PAGING_STATUS_OK) {
        return false;
    }

    malformed.count = 2U;
    return paging_device_windows_validate(&malformed, &normalized) ==
        PAGING_STATUS_REQUIRED_DEVICE_WINDOW_MISSING;
}

/*
 * Everything above the hardware: index arithmetic, canonical form, entry
 * composition, every refusal, and a complete map, protect, translate and unmap
 * cycle against a private hierarchy the processor never runs on. It also proves
 * the audit can count a violation, because an audit that always reports zero
 * would pass every boot without checking anything.
 */
bool paging_self_test(void)
{
    struct paging_translation translation;
    struct paging_audit audit;

    if (!test_indices_and_canonical_form() || !test_entry_composition() ||
        !test_pat_model() ||
        !test_range_validation() || !test_hierarchy_operations() ||
        !test_table_reclamation() || !test_table_supply() ||
        !test_layout_and_processor_checks() ||
        !test_device_window_registry()) {
        return false;
    }

    /* Nothing the public interface offers works before initialization. */
    if (paging_is_active()) {
        return false;
    }

    if (paging_map(PAGING_PROBE_ADDRESS, 0U, PAGING_PAGE_SIZE, PAGING_WRITE) !=
            PAGING_STATUS_NOT_INITIALIZED ||
        paging_unmap(PAGING_PROBE_ADDRESS, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_NOT_INITIALIZED ||
        paging_protect(PAGING_PROBE_ADDRESS, PAGING_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_NOT_INITIALIZED ||
        paging_verify() != PAGING_STATUS_NOT_INITIALIZED) {
        return false;
    }

    if (paging_translate(PAGING_PROBE_ADDRESS, &translation) !=
            PAGING_STATUS_NOT_INITIALIZED ||
        translation.level != 0U || translation.physical_address != 0U) {
        return false;
    }

    if (paging_translate(0U, NULL) != PAGING_STATUS_NULL_ARGUMENT ||
        paging_audit_hierarchy(NULL) != PAGING_STATUS_NULL_ARGUMENT) {
        return false;
    }

    if (paging_audit_hierarchy(&audit) != PAGING_STATUS_NOT_INITIALIZED ||
        audit.leaf_count != 0U) {
        return false;
    }

    return paging_initialize(NULL) == PAGING_STATUS_NULL_ARGUMENT;
}

const char *paging_status_string(enum paging_status status)
{
    static const char *const messages[PAGING_STATUS_COUNT] = {
        "ok",
        "null paging argument",
        "page tables were installed twice",
        "kernel page tables are not installed",
        "installing page tables requires interrupts disabled",
        "processor reports no no-execute bit",
        "the no-execute bit did not take effect in EFER",
        "supervisor write protection did not take effect in CR0",
        "processor is using a five-level page hierarchy",
        "physical address extension is disabled",
        "processor reports no page attribute table",
        "existing page attribute table layout is unsafe",
        "page attribute table readback did not match",
        "linked kernel layout cannot be mapped per section",
        "device window has an unknown kind",
        "device window has an invalid instance",
        "device window requests an unsupported memory type",
        "device window requests invalid permissions",
        "device window has zero length",
        "device window address or length is unaligned",
        "device window range overflows",
        "device window is outside the supported physical range",
        "device window registry is full",
        "overlapping device windows request conflicting memory types",
        "device windows overlap",
        "device window is duplicated",
        "required device window is missing",
        "device window overlaps the linked kernel",
        "installed device window does not match the registry",
        "paging range is empty",
        "paging address or length is unaligned",
        "virtual address is not canonical",
        "paging range overflows",
        "physical address is too wide for a page table entry",
        "paging permissions name an unknown right",
        "paging request combines incompatible memory types",
        "changing a live page memory type is unsafe",
        "a page may not be writable and executable",
        "virtual page is already mapped",
        "virtual page is not mapped",
        "a larger page already covers this address",
        "no physical frame is available for a page table",
        "installed page tables do not match their intent",
        "the bounded process address space is already owned",
        "the process address-space token is stale or belongs to another object",
        "the process address space is in the wrong lifecycle state",
        "a process mapping is outside the fixed user layout or permissions",
        "the executable identity-alias lease is invalid",
        "the bounded supervisor mapping-intent registry is full"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) == PAGING_STATUS_COUNT,
        "paging status messages are out of sync"
    );

    if (status < PAGING_STATUS_OK || status >= PAGING_STATUS_COUNT) {
        return "unknown paging status";
    }

    return messages[status];
}

const char *paging_memory_type_string(enum paging_memory_type memory_type)
{
    static const char *const names[PAGING_MEMORY_TYPE_COUNT] = {
        "write-back",
        "write-combining",
        "uncacheable",
        "write-through",
        "write-protected",
        "uncached-minus",
        "invalid"
    };

    _Static_assert(
        sizeof(names) / sizeof(names[0]) == PAGING_MEMORY_TYPE_COUNT,
        "paging memory type names are out of sync"
    );

    if (memory_type < PAGING_MEMORY_WRITE_BACK ||
        memory_type >= PAGING_MEMORY_TYPE_COUNT) {
        return "unknown";
    }

    return names[memory_type];
}

const char *paging_device_window_kind_string(
    enum paging_device_window_kind kind
)
{
    static const char *const names[PAGING_DEVICE_WINDOW_KIND_COUNT] = {
        "VGA",
        "local APIC",
        "I/O APIC",
        "PCI ECAM",
        "framebuffer"
    };

    _Static_assert(
        sizeof(names) / sizeof(names[0]) == PAGING_DEVICE_WINDOW_KIND_COUNT,
        "device-window kind names are out of sync"
    );

    if (kind < PAGING_DEVICE_WINDOW_VGA_TEXT ||
        kind >= PAGING_DEVICE_WINDOW_KIND_COUNT) {
        return "unknown device window";
    }

    return names[kind];
}
