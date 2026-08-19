/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_ACPI_H
#define SENERI_ACPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/boot.h>

/* ACPI 6.6 section 5.2.12 defines the MADT flag field. */
#define ACPI_MADT_PCAT_COMPAT UINT32_C(1)

/*
 * ACPI 6.6 table 5.10 defines the fixed ACPI description table's flags. Bit 8,
 * TMR_VAL_EXT, is clear when the power management timer counter is 24 bits wide
 * and set when it is 32.
 */
#define ACPI_FADT_TMR_VAL_EXT UINT32_C(0x00000100)

/*
 * ACPI 6.6 section 5.2.9 fixes the power management timer block at four bytes.
 * Any other length means the platform does not implement the timer.
 */
#define ACPI_PM_TIMER_BLOCK_LENGTH UINT8_C(4)

/*
 * ACPI 6.6 section 4.8.3.3: the power management timer counter is 24 bits wide,
 * or 32 when TMR_VAL_EXT is set. The register the counter is read through is a
 * 32-bit port either way, whichever part of it the counter occupies.
 */
#define ACPI_PM_TIMER_BASE_BITS UINT8_C(24)
#define ACPI_PM_TIMER_EXTENDED_BITS UINT8_C(32)
#define ACPI_PM_TIMER_REGISTER_BITS UINT8_C(32)

/*
 * PCI Firmware Specification 3.3 section 4.1.2 fixes the memory-mapped
 * configuration table's layout: a description header, eight reserved bytes,
 * then a whole number of sixteen-byte allocation structures.
 */
#define ACPI_MCFG_FIXED_SIZE 44U
#define ACPI_MCFG_ALLOCATION_SIZE 16U

/*
 * PCI Express Base Specification 6.1 section 7.2.2 lays configuration space out
 * so that the bus number occupies address bits 27:20, the device bits 19:15,
 * the function bits 14:12 and the register the low twelve. One bus is therefore
 * 1 MiB of memory and one function 4 KiB of it, and a base address that is not
 * 1 MiB aligned cannot be indexed that way at all.
 */
#define ACPI_ECAM_BUS_SIZE UINT64_C(0x100000)
#define ACPI_ECAM_FUNCTION_SIZE UINT64_C(0x1000)

/*
 * A Seneri early-boot policy bound on how many configuration windows firmware
 * may declare, not an architectural one. Every machine Seneri is tested on
 * declares one. It keeps the discovered description in fixed storage.
 */
#define ACPI_MAX_ECAM_ALLOCATIONS 8U

enum acpi_root_kind {
    ACPI_ROOT_NONE = 0,
    ACPI_ROOT_RSDT,
    ACPI_ROOT_XSDT
};

enum acpi_status {
    ACPI_STATUS_OK = 0,
    ACPI_STATUS_NULL_ARGUMENT,
    ACPI_STATUS_MISSING_RSDP,
    ACPI_STATUS_BAD_TAG_SIZE,
    ACPI_STATUS_BAD_SIGNATURE,
    ACPI_STATUS_BAD_LEGACY_CHECKSUM,
    ACPI_STATUS_BAD_REVISION,
    ACPI_STATUS_BAD_LENGTH,
    ACPI_STATUS_BAD_EXTENDED_CHECKSUM,
    ACPI_STATUS_NULL_ROOT,
    ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP,
    ACPI_STATUS_BAD_ROOT_SIGNATURE,
    ACPI_STATUS_BAD_ROOT_LENGTH,
    ACPI_STATUS_BAD_ROOT_ENTRY_SIZE,
    ACPI_STATUS_TOO_MANY_ROOT_ENTRIES,
    ACPI_STATUS_BAD_ROOT_CHECKSUM,
    ACPI_STATUS_NULL_TABLE,
    ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP,
    ACPI_STATUS_BAD_TABLE_LENGTH,
    ACPI_STATUS_BAD_TABLE_CHECKSUM,
    ACPI_STATUS_MISSING_MADT,
    ACPI_STATUS_DUPLICATE_MADT,
    ACPI_STATUS_BAD_MADT_LENGTH,
    ACPI_STATUS_BAD_MADT_FLAGS,
    ACPI_STATUS_TRUNCATED_MADT_ENTRY,
    ACPI_STATUS_BAD_MADT_ENTRY_LENGTH,
    ACPI_STATUS_BAD_PROCESSOR_FLAGS,
    ACPI_STATUS_DUPLICATE_PROCESSOR_UID,
    ACPI_STATUS_TOO_MANY_LOCAL_APICS,
    ACPI_STATUS_DUPLICATE_IO_APIC_ID,
    ACPI_STATUS_TOO_MANY_IO_APICS,
    ACPI_STATUS_APIC_OUTSIDE_EARLY_MAP,
    ACPI_STATUS_BAD_OVERRIDE_BUS,
    ACPI_STATUS_BAD_OVERRIDE_SOURCE,
    ACPI_STATUS_BAD_OVERRIDE_FLAGS,
    ACPI_STATUS_DUPLICATE_OVERRIDE_SOURCE,
    ACPI_STATUS_DUPLICATE_APIC_ADDRESS_OVERRIDE,
    ACPI_STATUS_MISSING_BOOT_PROCESSOR,
    ACPI_STATUS_MISSING_IO_APIC,
    ACPI_STATUS_MISSING_FADT,
    ACPI_STATUS_DUPLICATE_FADT,
    ACPI_STATUS_BAD_FADT_LENGTH,
    ACPI_STATUS_BAD_PM_TIMER_LENGTH,
    ACPI_STATUS_MISSING_PM_TIMER,
    ACPI_STATUS_BAD_PM_TIMER_BLOCK,
    ACPI_STATUS_BAD_PM_TIMER_PORT,
    ACPI_STATUS_MISSING_MCFG,
    ACPI_STATUS_DUPLICATE_MCFG,
    ACPI_STATUS_BAD_MCFG_LENGTH,
    ACPI_STATUS_NO_ECAM_ALLOCATIONS,
    ACPI_STATUS_TOO_MANY_ECAM_ALLOCATIONS,
    ACPI_STATUS_BAD_ECAM_BUS_RANGE,
    ACPI_STATUS_BAD_ECAM_BASE,
    ACPI_STATUS_OVERLAPPING_ECAM_RANGE
};

struct acpi_root {
    enum acpi_root_kind kind;
    uint8_t revision;
    char oem_id[7];
    uint64_t physical_address;
};

struct acpi_madt {
    uint64_t physical_address;
    uint32_t length;
    uint32_t local_apic_address;
    uint32_t flags;
    size_t root_entry_count;
    uint8_t revision;
    char oem_id[7];
    char oem_table_id[9];
};

/*
 * The fixed ACPI description table, reduced to the one facility Seneri uses
 * from it. The power management timer is the only clock on this machine whose
 * frequency is stated by a specification rather than measured against another
 * clock, which is what makes it worth reading this table for.
 */
struct acpi_fadt {
    uint64_t physical_address;
    uint32_t length;
    uint32_t flags;
    uint16_t pm_timer_port;
    uint8_t pm_timer_counter_bits;
    uint8_t revision;
    bool pm_timer_extended_address;
    char oem_id[7];
    char oem_table_id[9];
};

/*
 * One firmware-declared memory-mapped configuration window: where the enhanced
 * configuration access mechanism starts, which PCI segment group it serves, and
 * which buses of that group it covers. The window is described here and used by
 * src/kernel/pci.c; this reader never touches the memory it names.
 */
struct acpi_ecam_allocation {
    uint64_t base_address;
    uint16_t segment;
    uint8_t start_bus;
    uint8_t end_bus;
};

/*
 * The memory-mapped configuration table. Unlike the MADT and the FADT this
 * table is optional: a machine with no PCI Express host bridge has no reason to
 * publish one, and ACPI_STATUS_MISSING_MCFG is a description of such a machine
 * rather than a fault in its firmware. The caller decides what absence means.
 */
struct acpi_mcfg {
    uint64_t physical_address;
    uint32_t length;
    size_t allocation_count;
    uint8_t revision;
    char oem_id[7];
    char oem_table_id[9];
    struct acpi_ecam_allocation allocations[ACPI_MAX_ECAM_ALLOCATIONS];
};

/*
 * Seneri early-boot policy bounds on firmware-declared topology. They keep the
 * discovered description in fixed storage until a heap exists. They are not
 * ACPI architectural limits.
 */
#define ACPI_MAX_LOCAL_APICS 64U
#define ACPI_MAX_IO_APICS 8U

/*
 * Overrides need no runtime capacity check: only the ISA bus may be overridden,
 * only its sixteen IRQs are valid sources, and a repeated source is rejected.
 * acpi_madt.c statically asserts that this array covers that proved bound.
 */
#define ACPI_MAX_INTERRUPT_OVERRIDES 16U

struct acpi_local_apic {
    uint32_t processor_uid;
    uint32_t apic_id;
    bool enabled;
    bool online_capable;
};

struct acpi_io_apic {
    uint32_t identifier;
    uint32_t address;
    uint32_t interrupt_base;
};

struct acpi_interrupt_override {
    uint32_t global_system_interrupt;
    uint16_t flags;
    uint8_t bus;
    uint8_t source;
};

struct acpi_topology {
    uint64_t local_apic_address;
    bool local_apic_address_overridden;
    bool legacy_pic_present;
    size_t local_apic_count;
    size_t enabled_processor_count;
    size_t io_apic_count;
    size_t interrupt_override_count;
    size_t nmi_entry_count;
    size_t ignored_entry_count;
    struct acpi_local_apic local_apics[ACPI_MAX_LOCAL_APICS];
    struct acpi_io_apic io_apics[ACPI_MAX_IO_APICS];
    struct acpi_interrupt_override
        interrupt_overrides[ACPI_MAX_INTERRUPT_OVERRIDES];
};

enum acpi_status acpi_root_discover(
    const struct boot_context *context,
    struct acpi_root *root
);
enum acpi_status acpi_madt_discover(
    const struct acpi_root *root,
    struct acpi_madt *madt
);
enum acpi_status acpi_fadt_discover(
    const struct acpi_root *root,
    struct acpi_fadt *fadt
);
enum acpi_status acpi_mcfg_discover(
    const struct acpi_root *root,
    struct acpi_mcfg *mcfg
);
enum acpi_status acpi_topology_discover(
    const struct acpi_madt *madt,
    struct acpi_topology *topology
);

/*
 * How many bytes of memory an allocation's bus range occupies. Validated at
 * discovery to be non-zero and to fit inside a 64-bit address space.
 */
uint64_t acpi_ecam_allocation_size(const struct acpi_ecam_allocation *allocation);
bool acpi_self_test(void);
bool acpi_tables_self_test(void);
bool acpi_topology_self_test(void);
const char *acpi_root_kind_string(enum acpi_root_kind kind);
const char *acpi_status_string(enum acpi_status status);

#endif
