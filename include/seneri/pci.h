/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SENERI_PCI_H
#define SENERI_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>

/*
 * PCI Local Bus Specification 3.0 section 3.2.2.3.2 defines configuration
 * mechanism #1: a 32-bit address latched into one port and the selected
 * register read through the next. It reaches the first 256 bytes of every
 * function of every bus of segment group zero, needs no mapping, and has been
 * present on every PC since PCI existed. It is what Seneri enumerates through.
 */
#define PCI_CONFIG_ADDRESS_PORT UINT16_C(0x0CF8)
#define PCI_CONFIG_DATA_PORT UINT16_C(0x0CFC)
#define PCI_CONFIG_ENABLE UINT32_C(0x80000000)

/*
 * The address is a dword selector: bit 31 enables it, bits 23:16 are the bus,
 * 15:11 the device, 10:8 the function and 7:2 the register. Bits 30:24 and 1:0
 * carry nothing. This mask is every bit that means something, and the presence
 * probe compares a read-back through it rather than against it, because an
 * implementation is free to preserve the bits that mean nothing.
 */
#define PCI_CONFIG_ADDRESS_MASK UINT32_C(0x80FFFFFC)

#define PCI_MAX_BUSES 256U
#define PCI_DEVICES_PER_BUS 32U
#define PCI_FUNCTIONS_PER_DEVICE 8U

/* The configuration space every mechanism reaches, and the dwords in it. */
#define PCI_CONFIG_SPACE_SIZE 256U
#define PCI_CONFIG_SPACE_DWORDS (PCI_CONFIG_SPACE_SIZE / 4U)

/* PCI Local Bus Specification 3.0 section 6.1, the type 0 and type 1 headers. */
#define PCI_REGISTER_VENDOR_ID UINT16_C(0x00)
#define PCI_REGISTER_DEVICE_ID UINT16_C(0x02)
#define PCI_REGISTER_COMMAND UINT16_C(0x04)
#define PCI_REGISTER_STATUS UINT16_C(0x06)
#define PCI_REGISTER_REVISION UINT16_C(0x08)
#define PCI_REGISTER_PROG_IF UINT16_C(0x09)
#define PCI_REGISTER_SUBCLASS UINT16_C(0x0A)
#define PCI_REGISTER_CLASS UINT16_C(0x0B)
#define PCI_REGISTER_HEADER_TYPE UINT16_C(0x0E)
#define PCI_REGISTER_SECONDARY_BUS UINT16_C(0x19)
#define PCI_REGISTER_SUBORDINATE_BUS UINT16_C(0x1A)
#define PCI_REGISTER_CAPABILITY_POINTER UINT16_C(0x34)

#define PCI_STATUS_CAPABILITY_LIST UINT16_C(0x0010)

#define PCI_HEADER_TYPE_MASK UINT8_C(0x7F)
#define PCI_HEADER_TYPE_MULTIFUNCTION UINT8_C(0x80)
#define PCI_HEADER_TYPE_ENDPOINT UINT8_C(0x00)
#define PCI_HEADER_TYPE_BRIDGE UINT8_C(0x01)
#define PCI_HEADER_TYPE_CARDBUS UINT8_C(0x02)

/*
 * An absent function answers every configuration read with all ones, because
 * nothing drives the bus and it floats high. That is the only way to tell a
 * function is not there, so a vendor identifier of 0xFFFF is definitive and a
 * device identifier of 0xFFFF on its own is not.
 */
#define PCI_VENDOR_ABSENT UINT16_C(0xFFFF)

/*
 * PCI Local Bus Specification 3.0 section 6.7: capabilities are a linked list
 * inside configuration space. The first may not start before 0x40 - everything
 * below that is the standard header - and every pointer is dword aligned. That
 * leaves 48 possible positions, which bounds the walk without a policy number.
 */
#define PCI_CAPABILITY_FIRST_OFFSET UINT8_C(0x40)
#define PCI_MAX_CAPABILITIES \
    ((PCI_CONFIG_SPACE_SIZE - PCI_CAPABILITY_FIRST_OFFSET) / 4U)

/* PCI Code and ID Assignment Specification 1.15, the capability identifiers. */
#define PCI_CAPABILITY_POWER_MANAGEMENT UINT8_C(0x01)
#define PCI_CAPABILITY_MSI UINT8_C(0x05)
#define PCI_CAPABILITY_VENDOR UINT8_C(0x09)
#define PCI_CAPABILITY_EXPRESS UINT8_C(0x10)
#define PCI_CAPABILITY_MSI_X UINT8_C(0x11)

/* PCI Code and ID Assignment Specification 1.15 section 1, the base classes. */
#define PCI_CLASS_MASS_STORAGE UINT8_C(0x01)
#define PCI_CLASS_NETWORK UINT8_C(0x02)
#define PCI_CLASS_DISPLAY UINT8_C(0x03)
#define PCI_CLASS_BRIDGE UINT8_C(0x06)
#define PCI_SUBCLASS_HOST_BRIDGE UINT8_C(0x00)

/*
 * A Seneri early-boot policy bound on how many functions one enumeration may
 * record, not an architectural one - the architecture allows 65536. The table
 * is one kernel heap allocation made at pci_initialize and released at
 * pci_shutdown, following the pattern src/kernel/timer.c set: never per
 * operation, because the heap is not reentrant.
 */
#define PCI_MAX_FUNCTIONS 64U

enum pci_status {
    PCI_STATUS_OK = 0,
    PCI_STATUS_NULL_ARGUMENT,
    PCI_STATUS_ALREADY_INITIALIZED,
    PCI_STATUS_NOT_INITIALIZED,
    PCI_STATUS_INTERRUPTS_ENABLED,
    PCI_STATUS_NO_HEAP,
    PCI_STATUS_NO_MEMORY,
    PCI_STATUS_NO_MECHANISM,
    PCI_STATUS_BAD_ADDRESS,
    PCI_STATUS_BAD_OFFSET,
    PCI_STATUS_NO_ECAM,
    PCI_STATUS_OUTSIDE_ECAM_WINDOW,
    PCI_STATUS_TOO_MANY_FUNCTIONS,
    PCI_STATUS_BAD_CAPABILITY_POINTER,
    PCI_STATUS_CAPABILITY_LOOP,
    PCI_STATUS_BAD_BRIDGE_BUS,
    PCI_STATUS_MECHANISM_DISAGREEMENT,
    PCI_STATUS_VALIDATION_FAILURE
};

/*
 * Where a function sits. The segment group is carried because the MCFG names
 * it, even though only group zero is reachable through the I/O ports and only
 * the first group's window is mapped.
 */
struct pci_address {
    uint16_t segment;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct pci_capability {
    uint8_t identifier;
    uint8_t offset;
};

struct pci_function {
    struct pci_address address;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    bool multifunction;
    /* Meaningful only for a bridge; zero on an endpoint. */
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    /*
     * Offsets of the capabilities worth naming, or zero for absent. Zero cannot
     * be a real capability offset because the list may not start below 0x40.
     * Message-signalled interrupts are the reason this increment reads them:
     * they are delivered as a memory write to the local APIC and so need no
     * I/O APIC redirection entry at all.
     */
    uint8_t msi_offset;
    uint8_t msi_x_offset;
    uint8_t express_offset;
    size_t capability_count;
    struct pci_capability capabilities[PCI_MAX_CAPABILITIES];
};

struct pci_state {
    bool active;
    /* Whether a configuration window was both declared and mapped. */
    bool ecam_active;
    uint64_t ecam_base;
    uint64_t ecam_size;
    uint8_t ecam_start_bus;
    uint8_t ecam_end_bus;
    uint16_t ecam_segment;
    size_t capacity;
    size_t function_count;
    size_t bus_count;
    size_t bridge_count;
    /*
     * What the cross-check managed to compare. A register that does not read
     * the same twice through one mechanism cannot say anything about two, so it
     * is counted as volatile and skipped rather than reported as a
     * disagreement.
     */
    size_t compared_functions;
    size_t compared_dwords;
    size_t volatile_dwords;
};

enum pci_status pci_initialize(const struct acpi_mcfg *mcfg, bool mcfg_present);
enum pci_status pci_shutdown(void);
bool pci_is_initialized(void);

/*
 * Read one aligned 32-bit configuration register. Both readers take the same
 * address and offset and must answer identically for every register that reads
 * the same twice; pci_verify is what asserts that.
 */
enum pci_status pci_config_read_port(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
);
enum pci_status pci_config_read_ecam(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
);

size_t pci_function_count(void);
const struct pci_function *pci_function_at(size_t index);
const struct pci_function *pci_find_class(uint8_t class_code, uint8_t subclass);
struct pci_state pci_get_state(void);
enum pci_status pci_verify(void);
bool pci_self_test(void);
const char *pci_status_string(enum pci_status status);
const char *pci_class_string(uint8_t class_code);

#endif
