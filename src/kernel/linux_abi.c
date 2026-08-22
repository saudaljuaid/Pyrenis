/* SPDX-License-Identifier: GPL-3.0-only */
/* One private static BusyBox process and its complete reverse-order teardown. */

#include <sapote/linux_abi.h>

#include <sapote/cpu.h>
#include <sapote/dma.h>
#include <sapote/filesystem.h>
#include <sapote/interrupt_vector.h>
#include <sapote/linux_elf64.h>
#include <sapote/linux_syscall.h>
#include <sapote/memory.h>
#include <sapote/msix.h>
#include <sapote/paging.h>
#include <sapote/pci_resource.h>

#define LINUX_ARGUMENT_BYTES 20U
#define LINUX_INITIAL_STACK_WORDS 10U
#define LINUX_EXECUTABLE_ALIAS_PAGES 6U
#define LINUX_INITIAL_USER_PAGES \
    (PAGING_LINUX_IMAGE_PAGES + PAGING_LINUX_STACK_PAGES)

_Static_assert(sizeof(struct linux_elf64_validated_image) == 248U,
    "Rust/C Linux ELF64 validated-image ABI changed");
_Static_assert(LINUX_ELF64_PARSER_ROBUSTNESS_CONTROLS +
    LINUX_FAT16_ROBUSTNESS_CONTROLS +
    LINUX_ABI_STACK_FOUNDATION_CONTROLS +
    LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS +
    LINUX_SYSCALL_SEMANTIC_CONTROLS +
    LINUX_ABI_LIFECYCLE_CONTROLS ==
        LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS,
    "Linux ABI robustness matrix cardinality changed");
_Static_assert(LINUX_ABI_IMAGE_STACK_FOUNDATION_CONTROLS ==
    LINUX_ELF64_PARSER_ROBUSTNESS_CONTROLS +
        LINUX_ABI_STACK_FOUNDATION_CONTROLS,
    "Linux image/stack foundation total changed");
_Static_assert(LINUX_INITIAL_USER_PAGES <=
    PAGING_PROCESS_EXPECTED_MAX_PAGES,
    "Linux initial mappings exceed paging audit capacity");

struct linux_resource_census {
    struct frame_allocator_stats frames;
    struct paging_state paging;
    struct dma_state dma;
    struct pci_resource_state pci;
    struct interrupt_vector_state vectors;
    struct msix_state msix;
    uint64_t cr3;
    bool filesystem_released;
    bool paging_process_released;
    bool syscall_released;
    bool user_boundary_inactive;
    bool interrupts_enabled;
};

struct linux_runtime {
    uint64_t generation;
    enum linux_process_state state;
    enum linux_executable_state executable_state;
    enum linux_stack_state stack_state;
    struct filesystem_linux_file file;
    uint8_t elf_bytes[LINUX_ELF64_FILE_BYTES];
    struct linux_elf64_validated_image image;
    uintptr_t image_frames[PAGING_LINUX_IMAGE_PAGES];
    uintptr_t stack_frames[PAGING_LINUX_STACK_PAGES];
    uintptr_t heap_frames[PAGING_LINUX_HEAP_PAGES];
    uintptr_t anonymous_frame;
    size_t image_frame_count;
    size_t stack_frame_count;
    size_t heap_frame_count;
    size_t image_mapped_count;
    size_t stack_mapped_count;
    struct paging_process_space address_space;
    struct paging_process_alias_set aliases;
    struct paging_process_expected_page
        expected_pages[LINUX_INITIAL_USER_PAGES];
    uint64_t initial_stack_pointer;
    struct linux_syscall_result syscall_result;
    bool interrupts_were_enabled;
    bool active;
};

static struct linux_runtime runtime;
static struct linux_abi_proof_result installed_result;
static uint64_t next_generation = UINT64_C(1);
static bool proof_active;

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool canonical_user(uint64_t address)
{
    return address <= UINT64_C(0x00007FFFFFFFFFFF);
}

static enum linux_abi_status transition_process(
    enum linux_process_state *state,
    enum linux_process_state next
)
{
    bool allowed = false;

    if (state == NULL || next >= LINUX_PROCESS_STATE_COUNT) {
        return LINUX_ABI_STATUS_TRANSITION_INVALID;
    }
    if (*state == next) {
        return LINUX_ABI_STATUS_TRANSITION_REPEATED;
    }
    switch (*state) {
    case LINUX_PROCESS_CANDIDATE:
        allowed = next == LINUX_PROCESS_BUILDING ||
            next == LINUX_PROCESS_STOPPING;
        break;
    case LINUX_PROCESS_BUILDING:
        allowed = next == LINUX_PROCESS_INSTALLED ||
            next == LINUX_PROCESS_STOPPING;
        break;
    case LINUX_PROCESS_INSTALLED:
        allowed = next == LINUX_PROCESS_RUNNING ||
            next == LINUX_PROCESS_STOPPING;
        break;
    case LINUX_PROCESS_RUNNING:
        allowed = next == LINUX_PROCESS_EXITING ||
            next == LINUX_PROCESS_STOPPING;
        break;
    case LINUX_PROCESS_EXITING:
        allowed = next == LINUX_PROCESS_STOPPING;
        break;
    case LINUX_PROCESS_STOPPING:
        allowed = next == LINUX_PROCESS_RELEASED;
        break;
    case LINUX_PROCESS_RELEASED:
    case LINUX_PROCESS_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return next < *state ? LINUX_ABI_STATUS_TRANSITION_REVERSED :
            LINUX_ABI_STATUS_TRANSITION_INVALID;
    }
    *state = next;
    return LINUX_ABI_STATUS_OK;
}

static void capture_census(struct linux_resource_census *census)
{
    census->frames = frame_allocator_get_stats();
    census->paging = paging_get_state();
    census->dma = dma_get_state();
    census->pci = pci_resource_get_state();
    census->vectors = interrupt_vector_get_state();
    census->msix = msix_get_state();
    census->cr3 = cpu_read_cr3();
    census->filesystem_released = filesystem_resources_released();
    census->paging_process_released = paging_process_resources_released();
    census->syscall_released = linux_syscall_resources_released();
    census->user_boundary_inactive = !linux_process_boundary_active();
    census->interrupts_enabled = cpu_interrupts_enabled();
}

static bool census_equal(
    const struct linux_resource_census *left,
    const struct linux_resource_census *right
)
{
    return left->frames.addressable_frames == right->frames.addressable_frames &&
        left->frames.allocatable_frames == right->frames.allocatable_frames &&
        left->frames.free_frames == right->frames.free_frames &&
        left->frames.allocated_frames == right->frames.allocated_frames &&
        left->frames.reserved_frames == right->frames.reserved_frames &&
        left->frames.highest_allocatable_address ==
            right->frames.highest_allocatable_address &&
        left->paging.root_physical_address ==
            right->paging.root_physical_address &&
        left->paging.table_frames == right->paging.table_frames &&
        left->paging.active == right->paging.active &&
        left->dma.active_allocations == right->dma.active_allocations &&
        left->dma.cpu_owned_allocations == right->dma.cpu_owned_allocations &&
        left->dma.device_owned_allocations ==
            right->dma.device_owned_allocations &&
        left->pci.active_claims == right->pci.active_claims &&
        left->pci.active_mappings == right->pci.active_mappings &&
        left->pci.mapped_pages == right->pci.mapped_pages &&
        left->pci.bus_masters == right->pci.bus_masters &&
        left->vectors.allocated == right->vectors.allocated &&
        left->vectors.free == right->vectors.free &&
        left->msix.active_bindings == right->msix.active_bindings &&
        left->msix.failure_injection_armed ==
            right->msix.failure_injection_armed &&
        left->cr3 == right->cr3 &&
        left->filesystem_released == right->filesystem_released &&
        left->paging_process_released == right->paging_process_released &&
        left->syscall_released == right->syscall_released &&
        left->user_boundary_inactive == right->user_boundary_inactive &&
        left->interrupts_enabled == right->interrupts_enabled;
}

static bool validated_placement(
    const struct linux_elf64_validated_image *image
)
{
    if (image == NULL || image->valid != 1U ||
        image->program_header_count != LINUX_ELF64_PROGRAM_HEADERS ||
        image->segment_count != LINUX_ELF64_LOAD_SEGMENTS ||
        image->non_load_count != 1U || image->entry != LINUX_ELF64_ENTRY) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_ELF64_LOAD_SEGMENTS; ++index) {
        const struct linux_elf64_segment *segment = &image->segments[index];

        if (segment->reserved != 0U || segment->file_size == 0U ||
            segment->memory_size < segment->file_size ||
            segment->mapping_start < PAGING_LINUX_IMAGE_BASE ||
            segment->mapping_end > PAGING_LINUX_IMAGE_END ||
            segment->mapping_start >= segment->mapping_end ||
            !canonical_user(segment->virtual_address) ||
            !canonical_user(segment->mapping_end - 1U)) {
            return false;
        }
    }
    return true;
}

static bool initialize_frame(uintptr_t frame)
{
    uint8_t *bytes = (uint8_t *)(void *)frame;

    if (frame == 0U) {
        return false;
    }
    for (size_t index = 0U; index < PAGING_PAGE_SIZE; ++index) {
        bytes[index] = 0U;
    }
    return true;
}

static bool image_write(
    uint64_t virtual_address,
    const uint8_t *source,
    size_t length
)
{
    uint64_t cursor = virtual_address;
    size_t remaining = length;

    if (source == NULL || length == 0U ||
        virtual_address < PAGING_LINUX_IMAGE_BASE ||
        virtual_address > PAGING_LINUX_IMAGE_END - length) {
        return false;
    }
    while (remaining > 0U) {
        const size_t page = (size_t)((cursor - PAGING_LINUX_IMAGE_BASE) /
            PAGING_PAGE_SIZE);
        const size_t page_offset = (size_t)(cursor &
            (PAGING_PAGE_SIZE - 1U));
        size_t chunk = (size_t)PAGING_PAGE_SIZE - page_offset;
        uint8_t *destination;

        if (page >= runtime.image_frame_count ||
            runtime.image_frames[page] == 0U) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        destination = (uint8_t *)(void *)runtime.image_frames[page] +
            page_offset;
        for (size_t index = 0U; index < chunk; ++index) {
            destination[index] = source[index];
        }
        source += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool install_image(void)
{
    for (size_t segment_index = 0U;
         segment_index < LINUX_ELF64_LOAD_SEGMENTS; ++segment_index) {
        const struct linux_elf64_segment *segment =
            &runtime.image.segments[segment_index];
        const size_t offset = (size_t)segment->file_offset;
        const size_t file_size = (size_t)segment->file_size;

        if (offset > sizeof(runtime.elf_bytes) ||
            file_size > sizeof(runtime.elf_bytes) - offset ||
            !image_write(segment->virtual_address,
                &runtime.elf_bytes[offset], file_size)) {
            return false;
        }
    }
    return true;
}

static bool stack_write(uint64_t address, const uint8_t *source, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (source == NULL || length == 0U || address < PAGING_LINUX_STACK_BASE ||
        address > PAGING_LINUX_STACK_END - length) {
        return false;
    }
    while (remaining > 0U) {
        const size_t page = (size_t)((cursor - PAGING_LINUX_STACK_BASE) /
            PAGING_PAGE_SIZE);
        const size_t offset = (size_t)(cursor & (PAGING_PAGE_SIZE - 1U));
        size_t chunk = (size_t)PAGING_PAGE_SIZE - offset;
        uint8_t *destination;

        if (page >= runtime.stack_frame_count ||
            runtime.stack_frames[page] == 0U) {
            return false;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        destination = (uint8_t *)(void *)runtime.stack_frames[page] + offset;
        for (size_t index = 0U; index < chunk; ++index) {
            destination[index] = source[index];
        }
        source += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool stack_write_u64(uint64_t address, uint64_t value)
{
    uint8_t bytes[sizeof(uint64_t)];

    for (size_t index = 0U; index < sizeof(bytes); ++index) {
        bytes[index] = (uint8_t)(value >> (index * 8U));
    }
    return stack_write(address, bytes, sizeof(bytes));
}

static bool build_initial_stack(void)
{
    static const uint8_t argv_zero[] = "busybox";
    static const uint8_t argv_one[] = "echo";
    static const uint8_t argv_two[] = "SAPOTE";
    const uint64_t argv_two_address = PAGING_LINUX_STACK_END -
        sizeof(argv_two);
    const uint64_t argv_one_address = argv_two_address - sizeof(argv_one);
    const uint64_t argv_zero_address = argv_one_address - sizeof(argv_zero);
    const uint64_t vector_address = (argv_zero_address & ~UINT64_C(15)) -
        LINUX_INITIAL_STACK_WORDS * sizeof(uint64_t);
    const uint64_t words[LINUX_INITIAL_STACK_WORDS] = {
        UINT64_C(3), argv_zero_address, argv_one_address, argv_two_address,
        UINT64_C(0), UINT64_C(0), UINT64_C(6), PAGING_PAGE_SIZE,
        UINT64_C(0), UINT64_C(0)
    };

    if ((vector_address & UINT64_C(15)) != 0U ||
        vector_address < PAGING_LINUX_STACK_BASE ||
        sizeof(argv_zero) + sizeof(argv_one) + sizeof(argv_two) !=
            LINUX_ARGUMENT_BYTES ||
        !stack_write(argv_zero_address, argv_zero, sizeof(argv_zero)) ||
        !stack_write(argv_one_address, argv_one, sizeof(argv_one)) ||
        !stack_write(argv_two_address, argv_two, sizeof(argv_two))) {
        return false;
    }
    for (size_t index = 0U; index < LINUX_INITIAL_STACK_WORDS; ++index) {
        if (!stack_write_u64(vector_address + index * sizeof(uint64_t),
                words[index])) {
            return false;
        }
    }
    runtime.initial_stack_pointer = vector_address;
    runtime.stack_state = LINUX_STACK_INSTALLED;
    return true;
}

static uint32_t page_permissions(size_t page)
{
    for (size_t segment_index = 0U;
         segment_index < LINUX_ELF64_LOAD_SEGMENTS; ++segment_index) {
        const struct linux_elf64_segment *segment =
            &runtime.image.segments[segment_index];
        const uint64_t address = PAGING_LINUX_IMAGE_BASE +
            page * PAGING_PAGE_SIZE;

        if (address >= segment->mapping_start &&
            address < segment->mapping_end) {
            if (segment->flags == UINT32_C(5)) {
                return PAGING_EXECUTE;
            }
            if (segment->flags == UINT32_C(6)) {
                return PAGING_WRITE;
            }
            return PAGING_READ;
        }
    }
    return UINT32_MAX;
}

static bool map_initial_pages(void)
{
    size_t expected = 0U;

    for (size_t page = 0U; page < PAGING_LINUX_IMAGE_PAGES; ++page) {
        const uint64_t address = PAGING_LINUX_IMAGE_BASE +
            page * PAGING_PAGE_SIZE;
        const uint32_t permissions = page_permissions(page);

        if (permissions == UINT32_MAX ||
            paging_process_map_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_IMAGE, address,
                runtime.image_frames[page], permissions) != PAGING_STATUS_OK) {
            return false;
        }
        ++runtime.image_mapped_count;
        runtime.expected_pages[expected].virtual_address = address;
        runtime.expected_pages[expected].physical_address =
            runtime.image_frames[page];
        runtime.expected_pages[expected].permissions = permissions;
        ++expected;
    }
    for (size_t page = 0U; page < PAGING_LINUX_STACK_PAGES; ++page) {
        const uint64_t address = PAGING_LINUX_STACK_BASE +
            page * PAGING_PAGE_SIZE;

        if (paging_process_map_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_STACK, address,
                runtime.stack_frames[page], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            return false;
        }
        ++runtime.stack_mapped_count;
        runtime.expected_pages[expected].virtual_address = address;
        runtime.expected_pages[expected].physical_address =
            runtime.stack_frames[page];
        runtime.expected_pages[expected].permissions = PAGING_WRITE;
        ++expected;
    }
    return expected == LINUX_INITIAL_USER_PAGES;
}

static bool exit_observed(uint64_t process_generation)
{
    return runtime.active && runtime.state == LINUX_PROCESS_RUNNING &&
        runtime.generation == process_generation &&
        runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
            runtime.address_space.root_physical_address &&
        transition_process(&runtime.state, LINUX_PROCESS_EXITING) ==
            LINUX_ABI_STATUS_OK;
}

static bool unmap_if_present(
    enum paging_process_mapping_kind kind,
    uint64_t address
)
{
    struct paging_translation translation;
    const enum paging_status status = paging_process_translate(
        &runtime.address_space, address, &translation);

    if (status == PAGING_STATUS_NOT_MAPPED) {
        return true;
    }
    return status == PAGING_STATUS_OK && translation.user &&
        paging_process_unmap_user_page(&runtime.address_space, kind, address) ==
            PAGING_STATUS_OK;
}

static enum linux_abi_status release_runtime(enum linux_abi_status result)
{
    bool cleanup_failed = false;

    cpu_interrupt_disable();
    if (runtime.address_space.state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state != LINUX_PROCESS_STOPPING &&
        runtime.state != LINUX_PROCESS_RELEASED &&
        transition_process(&runtime.state, LINUX_PROCESS_STOPPING) !=
            LINUX_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    if (!linux_syscall_resources_released() &&
        linux_syscall_disarm() != LINUX_SYSCALL_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        runtime.address_space.state != PAGING_PROCESS_SPACE_RELEASED) {
        if (!unmap_if_present(PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS)) {
            cleanup_failed = true;
        }
        for (size_t remaining = PAGING_LINUX_HEAP_PAGES;
             remaining > 0U; --remaining) {
            if (!unmap_if_present(PAGING_PROCESS_MAPPING_LINUX_HEAP,
                    PAGING_LINUX_HEAP_BASE +
                        (remaining - 1U) * PAGING_PAGE_SIZE)) {
                cleanup_failed = true;
            }
        }
    }
    for (size_t mapped = runtime.stack_mapped_count; mapped > 0U; --mapped) {
        const size_t page = mapped - 1U;

        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_STACK,
                PAGING_LINUX_STACK_BASE + page * PAGING_PAGE_SIZE) !=
                PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            --runtime.stack_mapped_count;
        }
    }
    for (size_t mapped = runtime.image_mapped_count; mapped > 0U; --mapped) {
        const size_t page = mapped - 1U;

        if (paging_process_unmap_user_page(&runtime.address_space,
                PAGING_PROCESS_MAPPING_LINUX_IMAGE,
                PAGING_LINUX_IMAGE_BASE + page * PAGING_PAGE_SIZE) !=
                PAGING_STATUS_OK) {
            cleanup_failed = true;
        } else {
            --runtime.image_mapped_count;
        }
    }
    if (runtime.aliases.active &&
        paging_process_alias_set_restore(&runtime.address_space,
            &runtime.aliases) != PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.address_space.state != PAGING_PROCESS_SPACE_INVALID &&
        runtime.address_space.state != PAGING_PROCESS_SPACE_RELEASED &&
        paging_process_space_release(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.anonymous_frame != 0U) {
        if (frame_release(runtime.anonymous_frame) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.anonymous_frame = 0U;
        }
    }
    for (size_t count = runtime.heap_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.heap_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.heap_frames[page] = 0U;
            --runtime.heap_frame_count;
        }
    }
    for (size_t count = runtime.stack_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.stack_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.stack_frames[page] = 0U;
            --runtime.stack_frame_count;
        }
    }
    for (size_t count = runtime.image_frame_count; count > 0U; --count) {
        const size_t page = count - 1U;

        if (frame_release(runtime.image_frames[page]) != FRAME_STATUS_OK) {
            cleanup_failed = true;
        } else {
            runtime.image_frames[page] = 0U;
            --runtime.image_frame_count;
        }
    }
    zero_bytes(runtime.elf_bytes, sizeof(runtime.elf_bytes));
    runtime.executable_state = LINUX_EXECUTABLE_RELEASED;
    runtime.stack_state = LINUX_STACK_RELEASED;
    if (runtime.file.active &&
        filesystem_linux_read_close(&runtime.file) != FILESYSTEM_STATUS_OK) {
        cleanup_failed = true;
    }
    if (runtime.state == LINUX_PROCESS_STOPPING &&
        transition_process(&runtime.state, LINUX_PROCESS_RELEASED) !=
            LINUX_ABI_STATUS_OK) {
        cleanup_failed = true;
    }
    runtime.active = false;
    proof_active = false;
    if (runtime.interrupts_were_enabled) {
        cpu_interrupt_enable();
    }
    return cleanup_failed ? LINUX_ABI_STATUS_TEARDOWN : result;
}

bool linux_abi_image_stack_foundation_self_test(size_t *completed_tests)
{
    enum linux_process_state state = LINUX_PROCESS_CANDIDATE;

    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (sapote_linux_elf64_self_test() !=
            LINUX_ELF64_PARSER_ROBUSTNESS_CONTROLS ||
        (PAGING_LINUX_IMAGE_BASE & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (PAGING_LINUX_STACK_GUARD & (PAGING_PAGE_SIZE - 1U)) != 0U ||
        (PAGING_LINUX_STACK_END & UINT64_C(15)) != 0U ||
        !canonical_user(PAGING_LINUX_IMAGE_BASE) ||
        !canonical_user(PAGING_LINUX_STACK_END - 1U) ||
        PAGING_LINUX_IMAGE_END > PAGING_LINUX_HEAP_BASE ||
        PAGING_LINUX_HEAP_BASE +
            PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE >
                PAGING_LINUX_ANON_ADDRESS ||
        PAGING_LINUX_ANON_ADDRESS + PAGING_PAGE_SIZE >
            PAGING_LINUX_STACK_GUARD ||
        LINUX_ARGUMENT_BYTES != 20U || LINUX_INITIAL_STACK_WORDS != 10U ||
        !linux_syscall_enosys_self_test() ||
        transition_process(&state, LINUX_PROCESS_BUILDING) !=
            LINUX_ABI_STATUS_OK ||
        transition_process(&state, LINUX_PROCESS_INSTALLED) !=
            LINUX_ABI_STATUS_OK ||
        transition_process(&state, LINUX_PROCESS_RUNNING) !=
            LINUX_ABI_STATUS_OK ||
        transition_process(&state, LINUX_PROCESS_EXITING) !=
            LINUX_ABI_STATUS_OK ||
        transition_process(&state, LINUX_PROCESS_STOPPING) !=
            LINUX_ABI_STATUS_OK ||
        transition_process(&state, LINUX_PROCESS_RELEASED) !=
            LINUX_ABI_STATUS_OK) {
        return false;
    }
    state = LINUX_PROCESS_BUILDING;
    if (transition_process(&state, LINUX_PROCESS_BUILDING) !=
            LINUX_ABI_STATUS_TRANSITION_REPEATED ||
        transition_process(&state, LINUX_PROCESS_CANDIDATE) !=
            LINUX_ABI_STATUS_TRANSITION_REVERSED ||
        !linux_abi_resources_released()) {
        return false;
    }
    *completed_tests = LINUX_ABI_IMAGE_STACK_FOUNDATION_CONTROLS;
    return true;
}

static enum linux_abi_status allocate_runtime_frames(void)
{
    for (size_t page = 0U; page < PAGING_LINUX_IMAGE_PAGES; ++page) {
        if (frame_allocate(&runtime.image_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.image_frames[page])) {
            return LINUX_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.image_frame_count;
    }
    for (size_t page = 0U; page < PAGING_LINUX_STACK_PAGES; ++page) {
        if (frame_allocate(&runtime.stack_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.stack_frames[page])) {
            return LINUX_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.stack_frame_count;
    }
    for (size_t page = 0U; page < PAGING_LINUX_HEAP_PAGES; ++page) {
        if (frame_allocate(&runtime.heap_frames[page]) != FRAME_STATUS_OK ||
            !initialize_frame(runtime.heap_frames[page])) {
            return LINUX_ABI_STATUS_FRAME_ALLOCATION;
        }
        ++runtime.heap_frame_count;
    }
    if (frame_allocate(&runtime.anonymous_frame) != FRAME_STATUS_OK ||
        !initialize_frame(runtime.anonymous_frame)) {
        return LINUX_ABI_STATUS_FRAME_ALLOCATION;
    }
    return LINUX_ABI_STATUS_OK;
}

static enum linux_abi_status linux_attempt(
    struct linux_abi_proof_result *result
)
{
    struct linux_resource_census before;
    struct linux_resource_census after;
    struct linux_syscall_context syscall_context;
    uint64_t executable_aliases[LINUX_EXECUTABLE_ALIAS_PAGES];
    uint32_t file_bytes = 0U;
    uint32_t file_clusters = 0U;
    enum linux_abi_status status = LINUX_ABI_STATUS_OK;
    enum filesystem_status filesystem_status;

    if (result == NULL) {
        return LINUX_ABI_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(result, sizeof(*result));
    if (proof_active || !linux_abi_resources_released()) {
        return LINUX_ABI_STATUS_BUSY;
    }
    capture_census(&before);
    zero_bytes(&runtime, sizeof(runtime));
    runtime.state = LINUX_PROCESS_CANDIDATE;
    runtime.executable_state = LINUX_EXECUTABLE_CANDIDATE;
    runtime.stack_state = LINUX_STACK_CANDIDATE;
    runtime.generation = next_generation++;
    if (next_generation == 0U) {
        next_generation = 1U;
    }
    runtime.interrupts_were_enabled = before.interrupts_enabled;
    runtime.active = true;
    proof_active = true;
    if (transition_process(&runtime.state, LINUX_PROCESS_BUILDING) !=
            LINUX_ABI_STATUS_OK) {
        status = LINUX_ABI_STATUS_TRANSITION_INVALID;
        goto cleanup;
    }
    filesystem_status = filesystem_linux_read_open(&runtime.file,
        runtime.elf_bytes, sizeof(runtime.elf_bytes));
    if (filesystem_status == FILESYSTEM_STATUS_ABSENT) {
        status = LINUX_ABI_STATUS_ABSENT;
        goto cleanup;
    }
    if (filesystem_status != FILESYSTEM_STATUS_OK ||
        !runtime.file.cpu_owned ||
        runtime.file.file_bytes != LINUX_ELF64_FILE_BYTES ||
        runtime.file.cluster_count != LINUX_FAT16_FILE_CLUSTERS ||
        runtime.file.read_count != 3U + LINUX_FAT16_FILE_CLUSTERS ||
        runtime.file.msix_completion_count !=
            3U + LINUX_FAT16_FILE_CLUSTERS) {
        status = LINUX_ABI_STATUS_FILESYSTEM;
        goto cleanup;
    }
    file_bytes = runtime.file.file_bytes;
    file_clusters = runtime.file.cluster_count;
    if (sapote_linux_elf64_parse(runtime.elf_bytes,
            sizeof(runtime.elf_bytes), &runtime.image) !=
            LINUX_ELF64_STATUS_OK || !validated_placement(&runtime.image)) {
        status = LINUX_ABI_STATUS_ELF;
        goto cleanup;
    }
    runtime.executable_state = LINUX_EXECUTABLE_VALIDATED;
    status = allocate_runtime_frames();
    if (status != LINUX_ABI_STATUS_OK) {
        goto cleanup;
    }
    if (!install_image()) {
        status = LINUX_ABI_STATUS_ELF_INSTALL;
        goto cleanup;
    }
    runtime.executable_state = LINUX_EXECUTABLE_INSTALLED;
    runtime.stack_state = LINUX_STACK_BUILDING;
    if (!build_initial_stack()) {
        status = LINUX_ABI_STATUS_STACK;
        goto cleanup;
    }
    if (paging_process_space_build(&runtime.address_space) !=
            PAGING_STATUS_OK) {
        status = LINUX_ABI_STATUS_ADDRESS_SPACE;
        goto cleanup;
    }
    cpu_interrupt_disable();
    for (size_t page = 0U; page < LINUX_EXECUTABLE_ALIAS_PAGES; ++page) {
        executable_aliases[page] = runtime.image_frames[page + 1U];
    }
    if (paging_process_alias_set_narrow(&runtime.address_space,
            executable_aliases, LINUX_EXECUTABLE_ALIAS_PAGES,
            &runtime.aliases) != PAGING_STATUS_OK) {
        status = LINUX_ABI_STATUS_ALIAS;
        goto cleanup;
    }
    if (!map_initial_pages()) {
        status = LINUX_ABI_STATUS_MAPPING;
        goto cleanup;
    }
    if (paging_process_validate_linux(&runtime.address_space,
            runtime.expected_pages, LINUX_INITIAL_USER_PAGES) !=
            PAGING_STATUS_OK) {
        status = LINUX_ABI_STATUS_PERMISSION_AUDIT;
        goto cleanup;
    }
    zero_bytes(&syscall_context, sizeof(syscall_context));
    syscall_context.address_space = &runtime.address_space;
    syscall_context.process_generation = runtime.generation;
    syscall_context.executable_start = UINT64_C(0x0000400001001000);
    syscall_context.executable_end = UINT64_C(0x0000400001007000);
    syscall_context.stack_start = PAGING_LINUX_STACK_BASE;
    syscall_context.stack_end = PAGING_LINUX_STACK_END;
    syscall_context.fs_address = UINT64_C(0x0000400001008998);
    syscall_context.tid_address = UINT64_C(0x0000400001008B34);
    for (size_t page = 0U; page < PAGING_LINUX_HEAP_PAGES; ++page) {
        syscall_context.heap_frames[page] = runtime.heap_frames[page];
    }
    syscall_context.anonymous_frame = runtime.anonymous_frame;
    syscall_context.exit_observed = exit_observed;
    if (linux_syscall_arm(&syscall_context) != LINUX_SYSCALL_STATUS_OK ||
        linux_syscall_validate_armed() != LINUX_SYSCALL_STATUS_OK) {
        status = LINUX_ABI_STATUS_SYSCALL_CPU;
        goto cleanup;
    }
    if (transition_process(&runtime.state, LINUX_PROCESS_INSTALLED) !=
            LINUX_ABI_STATUS_OK ||
        paging_process_activate(&runtime.address_space) != PAGING_STATUS_OK ||
        transition_process(&runtime.state, LINUX_PROCESS_RUNNING) !=
            LINUX_ABI_STATUS_OK) {
        status = LINUX_ABI_STATUS_ENTRY;
        goto cleanup;
    }
    linux_process_enter_user(runtime.image.entry,
        runtime.initial_stack_pointer);
    runtime.syscall_result = linux_syscall_get_result();
    if (linux_process_boundary_active() ||
        runtime.address_space.state != PAGING_PROCESS_SPACE_INSTALLED ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            before.paging.root_physical_address ||
        runtime.state != LINUX_PROCESS_EXITING ||
        runtime.syscall_result.status != LINUX_SYSCALL_STATUS_OK ||
        runtime.syscall_result.cpu_state != LINUX_SYSCALL_CPU_RETURNED ||
        runtime.syscall_result.syscall_count != LINUX_SYSCALL_EXPECTED_CALLS ||
        runtime.syscall_result.distinct_syscalls !=
            LINUX_SYSCALL_ALLOWLIST_COUNT ||
        runtime.syscall_result.stdout_bytes != LINUX_SYSCALL_STDOUT_BYTES ||
        !runtime.syscall_result.stdout_valid ||
        !runtime.syscall_result.exit_zero ||
        !runtime.syscall_result.real_syscall_instruction ||
        !runtime.syscall_result.process_authenticated ||
        !runtime.syscall_result.cr3_authenticated) {
        status = LINUX_ABI_STATUS_EXIT;
    }

cleanup:
    status = release_runtime(status);
    if (status == LINUX_ABI_STATUS_TEARDOWN ||
        !linux_abi_resources_released() || paging_verify() != PAGING_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return LINUX_ABI_STATUS_TEARDOWN;
    }
    capture_census(&after);
    if (!census_equal(&before, &after)) {
        zero_bytes(result, sizeof(*result));
        return LINUX_ABI_STATUS_RESOURCE_CENSUS;
    }
    if (status != LINUX_ABI_STATUS_OK) {
        zero_bytes(result, sizeof(*result));
        return status;
    }
    runtime.syscall_result = linux_syscall_get_result();
    if (!runtime.syscall_result.cpu_disarmed) {
        return LINUX_ABI_STATUS_TEARDOWN;
    }
    result->file_bytes = file_bytes;
    result->program_headers = LINUX_ELF64_PROGRAM_HEADERS;
    result->load_segments = LINUX_ELF64_LOAD_SEGMENTS;
    result->file_clusters = file_clusters;
    result->stdout_bytes = runtime.syscall_result.stdout_bytes;
    result->syscall_count = runtime.syscall_result.syscall_count;
    result->distinct_syscalls = runtime.syscall_result.distinct_syscalls;
    result->exit_status = runtime.syscall_result.exit_status;
    result->robustness_tests = LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS;
    result->ring_three = true;
    result->private_address_space = true;
    result->real_syscall_instruction =
        runtime.syscall_result.real_syscall_instruction;
    result->stdout_valid = runtime.syscall_result.stdout_valid;
    result->exit_zero = runtime.syscall_result.exit_zero;
    result->unknown_enosys = linux_syscall_enosys_self_test();
    result->write_xor_execute = true;
    result->kernel_cr3_restored = after.cr3 == before.cr3;
    result->teardown_complete = true;
    result->resource_census_equal = true;
    installed_result = *result;
    return LINUX_ABI_STATUS_OK;
}

enum linux_abi_status linux_abi_installed_prove(
    struct linux_abi_proof_result *result
)
{
    if (result == NULL) {
        return LINUX_ABI_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(&installed_result, sizeof(installed_result));
    return linux_attempt(result);
}

struct linux_abi_proof_result linux_abi_get_proof_result(void)
{
    return installed_result;
}

bool linux_abi_resources_released(void)
{
    const struct paging_state paging = paging_get_state();

    return !proof_active && !runtime.active &&
        paging_process_resources_released() &&
        filesystem_resources_released() && linux_syscall_resources_released() &&
        !linux_process_boundary_active() &&
        (!paging.active ||
            (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) ==
                paging.root_physical_address);
}

const char *linux_abi_status_string(enum linux_abi_status status)
{
    static const char *const messages[LINUX_ABI_STATUS_COUNT] = {
        "ok",
        "Linux ABI BusyBox fixture is absent",
        "null Linux ABI argument",
        "Linux ABI proof is already active",
        "Linux ABI prerequisite is incomplete",
        "BusyBox filesystem read failed",
        "Rust BusyBox ELF parser refused the image",
        "BusyBox ELF installation failed",
        "Linux initial-stack construction failed",
        "Linux private-frame allocation failed",
        "Linux private address-space construction failed",
        "Linux executable alias narrowing failed",
        "Linux user mapping installation failed",
        "Linux effective-permission audit failed",
        "Linux SYSCALL CPU contract failed",
        "BusyBox Ring 3 entry failed",
        "BusyBox exit was not authenticated",
        "Linux process transition repeated",
        "Linux process transition reversed",
        "Linux process transition invalid",
        "Linux process teardown failed",
        "Linux process resource census differs",
        "Linux ABI controlled robustness failed"
    };

    if (status >= LINUX_ABI_STATUS_COUNT) {
        return "unknown Linux ABI status";
    }
    return messages[status];
}
