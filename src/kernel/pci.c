/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <seneri/acpi.h>
#include <seneri/console.h>
#include <seneri/cpu.h>
#include <seneri/heap.h>
#include <seneri/paging.h>
#include <seneri/pci.h>

/*
 * Enumerate the PCI configuration space the machine actually has.
 *
 * Nothing above this layer can name a device until something below it has
 * counted them, and every driver Seneri will ever have - storage, network,
 * wireless - starts by being found here. So this increment finds them and
 * reports what they are, and deliberately does nothing else: every access it
 * makes is a read, so enumerating cannot disturb a device that is already
 * working. Sizing a base address register means writing all ones into it, and
 * writing to configuration space belongs to the increment that also owns a
 * device rather than to the one that counts them.
 *
 * There are two ways to reach configuration space and this file implements
 * both, because that is what lets each check the other. The I/O ports are
 * always there and reach the first 256 bytes of everything; the memory-mapped
 * window is what firmware declares in the MCFG, needs mapping, and is the only
 * way to reach the extended space beyond 256 bytes. Enumeration runs on the
 * ports, because they need no mapping and are bounded by nothing. The window is
 * then required to agree with them, register for register, on every function
 * that falls inside it.
 */

/* One bus is 1 MiB of the window, one device 32 KiB, one function 4 KiB. */
#define ECAM_BUS_SHIFT 20U
#define ECAM_DEVICE_SHIFT 15U
#define ECAM_FUNCTION_SHIFT 12U

/*
 * A read repeated this many times through one mechanism before it is compared
 * against the other. Two reads are enough to catch a register that changes on
 * its own; more would only slow a boot-time walk down.
 */
#define STABILITY_READS 2U

static struct pci_state state;
static struct pci_function *functions;

static uint32_t port_config_address(struct pci_address address, uint16_t offset)
{
    return PCI_CONFIG_ENABLE |
        ((uint32_t)address.bus << 16) |
        ((uint32_t)address.device << 11) |
        ((uint32_t)address.function << 8) |
        ((uint32_t)offset & UINT32_C(0xFC));
}

/*
 * The address a function's register occupies inside the mapped window, or zero
 * when it falls outside it. Zero is safe to use as the refusal because the
 * window's base is a firmware address that discovery already refused to accept
 * as zero.
 */
static uint64_t ecam_address(struct pci_address address, uint16_t offset)
{
    uint64_t displacement;

    if (!state.ecam_active || address.segment != state.ecam_segment ||
        address.bus < state.ecam_start_bus || address.bus > state.ecam_end_bus) {
        return 0U;
    }

    displacement =
        ((uint64_t)(address.bus - state.ecam_start_bus) << ECAM_BUS_SHIFT) |
        ((uint64_t)address.device << ECAM_DEVICE_SHIFT) |
        ((uint64_t)address.function << ECAM_FUNCTION_SHIFT) |
        (uint64_t)offset;

    /*
     * Seneri maps one 2 MiB region of a window firmware may declare far larger,
     * so the mapped size is the bound that matters rather than the declared
     * one. A register past it is refused, not wrapped.
     */
    if (displacement + sizeof(uint32_t) > state.ecam_size) {
        return 0U;
    }

    return state.ecam_base + displacement;
}

static enum pci_status validate_access(
    struct pci_address address,
    uint16_t offset
)
{
    if (address.device >= PCI_DEVICES_PER_BUS ||
        address.function >= PCI_FUNCTIONS_PER_DEVICE) {
        return PCI_STATUS_BAD_ADDRESS;
    }

    /*
     * Aligned, and inside the space both mechanisms reach. A misaligned dword
     * read of configuration space is not a narrower read, it is a different
     * register, so it is refused rather than rounded down.
     */
    if (offset % sizeof(uint32_t) != 0U ||
        offset > PCI_CONFIG_SPACE_SIZE - sizeof(uint32_t)) {
        return PCI_STATUS_BAD_OFFSET;
    }

    return PCI_STATUS_OK;
}

enum pci_status pci_config_read_port(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
)
{
    enum pci_status status;

    if (value == NULL) {
        return PCI_STATUS_NULL_ARGUMENT;
    }

    *value = 0U;
    status = validate_access(address, offset);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    /*
     * Mechanism #1 carries no segment group. A function outside group zero is
     * simply not addressable this way, and answering with group zero's register
     * would be the wrong device rather than a near miss.
     */
    if (address.segment != 0U) {
        return PCI_STATUS_BAD_ADDRESS;
    }

    cpu_out32(PCI_CONFIG_ADDRESS_PORT, port_config_address(address, offset));
    *value = cpu_in32(PCI_CONFIG_DATA_PORT);
    return PCI_STATUS_OK;
}

enum pci_status pci_config_read_ecam(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
)
{
    enum pci_status status;
    uint64_t location;

    if (value == NULL) {
        return PCI_STATUS_NULL_ARGUMENT;
    }

    *value = 0U;
    status = validate_access(address, offset);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    if (!state.ecam_active) {
        return PCI_STATUS_NO_ECAM;
    }

    location = ecam_address(address, offset);

    if (location == 0U) {
        return PCI_STATUS_OUTSIDE_ECAM_WINDOW;
    }

    /*
     * Volatile because the compiler must not fold, reorder or drop a read of
     * device memory. The window is mapped uncacheable, so this is a bus cycle
     * rather than a cache hit; docs/PCI_ENUMERATION.md says why that matters.
     */
    *value = *(const volatile uint32_t *)(uintptr_t)location;
    return PCI_STATUS_OK;
}

/*
 * Read a register through the ports until two consecutive reads agree, or give
 * up and report that it does not hold still. A register that changes on its own
 * cannot be used to compare two mechanisms, and pretending otherwise would turn
 * a device's own activity into a false disagreement.
 */
static bool port_read_stable(
    struct pci_address address,
    uint16_t offset,
    uint32_t *value
)
{
    uint32_t first = 0U;
    uint32_t second = 0U;

    for (unsigned int attempt = 0; attempt < STABILITY_READS; ++attempt) {
        if (pci_config_read_port(address, offset, &first) != PCI_STATUS_OK ||
            pci_config_read_port(address, offset, &second) != PCI_STATUS_OK) {
            return false;
        }

        if (first == second) {
            *value = first;
            return true;
        }
    }

    return false;
}

static uint8_t byte_of(uint32_t dword, uint16_t offset)
{
    return (uint8_t)(dword >> ((offset % sizeof(uint32_t)) * 8U));
}

static uint16_t word_of(uint32_t dword, uint16_t offset)
{
    return (uint16_t)(dword >> ((offset % sizeof(uint32_t)) * 8U));
}

static enum pci_status read_aligned(
    struct pci_address address,
    uint16_t offset,
    uint32_t *dword
)
{
    return pci_config_read_port(
        address,
        (uint16_t)(offset & ~UINT16_C(3)),
        dword
    );
}

/*
 * A capability pointer is one byte and every position is dword aligned, so the
 * largest value it can hold after masking is 0xFC - which is exactly the last
 * register of configuration space. The walk therefore needs no upper bound of
 * its own: the type is the bound. This is asserted rather than checked at
 * runtime because a check no input can reach is a check no test can drive.
 */
_Static_assert(
    (UINT8_MAX & UINT8_C(0xFC)) == PCI_CONFIG_SPACE_SIZE - sizeof(uint32_t),
    "a dword-aligned byte capability pointer no longer bounds itself"
);

/*
 * Walk one function's capability list. The list is firmware-controlled and a
 * corrupt next pointer is the classic way to hang an enumerator, so every step
 * is bounded: a pointer below 0x40 is inside the standard header and refused, a
 * position already visited is a cycle and refused, and the number of
 * dword-aligned positions in configuration space bounds the walk even if both
 * of those somehow passed.
 */
static enum pci_status collect_capabilities(struct pci_function *function)
{
    bool visited[PCI_CONFIG_SPACE_SIZE / 4U] = {false};
    uint32_t dword = 0U;
    uint16_t offset;

    function->capability_count = 0U;
    function->msi_offset = 0U;
    function->msi_x_offset = 0U;
    function->express_offset = 0U;

    if ((function->status & PCI_STATUS_CAPABILITY_LIST) == 0U) {
        return PCI_STATUS_OK;
    }

    if (read_aligned(
            function->address,
            PCI_REGISTER_CAPABILITY_POINTER,
            &dword
        ) != PCI_STATUS_OK) {
        return PCI_STATUS_VALIDATION_FAILURE;
    }

    offset = byte_of(dword, PCI_REGISTER_CAPABILITY_POINTER) & UINT8_C(0xFC);

    while (offset != 0U) {
        uint8_t identifier;

        if (offset < PCI_CAPABILITY_FIRST_OFFSET) {
            return PCI_STATUS_BAD_CAPABILITY_POINTER;
        }

        if (visited[offset / 4U]) {
            return PCI_STATUS_CAPABILITY_LOOP;
        }

        visited[offset / 4U] = true;

        if (function->capability_count >= PCI_MAX_CAPABILITIES) {
            return PCI_STATUS_CAPABILITY_LOOP;
        }

        if (read_aligned(function->address, offset, &dword) != PCI_STATUS_OK) {
            return PCI_STATUS_VALIDATION_FAILURE;
        }

        identifier = byte_of(dword, offset);
        function->capabilities[function->capability_count].identifier =
            identifier;
        function->capabilities[function->capability_count].offset =
            (uint8_t)offset;
        ++function->capability_count;

        switch (identifier) {
        case PCI_CAPABILITY_MSI:
            function->msi_offset = (uint8_t)offset;
            break;
        case PCI_CAPABILITY_MSI_X:
            function->msi_x_offset = (uint8_t)offset;
            break;
        case PCI_CAPABILITY_EXPRESS:
            function->express_offset = (uint8_t)offset;
            break;
        default:
            break;
        }

        offset = byte_of(dword, (uint16_t)(offset + 1U)) & UINT8_C(0xFC);
    }

    return PCI_STATUS_OK;
}

static enum pci_status record_function(
    struct pci_address address,
    uint32_t identity,
    bool *recorded
)
{
    struct pci_function *function;
    uint32_t dword = 0U;
    enum pci_status status;

    *recorded = false;

    if (state.function_count >= state.capacity) {
        return PCI_STATUS_TOO_MANY_FUNCTIONS;
    }

    function = &functions[state.function_count];
    function->address = address;
    function->vendor_id = word_of(identity, PCI_REGISTER_VENDOR_ID);
    function->device_id = word_of(identity, PCI_REGISTER_DEVICE_ID);

    status = read_aligned(address, PCI_REGISTER_COMMAND, &dword);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    function->command = word_of(dword, PCI_REGISTER_COMMAND);
    function->status = word_of(dword, PCI_REGISTER_STATUS);
    status = read_aligned(address, PCI_REGISTER_REVISION, &dword);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    function->revision = byte_of(dword, PCI_REGISTER_REVISION);
    function->prog_if = byte_of(dword, PCI_REGISTER_PROG_IF);
    function->subclass = byte_of(dword, PCI_REGISTER_SUBCLASS);
    function->class_code = byte_of(dword, PCI_REGISTER_CLASS);
    status = read_aligned(address, PCI_REGISTER_HEADER_TYPE, &dword);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    function->header_type =
        byte_of(dword, PCI_REGISTER_HEADER_TYPE) & PCI_HEADER_TYPE_MASK;
    function->multifunction =
        (byte_of(dword, PCI_REGISTER_HEADER_TYPE) &
            PCI_HEADER_TYPE_MULTIFUNCTION) != 0U;
    function->secondary_bus = 0U;
    function->subordinate_bus = 0U;

    if (function->header_type == PCI_HEADER_TYPE_BRIDGE) {
        status = read_aligned(address, PCI_REGISTER_SECONDARY_BUS, &dword);

        if (status != PCI_STATUS_OK) {
            return status;
        }

        function->secondary_bus = byte_of(dword, PCI_REGISTER_SECONDARY_BUS);
        function->subordinate_bus =
            byte_of(dword, PCI_REGISTER_SUBORDINATE_BUS);
        ++state.bridge_count;
    }

    status = collect_capabilities(function);

    if (status != PCI_STATUS_OK) {
        return status;
    }

    ++state.function_count;
    *recorded = true;
    return PCI_STATUS_OK;
}

/*
 * Scan every bus reachable from bus zero.
 *
 * Buses are found through the bridges that lead to them, so the walk is a
 * worklist rather than a loop over all 256: a machine that populates bus 0 and
 * bus 5 should not have 254 empty buses probed. Each bus is scanned at most
 * once, which is what makes a bridge whose secondary bus points back at an
 * already-scanned bus - a loop firmware should never build, and a hang if it
 * did - terminate instead.
 */
static enum pci_status enumerate(void)
{
    bool scanned[PCI_MAX_BUSES] = {false};
    bool pending[PCI_MAX_BUSES] = {false};
    bool progress = true;

    pending[0] = true;

    while (progress) {
        progress = false;

        for (size_t bus = 0; bus < PCI_MAX_BUSES; ++bus) {
            struct pci_address address;

            if (!pending[bus] || scanned[bus]) {
                continue;
            }

            pending[bus] = false;
            scanned[bus] = true;
            progress = true;
            ++state.bus_count;
            address.segment = 0U;
            address.bus = (uint8_t)bus;

            for (uint8_t device = 0; device < PCI_DEVICES_PER_BUS; ++device) {
                uint8_t function_limit = 1U;

                address.device = device;

                for (uint8_t function = 0; function < function_limit;
                     ++function) {
                    uint32_t identity = 0U;
                    enum pci_status status;
                    bool recorded = false;

                    address.function = function;
                    status = read_aligned(
                        address,
                        PCI_REGISTER_VENDOR_ID,
                        &identity
                    );

                    if (status != PCI_STATUS_OK) {
                        return status;
                    }

                    if (word_of(identity, PCI_REGISTER_VENDOR_ID) ==
                        PCI_VENDOR_ABSENT) {
                        continue;
                    }

                    status = record_function(address, identity, &recorded);

                    if (status != PCI_STATUS_OK) {
                        return status;
                    }

                    if (!recorded) {
                        continue;
                    }

                    /*
                     * Only function zero's header type says whether the others
                     * exist. Probing 1 through 7 on a single-function device is
                     * how an enumerator invents devices that are not there.
                     */
                    if (function == 0U &&
                        functions[state.function_count - 1U].multifunction) {
                        function_limit = PCI_FUNCTIONS_PER_DEVICE;
                    }

                    if (functions[state.function_count - 1U].header_type ==
                        PCI_HEADER_TYPE_BRIDGE) {
                        const uint8_t secondary =
                            functions[state.function_count - 1U].secondary_bus;

                        /*
                         * A bridge whose secondary bus is its own primary bus
                         * describes a cycle. It is refused by name rather than
                         * skipped, because it means the description is wrong
                         * and something else read from it will be wrong too.
                         */
                        if (secondary == address.bus) {
                            return PCI_STATUS_BAD_BRIDGE_BUS;
                        }

                        if (!scanned[secondary]) {
                            pending[secondary] = true;
                        }
                    }
                }
            }
        }
    }

    return PCI_STATUS_OK;
}

/*
 * Require the memory-mapped window to agree with the I/O ports about every
 * register of every function that falls inside it.
 *
 * This is the same argument the three clocks make. Two mechanisms that were
 * built independently and reach the same registers by completely different
 * routes - a port pair versus a mapped page - have no reason to agree unless
 * both are addressing the device the enumeration thinks they are. An off-by-one
 * in the window's bus arithmetic, a base taken from the wrong allocation, or a
 * cached read of device memory would all show up here as one mechanism
 * answering about a different function than the other.
 */
static enum pci_status compare_mechanisms(void)
{
    state.compared_functions = 0U;
    state.compared_dwords = 0U;
    state.volatile_dwords = 0U;

    if (!state.ecam_active) {
        return PCI_STATUS_OK;
    }

    for (size_t index = 0; index < state.function_count; ++index) {
        const struct pci_address address = functions[index].address;
        bool compared = false;

        if (ecam_address(address, 0U) == 0U) {
            continue;
        }

        for (uint16_t offset = 0U;
             offset <= PCI_CONFIG_SPACE_SIZE - sizeof(uint32_t);
             offset = (uint16_t)(offset + sizeof(uint32_t))) {
            uint32_t through_ports = 0U;
            uint32_t through_window = 0U;

            if (!port_read_stable(address, offset, &through_ports)) {
                ++state.volatile_dwords;
                continue;
            }

            if (pci_config_read_ecam(address, offset, &through_window) !=
                PCI_STATUS_OK) {
                return PCI_STATUS_VALIDATION_FAILURE;
            }

            if (through_ports != through_window) {
                return PCI_STATUS_MECHANISM_DISAGREEMENT;
            }

            ++state.compared_dwords;
            compared = true;
        }

        if (compared) {
            ++state.compared_functions;
        }
    }

    return PCI_STATUS_OK;
}

/*
 * Prove the port mechanism is there before anything is read through it.
 *
 * A machine without it decodes nothing at 0xCF8 and answers 0xFFFFFFFF or zero,
 * so the test is whether the port behaves like a register: write a value and
 * get that value back. Writing the enable bit alone and requiring exactly it
 * back is the canonical probe, because that pattern sets no reserved bit and so
 * reads back identically whether an implementation preserves them or not.
 *
 * A second write saturates every field and the read-back is compared through a
 * mask. The reserved bits are deliberately not required to read zero: the rule
 * that reserved bits read zero belongs to PCI configuration space, and this
 * port is a platform register rather than part of it. Requiring it here was the
 * first version of this function, and it refused to boot on a machine where
 * everything worked - the check was wrong, not the machine.
 */
static bool port_mechanism_present(void)
{
    uint32_t enabled;
    uint32_t saturated;

    cpu_out32(PCI_CONFIG_ADDRESS_PORT, PCI_CONFIG_ENABLE);
    enabled = cpu_in32(PCI_CONFIG_ADDRESS_PORT);
    cpu_out32(PCI_CONFIG_ADDRESS_PORT, PCI_CONFIG_ADDRESS_MASK);
    saturated = cpu_in32(PCI_CONFIG_ADDRESS_PORT);
    cpu_out32(PCI_CONFIG_ADDRESS_PORT, 0U);

    return enabled == PCI_CONFIG_ENABLE &&
        (saturated & PCI_CONFIG_ADDRESS_MASK) == PCI_CONFIG_ADDRESS_MASK;
}

static void reset_state(void)
{
    state.active = false;
    state.ecam_active = false;
    state.ecam_base = 0U;
    state.ecam_size = 0U;
    state.ecam_start_bus = 0U;
    state.ecam_end_bus = 0U;
    state.ecam_segment = 0U;
    state.capacity = 0U;
    state.function_count = 0U;
    state.bus_count = 0U;
    state.bridge_count = 0U;
    state.compared_functions = 0U;
    state.compared_dwords = 0U;
    state.volatile_dwords = 0U;
}

/*
 * Adopt the window paging decided it could reach, if firmware declared one that
 * matches it. Paging owns the address space, so paging decides what is mapped;
 * this only has to agree that the window it is about to read is the one that
 * was made uncacheable, and refuse to use it otherwise.
 */
static void adopt_ecam_window(const struct acpi_mcfg *mcfg, bool mcfg_present)
{
    const struct paging_state paging = paging_get_state();
    const struct acpi_ecam_allocation *allocation;
    uint64_t declared;

    if (!mcfg_present || mcfg == NULL || mcfg->allocation_count == 0U) {
        return;
    }

    if (paging.ecam_window_base == 0U || paging.ecam_window_size == 0U) {
        return;
    }

    allocation = &mcfg->allocations[0];

    if (allocation->base_address != paging.ecam_window_base) {
        return;
    }

    declared = acpi_ecam_allocation_size(allocation);
    state.ecam_base = paging.ecam_window_base;
    state.ecam_size = declared < paging.ecam_window_size
        ? declared
        : paging.ecam_window_size;
    state.ecam_segment = allocation->segment;
    state.ecam_start_bus = allocation->start_bus;

    /*
     * The last bus this kernel can reach through the mapped region, which is
     * not necessarily the last bus firmware declared. Clamping here is what
     * keeps ecam_address from computing a displacement past the mapping.
     */
    state.ecam_end_bus = (uint8_t)(state.ecam_start_bus +
        (state.ecam_size / ACPI_ECAM_BUS_SIZE) - 1U);

    if (state.ecam_end_bus > allocation->end_bus) {
        state.ecam_end_bus = allocation->end_bus;
    }

    state.ecam_active = state.ecam_size >= ACPI_ECAM_BUS_SIZE;
}

enum pci_status pci_initialize(const struct acpi_mcfg *mcfg, bool mcfg_present)
{
    void *table = NULL;
    enum pci_status status;

    if (state.active) {
        return PCI_STATUS_ALREADY_INITIALIZED;
    }

    /*
     * The port pair is two registers used as one, so a read that lands between
     * the address write and the data read answers about a different function.
     * Nothing in Seneri reads configuration space from an interrupt handler,
     * and this is the refusal that keeps it that way.
     */
    if (cpu_interrupts_enabled()) {
        return PCI_STATUS_INTERRUPTS_ENABLED;
    }

    if (!heap_is_active()) {
        return PCI_STATUS_NO_HEAP;
    }

    reset_state();

    if (!port_mechanism_present()) {
        return PCI_STATUS_NO_MECHANISM;
    }

    if (heap_allocate(
            (uint64_t)PCI_MAX_FUNCTIONS * sizeof(struct pci_function),
            &table
        ) != HEAP_STATUS_OK) {
        return PCI_STATUS_NO_MEMORY;
    }

    functions = (struct pci_function *)table;
    state.capacity = PCI_MAX_FUNCTIONS;
    adopt_ecam_window(mcfg, mcfg_present);
    status = enumerate();

    if (status == PCI_STATUS_OK) {
        status = compare_mechanisms();
    }

    if (status != PCI_STATUS_OK) {
        (void)heap_free(table);
        functions = NULL;
        reset_state();
        return status;
    }

    state.active = true;
    return PCI_STATUS_OK;
}

enum pci_status pci_shutdown(void)
{
    if (!state.active) {
        return PCI_STATUS_NOT_INITIALIZED;
    }

    if (heap_free(functions) != HEAP_STATUS_OK) {
        return PCI_STATUS_VALIDATION_FAILURE;
    }

    functions = NULL;
    reset_state();
    return PCI_STATUS_OK;
}

bool pci_is_initialized(void)
{
    return state.active;
}

size_t pci_function_count(void)
{
    return state.active ? state.function_count : 0U;
}

const struct pci_function *pci_function_at(size_t index)
{
    if (!state.active || index >= state.function_count) {
        return NULL;
    }

    return &functions[index];
}

const struct pci_function *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    if (!state.active) {
        return NULL;
    }

    for (size_t index = 0; index < state.function_count; ++index) {
        if (functions[index].class_code == class_code &&
            functions[index].subclass == subclass) {
            return &functions[index];
        }
    }

    return NULL;
}

struct pci_state pci_get_state(void)
{
    return state;
}

/*
 * Re-read what the enumeration recorded and require the machine to still say
 * the same thing. Called at the end of boot for the same reason paging_verify
 * and heap_verify are: everything between enumeration and here ran on the same
 * hardware, and a subsystem that corrupted the address port or the window's
 * mapping would show up nowhere else.
 */
enum pci_status pci_verify(void)
{
    if (!state.active) {
        return PCI_STATUS_NOT_INITIALIZED;
    }

    if (functions == NULL || state.function_count > state.capacity) {
        return PCI_STATUS_VALIDATION_FAILURE;
    }

    for (size_t index = 0; index < state.function_count; ++index) {
        const struct pci_function *function = &functions[index];
        uint32_t identity = 0U;
        uint32_t classification = 0U;

        if (read_aligned(
                function->address,
                PCI_REGISTER_VENDOR_ID,
                &identity
            ) != PCI_STATUS_OK ||
            read_aligned(
                function->address,
                PCI_REGISTER_REVISION,
                &classification
            ) != PCI_STATUS_OK) {
            return PCI_STATUS_VALIDATION_FAILURE;
        }

        if (word_of(identity, PCI_REGISTER_VENDOR_ID) != function->vendor_id ||
            word_of(identity, PCI_REGISTER_DEVICE_ID) != function->device_id ||
            byte_of(classification, PCI_REGISTER_CLASS) !=
                function->class_code ||
            byte_of(classification, PCI_REGISTER_SUBCLASS) !=
                function->subclass) {
            return PCI_STATUS_VALIDATION_FAILURE;
        }
    }

    return PCI_STATUS_OK;
}

const char *pci_class_string(uint8_t class_code)
{
    switch (class_code) {
    case UINT8_C(0x00):
        return "unclassified";
    case PCI_CLASS_MASS_STORAGE:
        return "storage";
    case PCI_CLASS_NETWORK:
        return "network";
    case PCI_CLASS_DISPLAY:
        return "display";
    case UINT8_C(0x04):
        return "multimedia";
    case UINT8_C(0x05):
        return "memory";
    case PCI_CLASS_BRIDGE:
        return "bridge";
    case UINT8_C(0x07):
        return "communication";
    case UINT8_C(0x08):
        return "system peripheral";
    case UINT8_C(0x0C):
        return "serial bus";
    case UINT8_C(0x0D):
        return "wireless";
    default:
        return "other";
    }
}

const char *pci_status_string(enum pci_status status)
{
    static const char *const messages[] = {
        "ok",
        "null PCI argument",
        "PCI enumeration is already initialized",
        "PCI enumeration is not initialized",
        "PCI configuration access needs interrupts disabled",
        "PCI enumeration needs the kernel heap",
        "PCI function table could not be allocated",
        "no PCI configuration mechanism is present",
        "PCI configuration address is out of range",
        "PCI configuration offset is unaligned or out of range",
        "no PCI Express configuration window is mapped",
        "PCI register is outside the mapped configuration window",
        "PCI enumeration exceeds the early function limit",
        "PCI capability pointer is inside the standard header",
        "PCI capability list contains a cycle",
        "PCI bridge names its own bus as its secondary bus",
        "PCI configuration mechanisms disagree about a register",
        "PCI enumeration does not match the machine"
    };

    _Static_assert(
        sizeof(messages) / sizeof(messages[0]) ==
            (size_t)PCI_STATUS_VALIDATION_FAILURE + 1U,
        "PCI status messages are out of sync"
    );

    if ((size_t)status >= sizeof(messages) / sizeof(messages[0])) {
        return "unknown PCI status";
    }

    return messages[status];
}

/*
 * Everything below is arithmetic and list walking, driven by synthetic values.
 * It touches no hardware, so it runs on every boot before the machine is read,
 * and a wrong address composition is named here rather than found as a device
 * that is not there.
 */

/* A synthetic configuration space the capability walk is driven over. */
static uint8_t self_test_space[PCI_CONFIG_SPACE_SIZE];

static bool address_composition_is_right(void)
{
    struct pci_address address;

    address.segment = 0U;
    address.bus = UINT8_C(0xAB);
    address.device = UINT8_C(0x1F);
    address.function = UINT8_C(0x07);

    /*
     * Every field at its maximum, so a shift that is one bit out lands in a
     * neighbouring field rather than off the end where it would be invisible.
     */
    if (port_config_address(address, UINT16_C(0xFC)) !=
        UINT32_C(0x80ABFFFC)) {
        return false;
    }

    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (port_config_address(address, 0U) != PCI_CONFIG_ENABLE) {
        return false;
    }

    /*
     * The low two bits of the offset select a byte inside the register and are
     * not part of the address, so they must be dropped rather than shifted in.
     */
    if (port_config_address(address, UINT16_C(0x0F)) !=
        (PCI_CONFIG_ENABLE | UINT32_C(0x0C))) {
        return false;
    }

    address.bus = 1U;

    if (port_config_address(address, 0U) !=
        (PCI_CONFIG_ENABLE | UINT32_C(0x00010000))) {
        return false;
    }

    address.bus = 0U;
    address.device = 1U;

    if (port_config_address(address, 0U) !=
        (PCI_CONFIG_ENABLE | UINT32_C(0x0800))) {
        return false;
    }

    address.device = 0U;
    address.function = 1U;

    return port_config_address(address, 0U) ==
        (PCI_CONFIG_ENABLE | UINT32_C(0x0100));
}

static bool ecam_arithmetic_is_right(void)
{
    const struct pci_state saved = state;
    struct pci_address address;
    bool correct;

    reset_state();
    state.ecam_active = true;
    state.ecam_base = UINT64_C(0xB0000000);
    state.ecam_size = PAGING_ECAM_WINDOW_SIZE;
    state.ecam_segment = 0U;
    state.ecam_start_bus = 0U;
    state.ecam_end_bus = 1U;

    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    correct = ecam_address(address, 0U) == UINT64_C(0xB0000000);

    address.bus = 1U;
    correct = correct &&
        ecam_address(address, 0U) == UINT64_C(0xB0100000);

    address.bus = 0U;
    address.device = 1U;
    correct = correct &&
        ecam_address(address, 0U) == UINT64_C(0xB0008000);

    address.device = 0U;
    address.function = 1U;
    correct = correct &&
        ecam_address(address, 0U) == UINT64_C(0xB0001000);

    address.function = 0U;
    correct = correct &&
        ecam_address(address, UINT16_C(0xFC)) == UINT64_C(0xB00000FC);

    /* A bus past the mapped region, which is a refusal rather than a wrap. */
    address.bus = 2U;
    correct = correct && ecam_address(address, 0U) == 0U;

    /* A segment the window does not serve. */
    address.bus = 0U;
    address.segment = 1U;
    correct = correct && ecam_address(address, 0U) == 0U;

    /*
     * The last register the last mapped function exposes. It is not the last
     * dword of the window: each function occupies 4 KiB of the window but only
     * its first 256 bytes are configuration space both mechanisms reach, so the
     * highest displacement is the last function's slot plus 0xFC and the 3840
     * bytes of extended space above it are deliberately out of reach here.
     * Writing this expectation as "the end of the window" is the mistake the
     * self-test caught on its first run.
     */
    address.segment = 0U;
    address.bus = 1U;
    address.device = 31U;
    address.function = 7U;
    correct = correct &&
        ecam_address(address, UINT16_C(0xFC)) ==
            state.ecam_base + PAGING_ECAM_WINDOW_SIZE -
                ACPI_ECAM_FUNCTION_SIZE + UINT64_C(0xFC);

    /* And the first register of that same last function, for the other end. */
    correct = correct &&
        ecam_address(address, 0U) ==
            state.ecam_base + PAGING_ECAM_WINDOW_SIZE -
                ACPI_ECAM_FUNCTION_SIZE;

    state.ecam_active = false;
    correct = correct && ecam_address(address, 0U) == 0U;

    state = saved;
    return correct;
}

static void self_test_write_capability(
    uint8_t offset,
    uint8_t identifier,
    uint8_t next
)
{
    self_test_space[offset] = identifier;
    self_test_space[offset + 1U] = next;
}

/*
 * Drive the capability walk over the synthetic space. read_aligned goes to the
 * ports, so the walk is re-implemented here against self_test_space rather than
 * the function being called directly: the arithmetic and the bounds are what is
 * under test, and hardware would make the result depend on the machine.
 */
static enum pci_status self_test_walk(size_t *count)
{
    bool visited[PCI_CONFIG_SPACE_SIZE / 4U] = {false};
    uint16_t offset = self_test_space[PCI_REGISTER_CAPABILITY_POINTER] &
        UINT8_C(0xFC);

    *count = 0U;

    while (offset != 0U) {
        if (offset < PCI_CAPABILITY_FIRST_OFFSET) {
            return PCI_STATUS_BAD_CAPABILITY_POINTER;
        }

        if (visited[offset / 4U]) {
            return PCI_STATUS_CAPABILITY_LOOP;
        }

        visited[offset / 4U] = true;

        if (*count >= PCI_MAX_CAPABILITIES) {
            return PCI_STATUS_CAPABILITY_LOOP;
        }

        ++*count;
        offset = self_test_space[offset + 1U] & UINT8_C(0xFC);
    }

    return PCI_STATUS_OK;
}

static bool capability_walk_is_bounded(void)
{
    size_t count = 0U;

    for (size_t index = 0; index < PCI_CONFIG_SPACE_SIZE; ++index) {
        self_test_space[index] = 0U;
    }

    /* Three capabilities in a chain that terminates properly. */
    self_test_space[PCI_REGISTER_CAPABILITY_POINTER] = UINT8_C(0x40);
    self_test_write_capability(
        UINT8_C(0x40),
        PCI_CAPABILITY_POWER_MANAGEMENT,
        UINT8_C(0x50)
    );
    self_test_write_capability(UINT8_C(0x50), PCI_CAPABILITY_MSI_X,
        UINT8_C(0x60));
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS, 0U);

    if (self_test_walk(&count) != PCI_STATUS_OK || count != 3U) {
        return false;
    }

    /* A pointer into the standard header, which is refused rather than read. */
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS,
        UINT8_C(0x20));

    if (self_test_walk(&count) != PCI_STATUS_BAD_CAPABILITY_POINTER) {
        return false;
    }

    /*
     * One below the first legal position, so the lower bound is not off by one.
     * There is deliberately no case for a pointer past the end: the static
     * assertion above proves a byte pointer cannot express one.
     */
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS,
        PCI_CAPABILITY_FIRST_OFFSET - 4U);

    if (self_test_walk(&count) != PCI_STATUS_BAD_CAPABILITY_POINTER) {
        return false;
    }

    /* A chain that points back at itself: the classic enumerator hang. */
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS,
        UINT8_C(0x40));

    if (self_test_walk(&count) != PCI_STATUS_CAPABILITY_LOOP) {
        return false;
    }

    /* A chain that ends by pointing at itself rather than at the head. */
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS,
        UINT8_C(0x60));

    if (self_test_walk(&count) != PCI_STATUS_CAPABILITY_LOOP) {
        return false;
    }

    /*
     * The last legal position, so the upper bound accepts what it should. A
     * bound written as `>=` rather than `>` would reject this.
     */
    self_test_write_capability(UINT8_C(0x60), PCI_CAPABILITY_EXPRESS,
        UINT8_C(0xFC));
    self_test_write_capability(UINT8_C(0xFC), PCI_CAPABILITY_VENDOR, 0U);

    return self_test_walk(&count) == PCI_STATUS_OK && count == 4U;
}

static bool field_extraction_is_right(void)
{
    const uint32_t dword = UINT32_C(0x44332211);

    if (byte_of(dword, 0U) != UINT8_C(0x11) ||
        byte_of(dword, 1U) != UINT8_C(0x22) ||
        byte_of(dword, 2U) != UINT8_C(0x33) ||
        byte_of(dword, 3U) != UINT8_C(0x44)) {
        return false;
    }

    /*
     * The offsets the readers actually pass are absolute, so the extraction has
     * to take the position inside the register from the low two bits rather
     * than from the offset itself.
     */
    if (byte_of(dword, PCI_REGISTER_CLASS) != UINT8_C(0x44) ||
        byte_of(dword, PCI_REGISTER_SUBCLASS) != UINT8_C(0x33) ||
        byte_of(dword, PCI_REGISTER_PROG_IF) != UINT8_C(0x22) ||
        byte_of(dword, PCI_REGISTER_REVISION) != UINT8_C(0x11)) {
        return false;
    }

    return word_of(dword, PCI_REGISTER_VENDOR_ID) == UINT16_C(0x2211) &&
        word_of(dword, PCI_REGISTER_DEVICE_ID) == UINT16_C(0x4433) &&
        word_of(dword, PCI_REGISTER_COMMAND) == UINT16_C(0x2211) &&
        word_of(dword, PCI_REGISTER_STATUS) == UINT16_C(0x4433);
}

static bool refusals_are_named(void)
{
    struct pci_address address;
    uint32_t value = 0U;

    address.segment = 0U;
    address.bus = 0U;
    address.device = 0U;
    address.function = 0U;

    if (pci_config_read_port(address, 0U, NULL) != PCI_STATUS_NULL_ARGUMENT ||
        pci_config_read_ecam(address, 0U, NULL) != PCI_STATUS_NULL_ARGUMENT) {
        return false;
    }

    address.device = PCI_DEVICES_PER_BUS;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        return false;
    }

    address.device = 0U;
    address.function = PCI_FUNCTIONS_PER_DEVICE;

    if (pci_config_read_port(address, 0U, &value) != PCI_STATUS_BAD_ADDRESS) {
        return false;
    }

    address.function = 0U;

    if (pci_config_read_port(address, 1U, &value) != PCI_STATUS_BAD_OFFSET ||
        pci_config_read_port(address, PCI_CONFIG_SPACE_SIZE, &value) !=
            PCI_STATUS_BAD_OFFSET) {
        return false;
    }

    /* The last legal offset, so the bound is not off by one in the safe
     * direction either. This one is expected to succeed. */
    if (pci_config_read_port(
            address,
            PCI_CONFIG_SPACE_SIZE - 4U,
            &value
        ) != PCI_STATUS_OK) {
        return false;
    }

    /* A segment group the ports cannot carry at all. */
    address.segment = 1U;

    return pci_config_read_port(address, 0U, &value) == PCI_STATUS_BAD_ADDRESS;
}

bool pci_self_test(void)
{
    return address_composition_is_right() &&
        ecam_arithmetic_is_right() &&
        capability_walk_is_bounded() &&
        field_extraction_is_right() &&
        refusals_are_named();
}
