/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_PAGING_H
#define SENERI_PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>

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

/*
 * The virtual page the paging scenario and the boot lifecycle proof map a
 * freshly allocated frame at. It sits above the identity window so a mapping
 * made here cannot collide with one, and clear of 0x100000000, which the
 * page-fault scenario needs to stay absent.
 */
#define PAGING_PROBE_ADDRESS UINT64_C(0x0000000200000000)

/*
 * How much of a firmware-declared PCI Express configuration window Seneri makes
 * uncacheable and therefore readable as configuration space. One 2 MiB region,
 * because that is the unit the identity map is carved in: a device window is a
 * whole such region turned into 4 KiB pages, and one region is two buses of
 * configuration space, which is every bus any machine Seneri is tested on
 * populates. Reaching further needs the window mapped somewhere of its own, and
 * that is a later increment; src/kernel/pci.c reads no further than this.
 */
#define PAGING_ECAM_WINDOW_SIZE PAGING_HUGE_PAGE_SIZE

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
    PAGING_STATUS_CACHE_POLICY_UNAVAILABLE,
    PAGING_STATUS_BAD_KERNEL_LAYOUT,
    PAGING_STATUS_ZERO_LENGTH,
    PAGING_STATUS_UNALIGNED_ADDRESS,
    PAGING_STATUS_NONCANONICAL_ADDRESS,
    PAGING_STATUS_RANGE_OVERFLOW,
    PAGING_STATUS_PHYSICAL_TOO_WIDE,
    PAGING_STATUS_BAD_PERMISSIONS,
    PAGING_STATUS_WRITABLE_AND_EXECUTABLE,
    PAGING_STATUS_ALREADY_MAPPED,
    PAGING_STATUS_NOT_MAPPED,
    PAGING_STATUS_HUGE_PAGE_PRESENT,
    PAGING_STATUS_OUT_OF_FRAMES,
    PAGING_STATUS_VALIDATION_FAILURE
};

/*
 * A mapping is described by what it permits, never by raw entry bits. Read
 * access is implied by presence, so the absence of every flag is a read-only,
 * non-executable, write-back page. There is deliberately no user flag: every
 * entry Seneri writes is supervisor-only, and a request naming an unknown bit
 * is refused rather than masked.
 */
enum paging_permissions {
    PAGING_READ = 0U,
    PAGING_WRITE = 1U << 0,
    PAGING_EXECUTE = 1U << 1,
    PAGING_UNCACHED = 1U << 2
};

struct paging_state {
    uint64_t root_physical_address;
    size_t table_frames;
    size_t fine_regions;
    /*
     * Where the configuration window was made uncacheable, or zero when it was
     * not. Paging owns the address space, so paging decides whether a window
     * firmware declared can be reached at all, and src/kernel/pci.c reads that
     * decision rather than making its own.
     */
    uint64_t ecam_window_base;
    uint64_t ecam_window_size;
    bool no_execute_active;
    bool write_protect_active;
    bool active;
};

struct paging_translation {
    uint64_t physical_address;
    uint32_t permissions;
    unsigned int level;
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

/*
 * The MCFG may be NULL: a machine that declares no configuration window still
 * gets an address space, and PCI configuration space is still reachable through
 * the I/O ports. A window that is declared but cannot be carved into a device
 * region - not 2 MiB aligned, or outside the early identity window - is not an
 * error either. It is simply not mapped, ecam_window_base stays zero, and the
 * caller finds out by asking rather than by faulting.
 */
enum paging_status paging_initialize(
    const struct acpi_topology *topology,
    const struct acpi_mcfg *mcfg
);
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
enum paging_status paging_verify(void);
struct paging_state paging_get_state(void);
bool paging_is_active(void);
bool paging_self_test(void);
const char *paging_status_string(enum paging_status status);

/*
 * Store one byte through a supervisor pointer at an instruction address a test
 * can name. Written in assembly for the same reason interrupts.S owns the other
 * fault probes: the scenario matches the faulting RIP exactly, and a compiler
 * is free to move, duplicate, or delete an equivalent C store.
 */
void paging_probe_write(volatile uint8_t *target, uint8_t value);
extern const uint8_t paging_probe_write_site[];

#endif
