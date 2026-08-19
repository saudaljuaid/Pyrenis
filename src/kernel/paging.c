/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/boot.h>
#include <seneri/cpu.h>
#include <seneri/memory.h>
#include <seneri/paging.h>

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
 * installs would be advisory for ring 0 - which is the only ring Seneri has.
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
 * Intel SDM volume 3A section 12.12.2: IA32_PAT is MSR 0x277 and holds eight
 * one-byte memory types. With the PAT bit of a leaf clear, PWT and PCD select
 * entries 0 through 3, so PCD together with PWT selects entry 3. Section
 * 12.12.4 gives the power-on value, in which entry 3 is uncacheable. Seneri
 * checks that entry rather than reprogramming the register, because a write to
 * IA32_PAT changes the meaning of every mapping already in place.
 */
#define IA32_PAT_MSR UINT32_C(0x00000277)
#define PAT_UNCACHEABLE UINT64_C(0x00)
#define PAT_DEVICE_ENTRY 3U

/*
 * Intel SDM volume 3A section 4.1.4 and AMD APM volume 2 section 5.4.1: the
 * no-execute bit is reported by CPUID.80000001H:EDX[20], and leaf 0x80000000
 * reports how far the extended leaves go, so it must be asked first.
 */
#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_NO_EXECUTE UINT32_C(0x00100000)

/*
 * console.c writes the VGA text buffer directly. It is device memory sharing a
 * page with nothing else, so it is mapped uncacheable for the same reason the
 * APIC windows are.
 */
#define VGA_TEXT_BUFFER UINT64_C(0x000B8000)

/* One 4 KiB register window each, per Intel SDM volume 3A sections 11.4.1
   and 12.11.1. */
#define DEVICE_WINDOW_SIZE PAGING_PAGE_SIZE

/*
 * linker.ld refuses to link an image that ends above this, so the kernel can
 * never span more 2 MiB regions than there is storage for. The runtime check in
 * collect_kernel_regions is what the self-test drives; this assertion is what
 * keeps the two bounds from drifting apart.
 */
#define PAGING_KERNEL_IMAGE_LIMIT UINT64_C(0x800000)
#define PAGING_MAX_KERNEL_FINE_REGIONS 4U
#define PAGING_MAX_FINE_REGIONS \
    (PAGING_MAX_KERNEL_FINE_REGIONS + 3U + ACPI_MAX_IO_APICS + \
        PAGING_MAX_FRAMEBUFFER_REGIONS)
#define PAGING_MAX_SECTIONS (4U + 4U + ACPI_MAX_IO_APICS)

_Static_assert(
    PAGING_KERNEL_IMAGE_LIMIT <=
        PAGING_MAX_KERNEL_FINE_REGIONS * PAGING_HUGE_PAGE_SIZE,
    "the linked kernel bound needs more fine regions than paging reserves"
);

/*
 * Every 2 MiB region that is not the kernel's is a device window: the local
 * APIC, the VGA text buffer, one per firmware-declared I/O APIC, and at most one
 * PCI Express configuration window. The topology is already bounded by
 * acpi_topology_discover and the configuration window is at most one region, so
 * a region array this wide cannot overflow and needs no runtime capacity branch.
 */
_Static_assert(
    PAGING_MAX_FINE_REGIONS - PAGING_MAX_KERNEL_FINE_REGIONS >=
        3U + ACPI_MAX_IO_APICS + PAGING_MAX_FRAMEBUFFER_REGIONS,
    "fine region storage no longer covers every device window"
);
_Static_assert(
    PAGING_MAX_SECTIONS >= 4U + 4U + ACPI_MAX_IO_APICS,
    "section storage no longer covers the kernel and every device window"
);
_Static_assert(
    PAGING_ECAM_WINDOW_SIZE == PAGING_HUGE_PAGE_SIZE,
    "the configuration window is no longer exactly one identity map region"
);
_Static_assert(
    SENERI_EARLY_PHYSICAL_LIMIT % PAGING_HUGE_PAGE_SIZE == 0U,
    "the identity window is not a whole number of 2 MiB pages"
);
_Static_assert(
    PAGING_PROBE_ADDRESS >= SENERI_EARLY_PHYSICAL_LIMIT,
    "the paging probe page would collide with the identity window"
);

/* The private hierarchy paging_self_test builds: a root and five tables. */
#define PAGING_TEST_ARENA_PAGES 6U
#define PAGING_TEST_ENTRIES \
    (PAGING_TEST_ARENA_PAGES * (PAGING_PAGE_SIZE / sizeof(uint64_t)))

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
static uint64_t fine_regions[PAGING_MAX_FINE_REGIONS];
static struct paging_section kernel_sections[PAGING_MAX_SECTIONS];
static size_t fine_region_count;
static size_t kernel_section_count;
static uint64_t test_arena[PAGING_TEST_ENTRIES] __attribute__((aligned(4096)));

/*
 * Every table frame is below SENERI_EARLY_PHYSICAL_LIMIT and that whole range
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

    return flags;
}

static uint32_t flags_to_permissions(uint64_t entry)
{
    uint32_t permissions = PAGING_READ;

    if ((entry & PAGE_WRITABLE) != 0U) {
        permissions |= PAGING_WRITE;
    }

    if ((entry & PAGE_NO_EXECUTE) == 0U) {
        permissions |= PAGING_EXECUTE;
    }

    if ((entry & PAGE_CACHE_DISABLE) != 0U) {
        permissions |= PAGING_UNCACHED;
    }

    return permissions;
}

static enum paging_status validate_permissions(uint32_t permissions)
{
    const uint32_t known = PAGING_WRITE | PAGING_EXECUTE | PAGING_UNCACHED;

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
    if ((uint64_t)frame >= SENERI_EARLY_PHYSICAL_LIMIT) {
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
    uint32_t permissions
)
{
    enum paging_status status = validate_permissions(permissions);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = validate_virtual_range(virtual_address, length, PAGING_PAGE_SIZE);

    if (status != PAGING_STATUS_OK) {
        return status;
    }

    status = require_mapped_range(hierarchy, virtual_address, length);

    if (status != PAGING_STATUS_OK) {
        return status;
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
    bool executable
)
{
    const uint64_t page_size = level_page_size(level);
    uint32_t permissions = PAGING_READ;

    if (writable) {
        permissions |= PAGING_WRITE;
    }

    if (executable) {
        permissions |= PAGING_EXECUTE;
    }

    if ((entry & PAGE_CACHE_DISABLE) != 0U) {
        permissions |= PAGING_UNCACHED;
    }

    translation->physical_address =
        (entry & PAGE_FRAME_MASK & ~(page_size - 1U)) |
        (virtual_address & (page_size - 1U));
    translation->permissions = permissions;
    translation->level = level;
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
    struct paging_translation *translation
)
{
    uint64_t table = hierarchy->root;
    uint64_t entry;
    bool writable = true;
    bool executable = true;

    translation->physical_address = 0U;
    translation->permissions = PAGING_READ;
    translation->level = 0U;

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

        if ((entry & PAGE_HUGE) != 0U) {
            fill_translation(translation, entry, virtual_address, level,
                writable, executable);
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
    fill_translation(translation, entry, virtual_address, 1U, writable,
        executable);
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
    uint32_t extended_root_eax,
    uint32_t extended_features_edx,
    uint64_t cr4,
    uint64_t pat
)
{
    const unsigned int pat_shift = 8U * PAT_DEVICE_ENTRY;

    if (extended_root_eax < CPUID_EXTENDED_FEATURES ||
        (extended_features_edx & CPUID_NO_EXECUTE) == 0U) {
        return PAGING_STATUS_NO_EXECUTE_UNSUPPORTED;
    }

    if ((cr4 & CR4_FIVE_LEVEL_PAGING) != 0U) {
        return PAGING_STATUS_FIVE_LEVEL_PAGING;
    }

    if ((cr4 & CR4_PHYSICAL_ADDRESS_EXTENSION) == 0U) {
        return PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED;
    }

    if (((pat >> pat_shift) & UINT64_C(0xFF)) != PAT_UNCACHEABLE) {
        return PAGING_STATUS_CACHE_POLICY_UNAVAILABLE;
    }

    return PAGING_STATUS_OK;
}

/*
 * The 2 MiB regions that get 4 KiB granularity. The kernel needs it because all
 * four of its load segments share one such region and would otherwise share one
 * permission; the device windows need it so one register page can be made
 * uncacheable without making a whole 2 MiB of the address space uncacheable.
 */
static enum paging_status collect_kernel_regions(
    uint64_t kernel_start,
    uint64_t kernel_end,
    uint64_t *regions,
    size_t *count
)
{
    *count = 0U;

    if (kernel_start >= kernel_end) {
        return PAGING_STATUS_BAD_KERNEL_LAYOUT;
    }

    if ((kernel_start & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (kernel_end & (PAGING_PAGE_SIZE - 1U)) != 0U) {
        return PAGING_STATUS_BAD_KERNEL_LAYOUT;
    }

    if (kernel_end > PAGING_KERNEL_IMAGE_LIMIT) {
        return PAGING_STATUS_BAD_KERNEL_LAYOUT;
    }

    for (uint64_t base = kernel_start & ~(PAGING_HUGE_PAGE_SIZE - 1U);
         base < kernel_end; base += PAGING_HUGE_PAGE_SIZE) {
        regions[*count] = base;
        ++*count;
    }

    return PAGING_STATUS_OK;
}

static void add_region(uint64_t *regions, size_t *count, uint64_t address)
{
    const uint64_t base = address & ~(PAGING_HUGE_PAGE_SIZE - 1U);

    for (size_t index = 0; index < *count; ++index) {
        if (regions[index] == base) {
            return;
        }
    }

    regions[*count] = base;
    ++*count;
}

static bool region_listed(
    const uint64_t *regions,
    size_t count,
    uint64_t base
)
{
    for (size_t index = 0; index < count; ++index) {
        if (regions[index] == base) {
            return true;
        }
    }

    return false;
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

/*
 * Decide whether a firmware-declared configuration window can become a device
 * region, and return its base if it can. Three things have to be true, and none
 * of them failing is an error: a machine may declare no window at all, and a
 * window Seneri cannot carve out is one it reads through the I/O ports instead.
 *
 * The window must start on a 2 MiB boundary, because a device region is one
 * whole region of the identity map and a window straddling two would need both.
 * It must lie inside the early identity window, because that is the only
 * physical memory this map covers. And only the first declared window is taken:
 * nothing in Seneri yet has a second segment group to address.
 */
static uint64_t select_ecam_window(const struct acpi_mcfg *mcfg)
{
    uint64_t base;

    if (mcfg == NULL || mcfg->allocation_count == 0U) {
        return 0U;
    }

    base = mcfg->allocations[0].base_address;

    if (base == 0U || base % PAGING_HUGE_PAGE_SIZE != 0U) {
        return 0U;
    }

    if (base > SENERI_EARLY_PHYSICAL_LIMIT - PAGING_ECAM_WINDOW_SIZE) {
        return 0U;
    }

    return base;
}

/*
 * The span of identity map a framebuffer needs, rounded out to whole 2 MiB
 * regions because that is the unit a device window is carved in. Returns zero
 * when there is nothing to map or when what the loader set will not fit inside
 * the bound above - a framebuffer that is partly uncacheable would be worse
 * than one that is not mapped at all, because half of it would work.
 */
static uint64_t framebuffer_span(
    const struct boot_framebuffer *framebuffer,
    uint64_t *base_out
)
{
    uint64_t base;
    uint64_t end;
    uint64_t span;

    *base_out = 0U;

    if (framebuffer == NULL || !framebuffer->present ||
        framebuffer->size == 0U) {
        return 0U;
    }

    base = framebuffer->address & ~(PAGING_HUGE_PAGE_SIZE - 1U);
    end = framebuffer->address + framebuffer->size;

    if (end > SENERI_EARLY_PHYSICAL_LIMIT) {
        return 0U;
    }

    /* Round the end up to the region it falls in. */
    end = (end + PAGING_HUGE_PAGE_SIZE - 1U) &
        ~(PAGING_HUGE_PAGE_SIZE - 1U);
    span = end - base;

    if (span > (uint64_t)PAGING_MAX_FRAMEBUFFER_REGIONS *
        PAGING_HUGE_PAGE_SIZE) {
        return 0U;
    }

    *base_out = base;
    return span;
}

static void collect_sections(
    const struct acpi_topology *topology,
    const struct acpi_mcfg *mcfg,
    const struct boot_framebuffer *framebuffer,
    struct paging_section *sections,
    size_t *count
)
{
    const uint64_t kernel_start = (uint64_t)(uintptr_t)__kernel_start;
    const uint64_t text_start = (uint64_t)(uintptr_t)__text_start;
    const uint32_t device = PAGING_WRITE | PAGING_UNCACHED;
    uint64_t ecam;

    *count = 0U;

    /* The Multiboot2 header: read by the boot loader, never by the kernel. */
    add_section(sections, count, kernel_start, text_start, PAGING_READ);
    add_section(sections, count, text_start, (uint64_t)(uintptr_t)__text_end,
        PAGING_EXECUTE);
    add_section(sections, count, (uint64_t)(uintptr_t)__rodata_start,
        (uint64_t)(uintptr_t)__rodata_end, PAGING_READ);
    add_section(sections, count, (uint64_t)(uintptr_t)__data_start,
        (uint64_t)(uintptr_t)__kernel_end, PAGING_WRITE);
    add_section(sections, count, VGA_TEXT_BUFFER,
        VGA_TEXT_BUFFER + DEVICE_WINDOW_SIZE, device);
    add_section(sections, count, topology->local_apic_address,
        topology->local_apic_address + DEVICE_WINDOW_SIZE, device);

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        const uint64_t address = topology->io_apics[index].address;

        add_section(sections, count, address, address + DEVICE_WINDOW_SIZE,
            device);
    }

    /*
     * Configuration space is device memory and has to be read as device memory.
     * A cached read of a configuration register would be answered from a line
     * fetched at some earlier unrelated moment, so the window is uncacheable for
     * exactly the reason the APIC windows are.
     */
    ecam = select_ecam_window(mcfg);

    if (ecam != 0U) {
        add_section(sections, count, ecam, ecam + PAGING_ECAM_WINDOW_SIZE,
            device);
    }

    /*
     * The framebuffer is device memory for the same reason the APIC windows
     * are, and one section covers however many regions it spans.
     */
    {
        uint64_t framebuffer_base = 0U;
        const uint64_t span = framebuffer_span(framebuffer, &framebuffer_base);

        if (span != 0U) {
            add_section(sections, count, framebuffer_base,
                framebuffer_base + span, device);
        }
    }
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
    uint64_t address
)
{
    for (size_t index = 0; index < count; ++index) {
        if (address >= sections[index].start && address < sections[index].end) {
            return sections[index].permissions;
        }
    }

    return PAGING_WRITE;
}

static enum paging_status build_identity_map(struct page_hierarchy *hierarchy)
{
    for (size_t index = 0; index < fine_region_count; ++index) {
        for (uint64_t offset = 0U; offset < PAGING_HUGE_PAGE_SIZE;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = fine_regions[index] + offset;
            enum paging_status status;

            /*
             * The null page stays absent. Nothing in Seneri reads physical
             * address zero, and leaving it unmapped turns a null dereference
             * into a page fault naming CR2 = 0 instead of a silent read of the
             * real-mode interrupt vector table.
             */
            if (address == 0U) {
                continue;
            }

            status = map_range(hierarchy, address, address, PAGING_PAGE_SIZE,
                section_permissions(kernel_sections, kernel_section_count,
                    address), 1U);

            if (status != PAGING_STATUS_OK) {
                return status;
            }
        }
    }

    for (uint64_t base = 0U; base < SENERI_EARLY_PHYSICAL_LIMIT;
         base += PAGING_HUGE_PAGE_SIZE) {
        enum paging_status status;

        if (region_listed(fine_regions, fine_region_count, base)) {
            continue;
        }

        status = map_range(hierarchy, base, base, PAGING_HUGE_PAGE_SIZE,
            PAGING_WRITE, 2U);

        if (status != PAGING_STATUS_OK) {
            return status;
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
    struct page_hierarchy *hierarchy
)
{
    struct paging_translation translation;

    for (size_t index = 0; index < fine_region_count; ++index) {
        for (uint64_t offset = 0U; offset < PAGING_HUGE_PAGE_SIZE;
             offset += PAGING_PAGE_SIZE) {
            const uint64_t address = fine_regions[index] + offset;
            uint32_t expected;

            if (address == 0U) {
                if (translate_address(hierarchy, address, &translation) !=
                    PAGING_STATUS_NOT_MAPPED) {
                    return PAGING_STATUS_VALIDATION_FAILURE;
                }

                continue;
            }

            expected = section_permissions(kernel_sections,
                kernel_section_count, address);

            if (translate_address(hierarchy, address, &translation) !=
                    PAGING_STATUS_OK ||
                translation.physical_address != address ||
                translation.permissions != expected ||
                translation.level != 1U) {
                return PAGING_STATUS_VALIDATION_FAILURE;
            }
        }
    }

    for (uint64_t base = 0U; base < SENERI_EARLY_PHYSICAL_LIMIT;
         base += PAGING_HUGE_PAGE_SIZE) {
        if (region_listed(fine_regions, fine_region_count, base)) {
            continue;
        }

        if (translate_address(hierarchy, base, &translation) !=
                PAGING_STATUS_OK ||
            translation.physical_address != base ||
            translation.permissions != PAGING_WRITE ||
            translation.level != 2U) {
            return PAGING_STATUS_VALIDATION_FAILURE;
        }
    }

    return PAGING_STATUS_OK;
}

static void reset_state(void)
{
    state.root_physical_address = 0U;
    state.table_frames = 0U;
    state.fine_regions = 0U;
    state.ecam_window_base = 0U;
    state.ecam_window_size = 0U;
    state.framebuffer_base = 0U;
    state.framebuffer_size = 0U;
    state.framebuffer_regions = 0U;
    state.no_execute_active = false;
    state.write_protect_active = false;
    state.active = false;
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

enum paging_status paging_initialize(
    const struct acpi_topology *topology,
    const struct acpi_mcfg *mcfg,
    const struct boot_framebuffer *framebuffer
)
{
    const uint64_t ecam = select_ecam_window(mcfg);
    uint64_t framebuffer_base = 0U;
    const uint64_t framebuffer_bytes =
        framebuffer_span(framebuffer, &framebuffer_base);
    struct cpuid_result extended;
    struct paging_audit audit;
    uint32_t extended_features_edx = 0U;
    uint32_t extended_root_eax;
    uint64_t previous_root;
    uint64_t root = 0U;
    enum paging_status status;

    if (topology == NULL) {
        return PAGING_STATUS_NULL_ARGUMENT;
    }

    if (state.active) {
        return PAGING_STATUS_ALREADY_INITIALIZED;
    }

    /*
     * A page fault taken between the CR3 load and the validation that follows
     * it has nowhere to report from, so nothing may interrupt the switch.
     */
    if (cpu_interrupts_enabled()) {
        return PAGING_STATUS_INTERRUPTS_ENABLED;
    }

    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &extended);
    extended_root_eax = extended.eax;

    if (extended_root_eax >= CPUID_EXTENDED_FEATURES) {
        cpu_cpuid(CPUID_EXTENDED_FEATURES, 0U, &extended);
        extended_features_edx = extended.edx;
    }

    status = decode_processor_support(extended_root_eax, extended_features_edx,
        cpu_read_cr4(), cpu_read_msr(IA32_PAT_MSR));

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    status = enable_processor_features();

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    status = collect_kernel_regions((uint64_t)(uintptr_t)__kernel_start,
        (uint64_t)(uintptr_t)__kernel_end, fine_regions, &fine_region_count);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    add_region(fine_regions, &fine_region_count, VGA_TEXT_BUFFER);
    add_region(fine_regions, &fine_region_count, topology->local_apic_address);

    for (size_t index = 0; index < topology->io_apic_count; ++index) {
        add_region(fine_regions, &fine_region_count,
            topology->io_apics[index].address);
    }

    if (ecam != 0U) {
        add_region(fine_regions, &fine_region_count, ecam);
    }

    for (uint64_t offset = 0U; offset < framebuffer_bytes;
         offset += PAGING_HUGE_PAGE_SIZE) {
        add_region(fine_regions, &fine_region_count,
            framebuffer_base + offset);
    }

    collect_sections(topology, mcfg, framebuffer, kernel_sections,
        &kernel_section_count);

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
    status = build_identity_map(&live_hierarchy);

    if (status != PAGING_STATUS_OK) {
        reset_state();
        return status;
    }

    status = validate_identity_map(&live_hierarchy);

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

    previous_root = cpu_read_cr3();
    state.root_physical_address = live_hierarchy.root;
    state.table_frames = live_hierarchy.table_frames;
    state.fine_regions = fine_region_count;
    state.ecam_window_base = ecam;
    state.ecam_window_size = ecam == 0U ? 0U : PAGING_ECAM_WINDOW_SIZE;
    state.framebuffer_base = framebuffer_bytes == 0U ? 0U : framebuffer_base;
    state.framebuffer_size = framebuffer_bytes;
    state.framebuffer_regions =
        (size_t)(framebuffer_bytes / PAGING_HUGE_PAGE_SIZE);
    state.no_execute_active = true;
    state.write_protect_active = true;
    state.active = true;
    live_hierarchy.live = true;

    cpu_write_cr3(live_hierarchy.root);
    status = paging_verify();

    if (status != PAGING_STATUS_OK) {
        /*
         * The boot hierarchy is still intact and still maps everything, so
         * going back to it is the one recovery available. Reporting a status
         * from the old address space beats halting inside the new one.
         */
        cpu_write_cr3(previous_root);
        live_hierarchy.live = false;
        reset_state();
        return status;
    }

    return PAGING_STATUS_OK;
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
    state.table_frames = live_hierarchy.table_frames;
    return status;
}

enum paging_status paging_unmap(uint64_t virtual_address, uint64_t length)
{
    enum paging_status status;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    status = unmap_range(&live_hierarchy, virtual_address, length);

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
    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    return protect_range(&live_hierarchy, virtual_address, length, permissions);
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
    translation->level = 0U;

    if (!state.active) {
        return PAGING_STATUS_NOT_INITIALIZED;
    }

    return translate_address(&live_hierarchy, virtual_address, translation);
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

        if (translate_address(&live_hierarchy, address, &translation) !=
                PAGING_STATUS_OK ||
            translation.physical_address != address ||
            translation.permissions != expected[index].permissions) {
            return PAGING_STATUS_VALIDATION_FAILURE;
        }
    }

    if (translate_address(&live_hierarchy, 0U, &translation) !=
        PAGING_STATUS_NOT_MAPPED) {
        return PAGING_STATUS_VALIDATION_FAILURE;
    }

    return PAGING_STATUS_OK;
}

struct paging_state paging_get_state(void)
{
    return state;
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
        PAGING_WRITE | PAGING_UNCACHED,
        PAGING_EXECUTE | PAGING_UNCACHED
    };

    const uint64_t frame = UINT64_C(0x000ABCDE12345000);

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const uint64_t entry = frame | permissions_to_flags(cases[index]);

        if ((entry & PAGE_FRAME_MASK) != frame ||
            (entry & PAGE_PRESENT) == 0U ||
            (entry & PAGE_USER) != 0U ||
            flags_to_permissions(entry) != cases[index]) {
            return false;
        }
    }

    /* Read-only means no-execute is set, not merely that write is clear. */
    if ((permissions_to_flags(PAGING_READ) & PAGE_NO_EXECUTE) == 0U ||
        (permissions_to_flags(PAGING_EXECUTE) & PAGE_NO_EXECUTE) != 0U ||
        (permissions_to_flags(PAGING_UNCACHED) & PAGE_CACHE_DISABLE) == 0U) {
        return false;
    }

    return validate_permissions(PAGING_WRITE | PAGING_EXECUTE) ==
            PAGING_STATUS_WRITABLE_AND_EXECUTABLE &&
        validate_permissions(1U << 3) == PAGING_STATUS_BAD_PERMISSIONS &&
        validate_permissions(PAGING_WRITE | PAGING_UNCACHED) ==
            PAGING_STATUS_OK;
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
    if (translate_address(&hierarchy, address, &translation) !=
            PAGING_STATUS_NOT_MAPPED ||
        translation.level != 0U ||
        translation.physical_address != 0U ||
        unmap_range(&hierarchy, address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED ||
        protect_range(&hierarchy, address, PAGING_PAGE_SIZE, PAGING_WRITE) !=
            PAGING_STATUS_NOT_MAPPED) {
        return false;
    }

    if (map_range(&hierarchy, address, frame, PAGING_PAGE_SIZE, PAGING_WRITE,
            1U) != PAGING_STATUS_OK) {
        return false;
    }

    if (translate_address(&hierarchy, address + 0x40U, &translation) !=
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
            PAGING_WRITE | PAGING_EXECUTE) !=
        PAGING_STATUS_WRITABLE_AND_EXECUTABLE) {
        return false;
    }

    if (protect_range(&hierarchy, address, PAGING_PAGE_SIZE, PAGING_READ) !=
            PAGING_STATUS_OK ||
        translate_address(&hierarchy, address, &translation) !=
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
        translate_address(&hierarchy, address, &translation) !=
            PAGING_STATUS_NOT_MAPPED ||
        unmap_range(&hierarchy, address, PAGING_PAGE_SIZE) !=
            PAGING_STATUS_NOT_MAPPED) {
        return false;
    }

    if (map_range(&hierarchy, huge_address, PAGING_HUGE_PAGE_SIZE,
            PAGING_HUGE_PAGE_SIZE, PAGING_WRITE, 2U) != PAGING_STATUS_OK) {
        return false;
    }

    if (translate_address(&hierarchy, huge_address + 0x1000U, &translation) !=
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
            PAGING_READ) != PAGING_STATUS_HUGE_PAGE_PRESENT) {
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
        translate_address(&hierarchy, second, &translation) !=
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
        translate_address(&hierarchy, first, &translation) !=
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
        translate_address(&hierarchy, first, &translation) !=
            PAGING_STATUS_OK) {
        return false;
    }

    /* And undoing the last one collapses the rest of the path. */
    return unmap_range(&hierarchy, first, PAGING_PAGE_SIZE) ==
            PAGING_STATUS_OK &&
        hierarchy.table_frames == 1U &&
        translate_address(&hierarchy, far, &translation) ==
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
    hierarchy.arena_base = SENERI_EARLY_PHYSICAL_LIMIT;

    return allocate_table(&hierarchy, &hierarchy.root) ==
        PAGING_STATUS_PHYSICAL_TOO_WIDE;
}

static bool test_layout_and_processor_checks(void)
{
    uint64_t regions[PAGING_MAX_FINE_REGIONS];
    size_t count = 0U;

    /* An image spanning three 2 MiB regions produces exactly three. */
    if (collect_kernel_regions(UINT64_C(0x100000), UINT64_C(0x500000), regions,
            &count) != PAGING_STATUS_OK ||
        count != 3U || regions[0] != 0U ||
        regions[1] != PAGING_HUGE_PAGE_SIZE ||
        regions[2] != PAGING_HUGE_PAGE_SIZE * 2U) {
        return false;
    }

    if (collect_kernel_regions(UINT64_C(0x200000), UINT64_C(0x100000), regions,
            &count) != PAGING_STATUS_BAD_KERNEL_LAYOUT ||
        count != 0U) {
        return false;
    }

    if (collect_kernel_regions(UINT64_C(0x100800), UINT64_C(0x200000), regions,
            &count) != PAGING_STATUS_BAD_KERNEL_LAYOUT ||
        collect_kernel_regions(UINT64_C(0x100000), UINT64_C(0x200800), regions,
            &count) != PAGING_STATUS_BAD_KERNEL_LAYOUT) {
        return false;
    }

    /* An image past the linked bound would need more regions than exist. */
    if (collect_kernel_regions(UINT64_C(0x100000),
            PAGING_KERNEL_IMAGE_LIMIT + PAGING_PAGE_SIZE, regions, &count) !=
        PAGING_STATUS_BAD_KERNEL_LAYOUT) {
        return false;
    }

    count = 0U;
    add_region(regions, &count, UINT64_C(0xFEE00000));
    add_region(regions, &count, UINT64_C(0xFEE00FFF));
    add_region(regions, &count, UINT64_C(0xFEC00000));

    if (count != 2U || regions[0] != UINT64_C(0xFEE00000) ||
        regions[1] != UINT64_C(0xFEC00000) ||
        !region_listed(regions, count, UINT64_C(0xFEC00000)) ||
        region_listed(regions, count, 0U)) {
        return false;
    }

    /* Every reason this hierarchy would be a lie on the running processor. */
    if (decode_processor_support(CPUID_EXTENDED_FEATURES, CPUID_NO_EXECUTE,
            CR4_PHYSICAL_ADDRESS_EXTENSION,
            UINT64_C(0x0007040600070406)) != PAGING_STATUS_OK) {
        return false;
    }

    if (decode_processor_support(CPUID_EXTENDED_ROOT, CPUID_NO_EXECUTE,
            CR4_PHYSICAL_ADDRESS_EXTENSION, UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_NO_EXECUTE_UNSUPPORTED ||
        decode_processor_support(CPUID_EXTENDED_FEATURES, 0U,
            CR4_PHYSICAL_ADDRESS_EXTENSION, UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_NO_EXECUTE_UNSUPPORTED) {
        return false;
    }

    if (decode_processor_support(CPUID_EXTENDED_FEATURES, CPUID_NO_EXECUTE,
            CR4_PHYSICAL_ADDRESS_EXTENSION | CR4_FIVE_LEVEL_PAGING,
            UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_FIVE_LEVEL_PAGING ||
        decode_processor_support(CPUID_EXTENDED_FEATURES, CPUID_NO_EXECUTE, 0U,
            UINT64_C(0x0007040600070406)) !=
            PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED) {
        return false;
    }

    /* A PAT whose entry 3 is write-back would make every device page cached. */
    return decode_processor_support(CPUID_EXTENDED_FEATURES, CPUID_NO_EXECUTE,
        CR4_PHYSICAL_ADDRESS_EXTENSION, UINT64_C(0x0007040606070406)) ==
        PAGING_STATUS_CACHE_POLICY_UNAVAILABLE;
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
        !test_range_validation() || !test_hierarchy_operations() ||
        !test_table_reclamation() || !test_table_supply() ||
        !test_layout_and_processor_checks()) {
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

    return paging_initialize(NULL, NULL, NULL) ==
        PAGING_STATUS_NULL_ARGUMENT;
}

const char *paging_status_string(enum paging_status status)
{
    switch (status) {
    case PAGING_STATUS_OK:
        return "ok";
    case PAGING_STATUS_NULL_ARGUMENT:
        return "null paging argument";
    case PAGING_STATUS_ALREADY_INITIALIZED:
        return "page tables were installed twice";
    case PAGING_STATUS_NOT_INITIALIZED:
        return "kernel page tables are not installed";
    case PAGING_STATUS_INTERRUPTS_ENABLED:
        return "installing page tables requires interrupts disabled";
    case PAGING_STATUS_NO_EXECUTE_UNSUPPORTED:
        return "processor reports no no-execute bit";
    case PAGING_STATUS_NO_EXECUTE_INACTIVE:
        return "the no-execute bit did not take effect in EFER";
    case PAGING_STATUS_WRITE_PROTECT_INACTIVE:
        return "supervisor write protection did not take effect in CR0";
    case PAGING_STATUS_FIVE_LEVEL_PAGING:
        return "processor is using a five-level page hierarchy";
    case PAGING_STATUS_PHYSICAL_EXTENSION_DISABLED:
        return "physical address extension is disabled";
    case PAGING_STATUS_CACHE_POLICY_UNAVAILABLE:
        return "no uncacheable page attribute entry is available";
    case PAGING_STATUS_BAD_KERNEL_LAYOUT:
        return "linked kernel layout cannot be mapped per section";
    case PAGING_STATUS_ZERO_LENGTH:
        return "paging range is empty";
    case PAGING_STATUS_UNALIGNED_ADDRESS:
        return "paging address or length is unaligned";
    case PAGING_STATUS_NONCANONICAL_ADDRESS:
        return "virtual address is not canonical";
    case PAGING_STATUS_RANGE_OVERFLOW:
        return "paging range overflows";
    case PAGING_STATUS_PHYSICAL_TOO_WIDE:
        return "physical address is too wide for a page table entry";
    case PAGING_STATUS_BAD_PERMISSIONS:
        return "paging permissions name an unknown right";
    case PAGING_STATUS_WRITABLE_AND_EXECUTABLE:
        return "a page may not be writable and executable";
    case PAGING_STATUS_ALREADY_MAPPED:
        return "virtual page is already mapped";
    case PAGING_STATUS_NOT_MAPPED:
        return "virtual page is not mapped";
    case PAGING_STATUS_HUGE_PAGE_PRESENT:
        return "a larger page already covers this address";
    case PAGING_STATUS_OUT_OF_FRAMES:
        return "no physical frame is available for a page table";
    case PAGING_STATUS_VALIDATION_FAILURE:
        return "installed page tables do not match their intent";
    default:
        return "unknown paging status";
    }
}
