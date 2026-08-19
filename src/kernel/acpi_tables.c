/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/acpi_util.h>

/* Seneri early-boot policy bounds firmware-controlled work. */
#define ACPI_MAX_ROOT_ENTRIES 256U

/*
 * ACPI 6.6 section 5.2.9 table 5.9 fixes these byte offsets inside the fixed
 * ACPI description table. They are measured from the start of the table, not
 * from the end of its description header.
 */
#define FADT_OFFSET_PM_TMR_BLK 76U
#define FADT_OFFSET_PM_TMR_LEN 91U
#define FADT_OFFSET_FLAGS 112U
#define FADT_OFFSET_X_PM_TMR_BLK 208U

/*
 * ACPI 1.0 ends the table at 116 bytes, immediately past FLAGS. Every field
 * Seneri requires is inside that span, so a table of exactly that size is still
 * usable; the extended timer address is read only when the table's own declared
 * length reaches the end of the structure, because the bytes past a short
 * table's length belong to something else.
 */
#define FADT_MINIMUM_SIZE 116U

/* ACPI 6.6 section 5.2.3.2 lays out the generic address structure. */
#define ACPI_GAS_SIZE 12U
#define ACPI_GAS_OFFSET_SPACE_ID 0U
#define ACPI_GAS_OFFSET_BIT_WIDTH 1U
#define ACPI_GAS_OFFSET_BIT_OFFSET 2U
#define ACPI_GAS_OFFSET_ADDRESS 4U
#define ACPI_GAS_SPACE_SYSTEM_IO 1U

#define FADT_X_PM_TMR_BLK_END (FADT_OFFSET_X_PM_TMR_BLK + ACPI_GAS_SIZE)

/*
 * The fields read out of an ACPI 1.0 table must lie inside the span that
 * table is guaranteed to have. Proving it here removes the need for a bounds
 * branch per field that no test could reach.
 */
_Static_assert(FADT_OFFSET_PM_TMR_BLK + sizeof(uint32_t) <= FADT_MINIMUM_SIZE,
               "PM_TMR_BLK no longer fits inside an ACPI 1.0 FADT");
_Static_assert(FADT_OFFSET_PM_TMR_LEN < FADT_MINIMUM_SIZE,
               "PM_TMR_LEN no longer fits inside an ACPI 1.0 FADT");
_Static_assert(FADT_OFFSET_FLAGS + sizeof(uint32_t) <= FADT_MINIMUM_SIZE,
               "FLAGS no longer fits inside an ACPI 1.0 FADT");

/*
 * PCI Firmware Specification 3.3 table 4-2 places the allocation structures
 * after the description header and eight reserved bytes, and gives each one a
 * 64-bit base address, a 16-bit segment group, a start and an end bus number,
 * and four reserved bytes.
 */
#define MCFG_OFFSET_BASE_ADDRESS 0U
#define MCFG_OFFSET_SEGMENT 8U
#define MCFG_OFFSET_START_BUS 10U
#define MCFG_OFFSET_END_BUS 11U

_Static_assert(
    MCFG_OFFSET_END_BUS < ACPI_MCFG_ALLOCATION_SIZE,
    "an MCFG allocation field no longer fits inside an allocation structure"
);
_Static_assert(
    ACPI_MCFG_FIXED_SIZE > ACPI_SDT_HEADER_SIZE,
    "the MCFG reserved field no longer follows the description header"
);

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt_table {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

struct acpi_test_table {
    struct acpi_sdt_header header;
    uint32_t payload;
} __attribute__((packed));

struct acpi_test_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));

/*
 * ACPI 6.6 section 5.2.9 sizes a revision 6 fixed description table at 276
 * bytes. The fixture declares the whole span so both the fixed and the extended
 * timer address are reachable from it.
 */
#define ACPI_TEST_FADT_SIZE 276U

struct acpi_test_fadt {
    struct acpi_sdt_header header;
    uint8_t body[ACPI_TEST_FADT_SIZE - ACPI_SDT_HEADER_SIZE];
} __attribute__((packed));

/*
 * Two allocation structures, which is one more than any machine Seneri is
 * tested on declares. The second exists so the overlap rejection has something
 * to reject against.
 */
#define ACPI_TEST_MCFG_ALLOCATIONS 2U
#define ACPI_TEST_MCFG_SIZE \
    (ACPI_MCFG_FIXED_SIZE + \
        ACPI_TEST_MCFG_ALLOCATIONS * ACPI_MCFG_ALLOCATION_SIZE)

struct acpi_test_mcfg {
    struct acpi_sdt_header header;
    uint8_t body[ACPI_TEST_MCFG_SIZE - ACPI_SDT_HEADER_SIZE];
} __attribute__((packed));

struct acpi_test_xsdt {
    struct acpi_sdt_header header;
    uint64_t entries[4];
} __attribute__((packed));

struct acpi_test_rsdt {
    struct acpi_sdt_header header;
    uint32_t entries[4];
} __attribute__((packed));

struct acpi_test_fixture {
    struct acpi_test_table other;
    struct acpi_test_madt madt;
    struct acpi_test_fadt fadt;
    struct acpi_test_mcfg mcfg;
    struct acpi_test_xsdt xsdt;
    struct acpi_test_rsdt rsdt;
} __attribute__((aligned(8)));

_Static_assert(sizeof(struct acpi_sdt_header) == ACPI_SDT_HEADER_SIZE,
               "ACPI description header layout changed");
_Static_assert(offsetof(struct acpi_sdt_header, length) == 4U,
               "ACPI description length offset changed");
_Static_assert(offsetof(struct acpi_sdt_header, checksum) == 9U,
               "ACPI description checksum offset changed");
_Static_assert(sizeof(struct acpi_madt_table) == ACPI_MADT_FIXED_SIZE,
               "ACPI MADT fixed layout changed");
_Static_assert(offsetof(struct acpi_madt_table, local_apic_address) == 36U,
               "ACPI MADT local APIC address offset changed");
_Static_assert(offsetof(struct acpi_madt_table, flags) == 40U,
               "ACPI MADT flags offset changed");
_Static_assert(sizeof(struct acpi_test_fadt) == ACPI_TEST_FADT_SIZE,
               "ACPI FADT fixture no longer spans the extended timer address");
_Static_assert(sizeof(struct acpi_test_mcfg) == ACPI_TEST_MCFG_SIZE,
               "ACPI MCFG fixture no longer spans its allocation structures");

static const char rsdt_signature[4] = {'R', 'S', 'D', 'T'};
static const char xsdt_signature[4] = {'X', 'S', 'D', 'T'};
static const char madt_signature[4] = {'A', 'P', 'I', 'C'};
/* ACPI 6.6 section 5.2.9 signs the fixed description table FACP, not FADT. */
static const char fadt_signature[4] = {'F', 'A', 'C', 'P'};
/* PCI Firmware Specification 3.3 section 4.1.2 signs this table MCFG. */
static const char mcfg_signature[4] = {'M', 'C', 'F', 'G'};
static const char test_signature[4] = {'T', 'E', 'S', 'T'};
static const char test_oem_id[6] = {'Z', 'E', 'N', 'I', 'T', 'H'};
static const char test_oem_table_id[8] = {'Z', 'T', 'T', 'A', 'B', 'L', 'E', 'S'};

static void madt_reset(struct acpi_madt *madt)
{
    madt->physical_address = 0U;
    madt->length = 0U;
    madt->local_apic_address = 0U;
    madt->flags = 0U;
    madt->root_entry_count = 0U;
    madt->revision = 0U;

    for (size_t index = 0; index < sizeof(madt->oem_id); ++index) {
        madt->oem_id[index] = '\0';
    }

    for (size_t index = 0; index < sizeof(madt->oem_table_id); ++index) {
        madt->oem_table_id[index] = '\0';
    }
}

static void fadt_reset(struct acpi_fadt *fadt)
{
    fadt->physical_address = 0U;
    fadt->length = 0U;
    fadt->flags = 0U;
    fadt->pm_timer_port = 0U;
    fadt->pm_timer_counter_bits = 0U;
    fadt->revision = 0U;
    fadt->pm_timer_extended_address = false;

    for (size_t index = 0; index < sizeof(fadt->oem_id); ++index) {
        fadt->oem_id[index] = '\0';
    }

    for (size_t index = 0; index < sizeof(fadt->oem_table_id); ++index) {
        fadt->oem_table_id[index] = '\0';
    }
}

static void mcfg_reset(struct acpi_mcfg *mcfg)
{
    mcfg->physical_address = 0U;
    mcfg->length = 0U;
    mcfg->allocation_count = 0U;
    mcfg->revision = 0U;

    for (size_t index = 0; index < sizeof(mcfg->oem_id); ++index) {
        mcfg->oem_id[index] = '\0';
    }

    for (size_t index = 0; index < sizeof(mcfg->oem_table_id); ++index) {
        mcfg->oem_table_id[index] = '\0';
    }

    for (size_t index = 0; index < ACPI_MAX_ECAM_ALLOCATIONS; ++index) {
        mcfg->allocations[index].base_address = 0U;
        mcfg->allocations[index].segment = 0U;
        mcfg->allocations[index].start_bus = 0U;
        mcfg->allocations[index].end_bus = 0U;
    }
}

static enum acpi_status validate_root_table(
    const struct acpi_root *root,
    const struct acpi_sdt_header **header_out,
    size_t *entry_size_out,
    size_t *entry_count_out
)
{
    const struct acpi_sdt_header *header;
    const char *expected_signature;
    size_t entry_size;
    size_t payload_size;
    size_t entry_count;

    if (root->physical_address == 0U) {
        return ACPI_STATUS_NULL_ROOT;
    }

    if (!acpi_span_is_early_mapped(
            root->physical_address,
            ACPI_SDT_HEADER_SIZE
        )) {
        return ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP;
    }

    header = (const struct acpi_sdt_header *)(uintptr_t)root->physical_address;

    if (root->kind == ACPI_ROOT_XSDT) {
        expected_signature = xsdt_signature;
        entry_size = sizeof(uint64_t);
    } else if (root->kind == ACPI_ROOT_RSDT) {
        expected_signature = rsdt_signature;
        entry_size = sizeof(uint32_t);
    } else {
        return ACPI_STATUS_BAD_ROOT_SIGNATURE;
    }

    if (!acpi_bytes_equal(
            header->signature,
            expected_signature,
            sizeof(header->signature)
        )) {
        return ACPI_STATUS_BAD_ROOT_SIGNATURE;
    }

    if (header->length < sizeof(*header) ||
        header->length > ACPI_MAX_TABLE_SIZE) {
        return ACPI_STATUS_BAD_ROOT_LENGTH;
    }

    payload_size = header->length - sizeof(*header);

    if (payload_size % entry_size != 0U) {
        return ACPI_STATUS_BAD_ROOT_ENTRY_SIZE;
    }

    entry_count = payload_size / entry_size;

    if (entry_count > ACPI_MAX_ROOT_ENTRIES) {
        return ACPI_STATUS_TOO_MANY_ROOT_ENTRIES;
    }

    if (!acpi_span_is_early_mapped(root->physical_address, header->length)) {
        return ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP;
    }

    if (acpi_byte_sum(header, header->length) != 0U) {
        return ACPI_STATUS_BAD_ROOT_CHECKSUM;
    }

    *header_out = header;
    *entry_size_out = entry_size;
    *entry_count_out = entry_count;
    return ACPI_STATUS_OK;
}

static enum acpi_status validate_referenced_table(
    uint64_t physical_address,
    const struct acpi_sdt_header **header_out
)
{
    const struct acpi_sdt_header *header;

    if (physical_address == 0U) {
        return ACPI_STATUS_NULL_TABLE;
    }

    if (!acpi_span_is_early_mapped(physical_address, ACPI_SDT_HEADER_SIZE)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    header = (const struct acpi_sdt_header *)(uintptr_t)physical_address;

    if (header->length < sizeof(*header) ||
        header->length > ACPI_MAX_TABLE_SIZE) {
        return ACPI_STATUS_BAD_TABLE_LENGTH;
    }

    if (!acpi_span_is_early_mapped(physical_address, header->length)) {
        return ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP;
    }

    if (acpi_byte_sum(header, header->length) != 0U) {
        return ACPI_STATUS_BAD_TABLE_CHECKSUM;
    }

    *header_out = header;
    return ACPI_STATUS_OK;
}

/*
 * Walk the root table once and return the single entry carrying a signature.
 * Every table Seneri reads comes through here, so a referenced table is bounds
 * checked and its checksum verified before any reader sees it, and a firmware
 * that declares the same table twice is refused rather than resolved by
 * position. The caller names the two refusals so each table reports its own.
 */
static enum acpi_status find_unique_table(
    const struct acpi_root *root,
    const char signature[4],
    enum acpi_status missing_status,
    enum acpi_status duplicate_status,
    const struct acpi_sdt_header **header_out,
    uint64_t *physical_address_out,
    size_t *entry_count_out
)
{
    const struct acpi_sdt_header *root_header;
    const uint8_t *entries;
    size_t entry_size;
    size_t entry_count;
    bool found = false;
    enum acpi_status status;

    status = validate_root_table(
        root,
        &root_header,
        &entry_size,
        &entry_count
    );

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    entries = (const uint8_t *)(const void *)root_header + sizeof(*root_header);

    for (size_t index = 0; index < entry_count; ++index) {
        const uint8_t *entry = entries + index * entry_size;
        const struct acpi_sdt_header *header;
        uint64_t physical_address = entry_size == sizeof(uint64_t)
            ? acpi_read_u64(entry)
            : acpi_read_u32(entry);

        status = validate_referenced_table(physical_address, &header);

        if (status != ACPI_STATUS_OK) {
            return status;
        }

        if (!acpi_bytes_equal(
                header->signature,
                signature,
                sizeof(header->signature)
            )) {
            continue;
        }

        if (found) {
            return duplicate_status;
        }

        *header_out = header;
        *physical_address_out = physical_address;
        found = true;
    }

    if (!found) {
        return missing_status;
    }

    *entry_count_out = entry_count;
    return ACPI_STATUS_OK;
}

enum acpi_status acpi_madt_discover(
    const struct acpi_root *root,
    struct acpi_madt *madt
)
{
    const struct acpi_sdt_header *header;
    const struct acpi_madt_table *table;
    uint64_t physical_address = 0U;
    size_t entry_count = 0U;
    enum acpi_status status;

    if (root == NULL || madt == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    madt_reset(madt);
    status = find_unique_table(
        root,
        madt_signature,
        ACPI_STATUS_MISSING_MADT,
        ACPI_STATUS_DUPLICATE_MADT,
        &header,
        &physical_address,
        &entry_count
    );

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    if (header->length < sizeof(struct acpi_madt_table)) {
        return ACPI_STATUS_BAD_MADT_LENGTH;
    }

    table = (const struct acpi_madt_table *)(const void *)header;

    if ((table->flags & ~ACPI_MADT_PCAT_COMPAT) != 0U) {
        return ACPI_STATUS_BAD_MADT_FLAGS;
    }

    madt->physical_address = physical_address;
    madt->length = header->length;
    madt->local_apic_address = table->local_apic_address;
    madt->flags = table->flags;
    madt->root_entry_count = entry_count;
    madt->revision = header->revision;
    acpi_copy_string(madt->oem_id, header->oem_id, sizeof(header->oem_id));
    acpi_copy_string(
        madt->oem_table_id,
        header->oem_table_id,
        sizeof(header->oem_table_id)
    );
    return ACPI_STATUS_OK;
}

/*
 * Decode the power management timer's address out of a validated FADT.
 * Firmware chooses every byte read here, so each field is checked before it is
 * believed, and nothing is written into the caller's structure until the last
 * check has passed.
 */
static enum acpi_status decode_pm_timer(
    const uint8_t *table,
    uint32_t length,
    struct acpi_fadt *fadt
)
{
    uint32_t flags;
    uint64_t port = 0U;
    bool extended = false;

    if (length < FADT_MINIMUM_SIZE) {
        return ACPI_STATUS_BAD_FADT_LENGTH;
    }

    /*
     * ACPI 6.6 section 5.2.9 fixes PM_TMR_LEN at four. Firmware reports zero
     * on a platform with no power management timer, and any other value
     * describes a register this reader does not know how to address.
     */
    if (table[FADT_OFFSET_PM_TMR_LEN] != ACPI_PM_TIMER_BLOCK_LENGTH) {
        return ACPI_STATUS_BAD_PM_TIMER_LENGTH;
    }

    flags = acpi_read_u32(table + FADT_OFFSET_FLAGS);

    /*
     * ACPI 6.6 section 5.2.9 gives X_PM_TMR_BLK precedence: when it holds an
     * address the operating system can use, PM_TMR_BLK must be ignored. It is
     * read only when the table's declared length reaches the end of the generic
     * address structure, because an ACPI 1.0 table stops at 116 bytes and the
     * memory past its length is not the table's to describe.
     *
     * A block in any other address space is one this kernel cannot reach with a
     * port read, so it selects nothing and the fixed field stands. A block that
     * does claim System I/O is taken, and then has to be well formed: a
     * contradiction between the two fields is refused rather than resolved by
     * preferring whichever one happens to look plausible.
     */
    if (length >= FADT_X_PM_TMR_BLK_END) {
        const uint8_t *block = table + FADT_OFFSET_X_PM_TMR_BLK;
        const uint64_t address = acpi_read_u64(block + ACPI_GAS_OFFSET_ADDRESS);

        if (address != 0U &&
            block[ACPI_GAS_OFFSET_SPACE_ID] == ACPI_GAS_SPACE_SYSTEM_IO) {
            if (block[ACPI_GAS_OFFSET_BIT_WIDTH] !=
                    ACPI_PM_TIMER_REGISTER_BITS ||
                block[ACPI_GAS_OFFSET_BIT_OFFSET] != 0U) {
                return ACPI_STATUS_BAD_PM_TIMER_BLOCK;
            }

            port = address;
            extended = true;
        }
    }

    if (!extended) {
        port = acpi_read_u32(table + FADT_OFFSET_PM_TMR_BLK);

        if (port == 0U) {
            return ACPI_STATUS_MISSING_PM_TIMER;
        }
    }

    /*
     * The block is four bytes wide and an x86 I/O port address stops at
     * 0xFFFF, so its last byte must still be addressable.
     */
    if (port > (uint64_t)UINT16_MAX - (ACPI_PM_TIMER_BLOCK_LENGTH - 1U)) {
        return ACPI_STATUS_BAD_PM_TIMER_PORT;
    }

    fadt->flags = flags;
    fadt->pm_timer_port = (uint16_t)port;
    fadt->pm_timer_extended_address = extended;
    fadt->pm_timer_counter_bits = (flags & ACPI_FADT_TMR_VAL_EXT) != 0U
        ? ACPI_PM_TIMER_EXTENDED_BITS
        : ACPI_PM_TIMER_BASE_BITS;
    return ACPI_STATUS_OK;
}

enum acpi_status acpi_fadt_discover(
    const struct acpi_root *root,
    struct acpi_fadt *fadt
)
{
    const struct acpi_sdt_header *header;
    uint64_t physical_address = 0U;
    size_t entry_count = 0U;
    enum acpi_status status;

    if (root == NULL || fadt == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    fadt_reset(fadt);
    status = find_unique_table(
        root,
        fadt_signature,
        ACPI_STATUS_MISSING_FADT,
        ACPI_STATUS_DUPLICATE_FADT,
        &header,
        &physical_address,
        &entry_count
    );

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    status = decode_pm_timer(
        (const uint8_t *)(const void *)header,
        header->length,
        fadt
    );

    if (status != ACPI_STATUS_OK) {
        fadt_reset(fadt);
        return status;
    }

    fadt->physical_address = physical_address;
    fadt->length = header->length;
    fadt->revision = header->revision;
    acpi_copy_string(fadt->oem_id, header->oem_id, sizeof(header->oem_id));
    acpi_copy_string(
        fadt->oem_table_id,
        header->oem_table_id,
        sizeof(header->oem_table_id)
    );
    return ACPI_STATUS_OK;
}

uint64_t acpi_ecam_allocation_size(
    const struct acpi_ecam_allocation *allocation
)
{
    if (allocation == NULL) {
        return 0U;
    }

    /*
     * end_bus is inclusive, and both are eight bits, so the count is at most
     * 256 and the product at most 256 MiB. Computed in 64 bits because the
     * eight-bit difference plus one would overflow a uint8_t at the full range.
     */
    return ((uint64_t)allocation->end_bus - allocation->start_bus + 1U) *
        ACPI_ECAM_BUS_SIZE;
}

/*
 * Two windows conflict if they describe the same buses of the same segment
 * group, or if they occupy the same memory whatever they claim to describe.
 * Either way one of them is wrong, and picking whichever came first would
 * silently address the wrong device.
 */
static bool ecam_allocations_conflict(
    const struct acpi_ecam_allocation *left,
    const struct acpi_ecam_allocation *right
)
{
    const uint64_t left_end = left->base_address +
        acpi_ecam_allocation_size(left);
    const uint64_t right_end = right->base_address +
        acpi_ecam_allocation_size(right);

    if (left->segment == right->segment &&
        left->start_bus <= right->end_bus &&
        right->start_bus <= left->end_bus) {
        return true;
    }

    return left->base_address < right_end && right->base_address < left_end;
}

static enum acpi_status decode_ecam_allocations(
    const uint8_t *table,
    uint32_t length,
    struct acpi_mcfg *mcfg
)
{
    const uint8_t *records = table + ACPI_MCFG_FIXED_SIZE;
    uint32_t payload;
    size_t count;

    if (length < ACPI_MCFG_FIXED_SIZE) {
        return ACPI_STATUS_BAD_MCFG_LENGTH;
    }

    payload = length - ACPI_MCFG_FIXED_SIZE;

    /*
     * A table whose payload is not a whole number of allocation structures
     * describes a layout this reader does not know, so it is refused rather
     * than truncated to the part that happens to fit.
     */
    if (payload % ACPI_MCFG_ALLOCATION_SIZE != 0U) {
        return ACPI_STATUS_BAD_MCFG_LENGTH;
    }

    count = payload / ACPI_MCFG_ALLOCATION_SIZE;

    if (count == 0U) {
        return ACPI_STATUS_NO_ECAM_ALLOCATIONS;
    }

    if (count > ACPI_MAX_ECAM_ALLOCATIONS) {
        return ACPI_STATUS_TOO_MANY_ECAM_ALLOCATIONS;
    }

    for (size_t index = 0; index < count; ++index) {
        const uint8_t *record = records + index * ACPI_MCFG_ALLOCATION_SIZE;
        struct acpi_ecam_allocation allocation;
        uint64_t size;

        allocation.base_address =
            acpi_read_u64(record + MCFG_OFFSET_BASE_ADDRESS);
        allocation.segment = acpi_read_u16(record + MCFG_OFFSET_SEGMENT);
        allocation.start_bus = record[MCFG_OFFSET_START_BUS];
        allocation.end_bus = record[MCFG_OFFSET_END_BUS];

        if (allocation.end_bus < allocation.start_bus) {
            return ACPI_STATUS_BAD_ECAM_BUS_RANGE;
        }

        /*
         * Zero is not an address a host bridge decodes, and a base that is not
         * 1 MiB aligned cannot carry the bus number in address bits 27:20, so
         * the addressing this window exists for would not work at all.
         */
        if (allocation.base_address == 0U ||
            allocation.base_address % ACPI_ECAM_BUS_SIZE != 0U) {
            return ACPI_STATUS_BAD_ECAM_BASE;
        }

        size = acpi_ecam_allocation_size(&allocation);

        if (allocation.base_address > UINT64_MAX - size) {
            return ACPI_STATUS_BAD_ECAM_BASE;
        }

        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (ecam_allocations_conflict(
                    &mcfg->allocations[earlier],
                    &allocation
                )) {
                return ACPI_STATUS_OVERLAPPING_ECAM_RANGE;
            }
        }

        mcfg->allocations[index] = allocation;
    }

    mcfg->allocation_count = count;
    return ACPI_STATUS_OK;
}

/*
 * Discover the memory-mapped configuration window firmware advertises.
 *
 * This is the first optional table Seneri reads. A machine with no PCI Express
 * host bridge publishes no MCFG, and that is a fact about the machine rather
 * than a fault: ACPI_STATUS_MISSING_MCFG is returned and the caller decides
 * whether it can proceed without one. Everything else here is refused for the
 * same reason every other firmware table is - a window described wrongly would
 * be read as a device that is not there.
 */
enum acpi_status acpi_mcfg_discover(
    const struct acpi_root *root,
    struct acpi_mcfg *mcfg
)
{
    const struct acpi_sdt_header *header;
    uint64_t physical_address = 0U;
    size_t entry_count = 0U;
    enum acpi_status status;

    if (root == NULL || mcfg == NULL) {
        return ACPI_STATUS_NULL_ARGUMENT;
    }

    mcfg_reset(mcfg);
    status = find_unique_table(
        root,
        mcfg_signature,
        ACPI_STATUS_MISSING_MCFG,
        ACPI_STATUS_DUPLICATE_MCFG,
        &header,
        &physical_address,
        &entry_count
    );

    if (status != ACPI_STATUS_OK) {
        return status;
    }

    status = decode_ecam_allocations(
        (const uint8_t *)(const void *)header,
        header->length,
        mcfg
    );

    if (status != ACPI_STATUS_OK) {
        mcfg_reset(mcfg);
        return status;
    }

    mcfg->physical_address = physical_address;
    mcfg->length = header->length;
    mcfg->revision = header->revision;
    acpi_copy_string(mcfg->oem_id, header->oem_id, sizeof(header->oem_id));
    acpi_copy_string(
        mcfg->oem_table_id,
        header->oem_table_id,
        sizeof(header->oem_table_id)
    );
    return ACPI_STATUS_OK;
}

static void initialize_header(
    struct acpi_sdt_header *header,
    const char signature[4],
    uint32_t length
)
{
    acpi_bytes_zero(header, sizeof(*header));

    for (size_t index = 0; index < sizeof(header->signature); ++index) {
        header->signature[index] = signature[index];
    }

    for (size_t index = 0; index < sizeof(header->oem_id); ++index) {
        header->oem_id[index] = test_oem_id[index];
    }

    for (size_t index = 0; index < sizeof(header->oem_table_id); ++index) {
        header->oem_table_id[index] = test_oem_table_id[index];
    }

    header->length = length;
    header->revision = 1U;
    header->oem_revision = 1U;
    header->creator_id = UINT32_C(0x54494E5A);
    header->creator_revision = 1U;
}

static void set_checksum(void *table, size_t size)
{
    struct acpi_sdt_header *header = (struct acpi_sdt_header *)table;

    header->checksum = 0U;
    header->checksum = (uint8_t)(0U - acpi_byte_sum(table, size));
}

/* The fixture's synthetic ports, chosen to look nothing like a default. */
#define TEST_PM_TIMER_FIXED_PORT UINT32_C(0x0408)
#define TEST_PM_TIMER_EXTENDED_PORT UINT64_C(0x1008)

/*
 * The fixture's synthetic configuration windows. Both are 1 MiB aligned and
 * cover four buses each, and the second sits far enough above the first that
 * only a deliberate edit can make them overlap.
 */
#define TEST_ECAM_BASE UINT64_C(0xE0000000)
#define TEST_ECAM_SECOND_BASE UINT64_C(0xE8000000)
#define TEST_ECAM_SEGMENT UINT16_C(0)
#define TEST_ECAM_SECOND_SEGMENT UINT16_C(1)
#define TEST_ECAM_END_BUS UINT8_C(3)

/*
 * Firmware tables are little-endian on the wire wherever they sit, and the
 * fixture writes the same fields acpi_read_* reads back, so it builds them a
 * byte at a time rather than trusting the host layout of a wider store.
 */
static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_u64(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

static void write_u16(uint8_t *bytes, uint16_t value)
{
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
}

/* Address a FADT field by its offset from the start of the table. */
static uint8_t *fadt_field(struct acpi_test_fadt *fadt, size_t offset)
{
    return (uint8_t *)(void *)fadt + offset;
}

/* Address a field of one MCFG allocation structure by allocation and offset. */
static uint8_t *mcfg_field(
    struct acpi_test_mcfg *mcfg,
    size_t allocation,
    size_t offset
)
{
    return (uint8_t *)(void *)mcfg + ACPI_MCFG_FIXED_SIZE +
        allocation * ACPI_MCFG_ALLOCATION_SIZE + offset;
}

static void prepare_test_fixture(struct acpi_test_fixture *fixture)
{
    uint8_t *block;

    acpi_bytes_zero(fixture, sizeof(*fixture));

    initialize_header(
        &fixture->other.header,
        test_signature,
        sizeof(fixture->other)
    );
    fixture->other.payload = UINT32_C(0x12345678);
    set_checksum(&fixture->other, sizeof(fixture->other));

    initialize_header(
        &fixture->madt.header,
        madt_signature,
        sizeof(fixture->madt)
    );
    fixture->madt.header.revision = 7U;
    fixture->madt.local_apic_address = UINT32_C(0xFEE00000);
    fixture->madt.flags = ACPI_MADT_PCAT_COMPAT;
    set_checksum(&fixture->madt, sizeof(fixture->madt));

    initialize_header(
        &fixture->fadt.header,
        fadt_signature,
        sizeof(fixture->fadt)
    );
    fixture->fadt.header.revision = 6U;
    write_u32(
        fadt_field(&fixture->fadt, FADT_OFFSET_PM_TMR_BLK),
        TEST_PM_TIMER_FIXED_PORT
    );
    *fadt_field(&fixture->fadt, FADT_OFFSET_PM_TMR_LEN) =
        ACPI_PM_TIMER_BLOCK_LENGTH;
    write_u32(fadt_field(&fixture->fadt, FADT_OFFSET_FLAGS), 0U);
    block = fadt_field(&fixture->fadt, FADT_OFFSET_X_PM_TMR_BLK);
    block[ACPI_GAS_OFFSET_SPACE_ID] = ACPI_GAS_SPACE_SYSTEM_IO;
    block[ACPI_GAS_OFFSET_BIT_WIDTH] = ACPI_PM_TIMER_REGISTER_BITS;
    block[ACPI_GAS_OFFSET_BIT_OFFSET] = 0U;
    write_u64(block + ACPI_GAS_OFFSET_ADDRESS, TEST_PM_TIMER_EXTENDED_PORT);
    set_checksum(&fixture->fadt, sizeof(fixture->fadt));

    initialize_header(
        &fixture->mcfg.header,
        mcfg_signature,
        sizeof(fixture->mcfg)
    );
    fixture->mcfg.header.revision = 1U;
    write_u64(
        mcfg_field(&fixture->mcfg, 0U, MCFG_OFFSET_BASE_ADDRESS),
        TEST_ECAM_BASE
    );
    write_u16(
        mcfg_field(&fixture->mcfg, 0U, MCFG_OFFSET_SEGMENT),
        TEST_ECAM_SEGMENT
    );
    *mcfg_field(&fixture->mcfg, 0U, MCFG_OFFSET_START_BUS) = 0U;
    *mcfg_field(&fixture->mcfg, 0U, MCFG_OFFSET_END_BUS) = TEST_ECAM_END_BUS;
    write_u64(
        mcfg_field(&fixture->mcfg, 1U, MCFG_OFFSET_BASE_ADDRESS),
        TEST_ECAM_SECOND_BASE
    );
    write_u16(
        mcfg_field(&fixture->mcfg, 1U, MCFG_OFFSET_SEGMENT),
        TEST_ECAM_SECOND_SEGMENT
    );
    *mcfg_field(&fixture->mcfg, 1U, MCFG_OFFSET_START_BUS) = 0U;
    *mcfg_field(&fixture->mcfg, 1U, MCFG_OFFSET_END_BUS) = TEST_ECAM_END_BUS;
    set_checksum(&fixture->mcfg, sizeof(fixture->mcfg));

    initialize_header(
        &fixture->xsdt.header,
        xsdt_signature,
        sizeof(fixture->xsdt)
    );
    fixture->xsdt.entries[0] =
        (uint64_t)(uintptr_t)(void *)&fixture->other;
    fixture->xsdt.entries[1] =
        (uint64_t)(uintptr_t)(void *)&fixture->madt;
    fixture->xsdt.entries[2] =
        (uint64_t)(uintptr_t)(void *)&fixture->fadt;
    fixture->xsdt.entries[3] =
        (uint64_t)(uintptr_t)(void *)&fixture->mcfg;
    set_checksum(&fixture->xsdt, sizeof(fixture->xsdt));

    initialize_header(
        &fixture->rsdt.header,
        rsdt_signature,
        sizeof(fixture->rsdt)
    );
    fixture->rsdt.entries[0] =
        (uint32_t)(uintptr_t)(void *)&fixture->other;
    fixture->rsdt.entries[1] =
        (uint32_t)(uintptr_t)(void *)&fixture->madt;
    fixture->rsdt.entries[2] =
        (uint32_t)(uintptr_t)(void *)&fixture->fadt;
    fixture->rsdt.entries[3] =
        (uint32_t)(uintptr_t)(void *)&fixture->mcfg;
    set_checksum(&fixture->rsdt, sizeof(fixture->rsdt));
}

static struct acpi_root test_root(
    const struct acpi_test_fixture *fixture,
    enum acpi_root_kind kind
)
{
    struct acpi_root root = {
        .kind = kind,
        .revision = 2U,
        .oem_id = {'Z', 'E', 'N', 'I', 'T', 'H', '\0'},
        .physical_address = kind == ACPI_ROOT_XSDT
            ? (uint64_t)(uintptr_t)(const void *)&fixture->xsdt
            : (uint64_t)(uintptr_t)(const void *)&fixture->rsdt
    };

    return root;
}

static bool valid_fixture_is_accepted(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_madt madt;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_OK ||
        madt.physical_address !=
            (uint64_t)(uintptr_t)(void *)&fixture.madt ||
        madt.length != sizeof(fixture.madt) ||
        madt.local_apic_address != UINT32_C(0xFEE00000) ||
        madt.flags != ACPI_MADT_PCAT_COMPAT ||
        madt.root_entry_count != 4U ||
        madt.revision != 7U ||
        !acpi_bytes_equal(madt.oem_id, test_oem_id, sizeof(test_oem_id)) ||
        !acpi_bytes_equal(
            madt.oem_table_id,
            test_oem_table_id,
            sizeof(test_oem_table_id)
        )) {
        return false;
    }

    root = test_root(&fixture, ACPI_ROOT_RSDT);
    return acpi_madt_discover(&root, &madt) == ACPI_STATUS_OK &&
        madt.physical_address == (uint64_t)(uintptr_t)(void *)&fixture.madt;
}

/*
 * The FADT is read for exactly one thing: where the power management timer
 * lives and how wide its counter is. Getting either wrong would not fail
 * loudly, it would produce a reference clock that reads the wrong port or folds
 * wraps at the wrong width, so both decisions are proved here without firmware.
 */
static bool valid_fadt_is_accepted(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_fadt fadt;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    /* A well-formed extended address in System I/O space takes precedence. */
    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_OK ||
        fadt.physical_address != (uint64_t)(uintptr_t)(void *)&fixture.fadt ||
        fadt.length != sizeof(fixture.fadt) ||
        fadt.revision != 6U ||
        !fadt.pm_timer_extended_address ||
        fadt.pm_timer_port != (uint16_t)TEST_PM_TIMER_EXTENDED_PORT ||
        fadt.pm_timer_counter_bits != ACPI_PM_TIMER_BASE_BITS ||
        !acpi_bytes_equal(fadt.oem_id, test_oem_id, sizeof(test_oem_id)) ||
        !acpi_bytes_equal(
            fadt.oem_table_id,
            test_oem_table_id,
            sizeof(test_oem_table_id)
        )) {
        return false;
    }

    /* TMR_VAL_EXT widens the counter to 32 bits and changes nothing else. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u32(
        fadt_field(&fixture.fadt, FADT_OFFSET_FLAGS),
        ACPI_FADT_TMR_VAL_EXT
    );
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_OK ||
        fadt.flags != ACPI_FADT_TMR_VAL_EXT ||
        fadt.pm_timer_counter_bits != ACPI_PM_TIMER_EXTENDED_BITS ||
        fadt.pm_timer_port != (uint16_t)TEST_PM_TIMER_EXTENDED_PORT) {
        return false;
    }

    /* An absent extended address leaves the fixed field in charge. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK) +
            ACPI_GAS_OFFSET_ADDRESS,
        0U
    );
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_OK ||
        fadt.pm_timer_extended_address ||
        fadt.pm_timer_port != (uint16_t)TEST_PM_TIMER_FIXED_PORT) {
        return false;
    }

    /* So does one in an address space no port read can reach. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK)
        [ACPI_GAS_OFFSET_SPACE_ID] = 0U;
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_OK ||
        fadt.pm_timer_extended_address ||
        fadt.pm_timer_port != (uint16_t)TEST_PM_TIMER_FIXED_PORT) {
        return false;
    }

    /*
     * An ACPI 1.0 table stops before the extended address, so it is never read
     * even though the fixture's bytes are still sitting where it would be.
     */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.fadt.header.length = FADT_MINIMUM_SIZE;
    set_checksum(&fixture.fadt, FADT_MINIMUM_SIZE);

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_OK ||
        fadt.pm_timer_extended_address ||
        fadt.length != FADT_MINIMUM_SIZE ||
        fadt.pm_timer_port != (uint16_t)TEST_PM_TIMER_FIXED_PORT) {
        return false;
    }

    root = test_root(&fixture, ACPI_ROOT_RSDT);
    return acpi_fadt_discover(&root, &fadt) == ACPI_STATUS_OK;
}

/*
 * Every refusal decode_pm_timer and the FADT walk can produce, and the proof
 * that a refusal leaves nothing behind that could be mistaken for a result.
 */
static bool fadt_rejections_are_refused(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_fadt fadt;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    if (acpi_fadt_discover(NULL, &fadt) != ACPI_STATUS_NULL_ARGUMENT ||
        acpi_fadt_discover(&root, NULL) != ACPI_STATUS_NULL_ARGUMENT) {
        return false;
    }

    /* A table too short to hold the fields ACPI 1.0 guarantees. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.fadt.header.length = FADT_MINIMUM_SIZE - 1U;
    set_checksum(&fixture.fadt, FADT_MINIMUM_SIZE - 1U);

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_FADT_LENGTH ||
        fadt.physical_address != 0U ||
        fadt.length != 0U ||
        fadt.pm_timer_port != 0U ||
        fadt.pm_timer_counter_bits != 0U ||
        fadt.pm_timer_extended_address) {
        return false;
    }

    /* A block length of zero is how firmware says the timer is absent. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    *fadt_field(&fixture.fadt, FADT_OFFSET_PM_TMR_LEN) = 0U;
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_LENGTH) {
        return false;
    }

    /* Any other width describes a register this reader cannot address. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    *fadt_field(&fixture.fadt, FADT_OFFSET_PM_TMR_LEN) = 8U;
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_LENGTH) {
        return false;
    }

    /* Neither address present at all. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK) +
            ACPI_GAS_OFFSET_ADDRESS,
        0U
    );
    write_u32(fadt_field(&fixture.fadt, FADT_OFFSET_PM_TMR_BLK), 0U);
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_MISSING_PM_TIMER) {
        return false;
    }

    /* A fixed port whose last byte falls outside the I/O address space. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK) +
            ACPI_GAS_OFFSET_ADDRESS,
        0U
    );
    write_u32(
        fadt_field(&fixture.fadt, FADT_OFFSET_PM_TMR_BLK),
        UINT32_C(0xFFFE)
    );
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_PORT) {
        return false;
    }

    /* An extended address claiming System I/O beyond where ports exist. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK) +
            ACPI_GAS_OFFSET_ADDRESS,
        UINT64_C(0x10000)
    );
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_PORT) {
        return false;
    }

    /* A selected block that contradicts the register width it must have. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK)
        [ACPI_GAS_OFFSET_BIT_WIDTH] = ACPI_PM_TIMER_BASE_BITS;
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_BLOCK) {
        return false;
    }

    /* Or one that places the counter somewhere inside the register. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fadt_field(&fixture.fadt, FADT_OFFSET_X_PM_TMR_BLK)
        [ACPI_GAS_OFFSET_BIT_OFFSET] = 8U;
    set_checksum(&fixture.fadt, sizeof(fixture.fadt));

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_BAD_PM_TIMER_BLOCK) {
        return false;
    }

    /* A root that references no FADT at all. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length =
        sizeof(struct acpi_sdt_header) + 2U * sizeof(uint64_t);
    set_checksum(&fixture.xsdt, fixture.xsdt.header.length);

    if (acpi_fadt_discover(&root, &fadt) != ACPI_STATUS_MISSING_FADT) {
        return false;
    }

    /* Two of them, which position must not be allowed to resolve. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] = (uint64_t)(uintptr_t)(void *)&fixture.fadt;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    return acpi_fadt_discover(&root, &fadt) == ACPI_STATUS_DUPLICATE_FADT;
}

/*
 * Every MCFG rejection, driven by editing a fixture that is otherwise accepted,
 * so each check is reached with everything before it valid. The first case is
 * the acceptance, which is what makes the rest of them mean anything.
 */
static bool valid_mcfg_is_accepted(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_mcfg mcfg;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_OK ||
        mcfg.physical_address != (uint64_t)(uintptr_t)(void *)&fixture.mcfg ||
        mcfg.length != sizeof(fixture.mcfg) ||
        mcfg.revision != 1U ||
        mcfg.allocation_count != ACPI_TEST_MCFG_ALLOCATIONS ||
        mcfg.allocations[0].base_address != TEST_ECAM_BASE ||
        mcfg.allocations[0].segment != TEST_ECAM_SEGMENT ||
        mcfg.allocations[0].start_bus != 0U ||
        mcfg.allocations[0].end_bus != TEST_ECAM_END_BUS ||
        mcfg.allocations[1].base_address != TEST_ECAM_SECOND_BASE ||
        mcfg.allocations[1].segment != TEST_ECAM_SECOND_SEGMENT ||
        !acpi_bytes_equal(mcfg.oem_id, test_oem_id, sizeof(test_oem_id))) {
        return false;
    }

    if (acpi_ecam_allocation_size(&mcfg.allocations[0]) !=
            ((uint64_t)TEST_ECAM_END_BUS + 1U) * ACPI_ECAM_BUS_SIZE ||
        acpi_ecam_allocation_size(NULL) != 0U) {
        return false;
    }

    /* The same table reached through the 32-bit root, which is a separate walk. */
    root = test_root(&fixture, ACPI_ROOT_RSDT);
    return acpi_mcfg_discover(&root, &mcfg) == ACPI_STATUS_OK &&
        mcfg.allocation_count == ACPI_TEST_MCFG_ALLOCATIONS;
}

static bool mcfg_rejections_are_named(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_mcfg mcfg;
    struct acpi_root root;

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    if (acpi_mcfg_discover(NULL, &mcfg) != ACPI_STATUS_NULL_ARGUMENT ||
        acpi_mcfg_discover(&root, NULL) != ACPI_STATUS_NULL_ARGUMENT) {
        return false;
    }

    /*
     * A payload that is not a whole number of allocation structures. The
     * checksum is restored each time, so the length is what is being tested
     * rather than the corruption editing it causes.
     */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.mcfg.header.length = sizeof(fixture.mcfg) - 1U;
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg) - 1U);

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_BAD_MCFG_LENGTH) {
        return false;
    }

    /* A table that stops before its first allocation structure. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.mcfg.header.length = ACPI_MCFG_FIXED_SIZE;
    set_checksum(&fixture.mcfg, ACPI_MCFG_FIXED_SIZE);

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_NO_ECAM_ALLOCATIONS) {
        return false;
    }

    /* A window whose last bus precedes its first. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    *mcfg_field(&fixture.mcfg, 0U, MCFG_OFFSET_START_BUS) =
        TEST_ECAM_END_BUS + 1U;
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_BAD_ECAM_BUS_RANGE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(mcfg_field(&fixture.mcfg, 0U, MCFG_OFFSET_BASE_ADDRESS), 0U);
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_BAD_ECAM_BASE) {
        return false;
    }

    /* One byte short of 1 MiB alignment, which is the whole addressing scheme. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        mcfg_field(&fixture.mcfg, 0U, MCFG_OFFSET_BASE_ADDRESS),
        TEST_ECAM_BASE + 1U
    );
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_BAD_ECAM_BASE) {
        return false;
    }

    /*
     * A base so high that its own bus range runs past the end of the address
     * space. Refused rather than wrapped to a low address that reads as memory.
     */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        mcfg_field(&fixture.mcfg, 0U, MCFG_OFFSET_BASE_ADDRESS),
        UINT64_MAX - ACPI_ECAM_BUS_SIZE + 1U
    );
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_BAD_ECAM_BASE) {
        return false;
    }

    /* Two windows of different segments placed on the same memory. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u64(
        mcfg_field(&fixture.mcfg, 1U, MCFG_OFFSET_BASE_ADDRESS),
        TEST_ECAM_BASE
    );
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) !=
        ACPI_STATUS_OVERLAPPING_ECAM_RANGE) {
        return false;
    }

    /* Two windows of the same segment claiming the same buses. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    write_u16(
        mcfg_field(&fixture.mcfg, 1U, MCFG_OFFSET_SEGMENT),
        TEST_ECAM_SEGMENT
    );
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) !=
        ACPI_STATUS_OVERLAPPING_ECAM_RANGE) {
        return false;
    }

    /*
     * Absence is the one outcome here that is not a fault. The signature is
     * edited rather than the table removed, so the walk still visits it and
     * still has to conclude that no MCFG is present.
     */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.mcfg.header.signature[0] = 'N';
    set_checksum(&fixture.mcfg, sizeof(fixture.mcfg));

    if (acpi_mcfg_discover(&root, &mcfg) != ACPI_STATUS_MISSING_MCFG ||
        mcfg.allocation_count != 0U) {
        return false;
    }

    /* The same table twice, which cannot be resolved by position. */
    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);

    for (size_t index = 0; index < sizeof(fixture.other.header.signature);
         ++index) {
        fixture.other.header.signature[index] = mcfg_signature[index];
    }

    set_checksum(&fixture.other, sizeof(fixture.other));
    return acpi_mcfg_discover(&root, &mcfg) == ACPI_STATUS_DUPLICATE_MCFG;
}

bool acpi_tables_self_test(void)
{
    struct acpi_test_fixture fixture;
    struct acpi_madt madt;
    struct acpi_root root;

    if (!valid_fixture_is_accepted() || !valid_fadt_is_accepted() ||
        !valid_mcfg_is_accepted() || !mcfg_rejections_are_named()) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.signature[0] = 'B';

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_SIGNATURE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    root.physical_address = 0U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_NULL_ROOT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    root.physical_address =
        SENERI_EARLY_PHYSICAL_LIMIT - ACPI_SDT_HEADER_SIZE + 1U;

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_ROOT_OUTSIDE_EARLY_MAP) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) - 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) + 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_ENTRY_SIZE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) +
        (ACPI_MAX_ROOT_ENTRIES + 1U) * sizeof(uint64_t);

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_TOO_MANY_ROOT_ENTRIES) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.checksum ^= UINT8_C(1);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_ROOT_CHECKSUM) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] = 0U;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_NULL_TABLE) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] =
        SENERI_EARLY_PHYSICAL_LIMIT - ACPI_SDT_HEADER_SIZE + 1U;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) !=
        ACPI_STATUS_TABLE_OUTSIDE_EARLY_MAP) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.other.header.length = sizeof(struct acpi_sdt_header) - 1U;

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_TABLE_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.other.header.checksum ^= UINT8_C(1);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_TABLE_CHECKSUM) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.header.length = sizeof(struct acpi_sdt_header) + sizeof(uint64_t);
    set_checksum(&fixture.xsdt, fixture.xsdt.header.length);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_MISSING_MADT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.xsdt.entries[0] =
        (uint64_t)(uintptr_t)(void *)&fixture.madt;
    set_checksum(&fixture.xsdt, sizeof(fixture.xsdt));

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_DUPLICATE_MADT) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.madt.header.length = sizeof(struct acpi_sdt_header);
    set_checksum(&fixture.madt, fixture.madt.header.length);

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_MADT_LENGTH) {
        return false;
    }

    prepare_test_fixture(&fixture);
    root = test_root(&fixture, ACPI_ROOT_XSDT);
    fixture.madt.flags = UINT32_C(2);
    set_checksum(&fixture.madt, sizeof(fixture.madt));

    if (acpi_madt_discover(&root, &madt) != ACPI_STATUS_BAD_MADT_FLAGS) {
        return false;
    }

    return fadt_rejections_are_refused();
}
