/* SPDX-License-Identifier: GPL-3.0-only */
/* One measured Linux x86-64 SYSCALL subset for the BusyBox echo proof. */

#include <sapote/linux_syscall.h>

#include <stddef.h>

#include <sapote/console.h>
#include <sapote/cpu.h>
#include <sapote/memory.h>

#define CPUID_EXTENDED_ROOT UINT32_C(0x80000000)
#define CPUID_EXTENDED_FEATURES UINT32_C(0x80000001)
#define CPUID_SYSCALL_SYSRET (UINT32_C(1) << 11U)
#define IA32_EFER UINT32_C(0xC0000080)
#define IA32_STAR UINT32_C(0xC0000081)
#define IA32_LSTAR UINT32_C(0xC0000082)
#define IA32_FMASK UINT32_C(0xC0000084)
#define IA32_FS_BASE UINT32_C(0xC0000100)
#define EFER_SCE UINT64_C(1)
#define ARCH_SET_FS UINT64_C(0x1002)
#define LINUX_STAR_VALUE \
    ((UINT64_C(0x23) << 48U) | (UINT64_C(0x08) << 32U))
#define LINUX_FMASK_VALUE \
    ((UINT64_C(1) << 8U) | (UINT64_C(1) << 9U) | \
        (UINT64_C(1) << 10U) | (UINT64_C(1) << 14U) | \
        (UINT64_C(1) << 18U))
#define LINUX_USER_RFLAGS_ALLOWED UINT64_C(0x8D7)
#define LINUX_ERRNO_EINVAL 22
#define LINUX_ERRNO_ENOSYS 38
#define LINUX_KERNEL_STACK_BYTES (16U * 1024U)

_Static_assert(sizeof(struct linux_syscall_frame) == 144U,
    "Linux syscall assembly frame size changed");
_Static_assert(offsetof(struct linux_syscall_frame, rax) == 0U &&
    offsetof(struct linux_syscall_frame, rdi) == 8U &&
    offsetof(struct linux_syscall_frame, r10) == 32U &&
    offsetof(struct linux_syscall_frame, r9) == 48U &&
    offsetof(struct linux_syscall_frame, rip) == 104U &&
    offsetof(struct linux_syscall_frame, rflags) == 120U &&
    offsetof(struct linux_syscall_frame, rsp) == 128U &&
    offsetof(struct linux_syscall_frame, ss) == 136U,
    "Linux syscall assembly frame offsets changed");
_Static_assert(LINUX_SYSCALL_ALLOWLIST_COUNT <= LINUX_SYSCALL_ALLOWLIST_MAX,
    "Linux syscall allowlist exceeds the fixed ceiling");

enum stdout_sink_state {
    STDOUT_SINK_CANDIDATE = 0,
    STDOUT_SINK_ARMED,
    STDOUT_SINK_WRITTEN,
    STDOUT_SINK_RELEASED
};

enum provenance_state {
    PROVENANCE_CANDIDATE = 0,
    PROVENANCE_ENTERED,
    PROVENANCE_COMPLETED,
    PROVENANCE_RELEASED
};

struct linux_syscall_runtime {
    struct linux_syscall_context context;
    struct linux_syscall_result result;
    uint64_t saved_efer;
    uint64_t saved_star;
    uint64_t saved_lstar;
    uint64_t saved_fmask;
    uint64_t saved_fs_base;
    uint64_t request_generation;
    uint32_t call_index;
    uint32_t request_ordinal;
    enum linux_syscall_cpu_state state;
    enum stdout_sink_state stdout_state;
    enum provenance_state provenance_state;
    bool seen[LINUX_SYSCALL_ALLOWLIST_COUNT];
    bool heap_mapped[PAGING_LINUX_HEAP_PAGES];
    bool anonymous_mapped;
    bool active;
};

static const uint64_t allowlist[LINUX_SYSCALL_ALLOWLIST_COUNT] = {
    UINT64_C(1), UINT64_C(9), UINT64_C(11), UINT64_C(12),
    UINT64_C(158), UINT64_C(218), UINT64_C(231)
};

static const uint64_t expected_calls[LINUX_SYSCALL_EXPECTED_CALLS] = {
    UINT64_C(158), UINT64_C(218), UINT64_C(12), UINT64_C(12),
    UINT64_C(9), UINT64_C(9), UINT64_C(1), UINT64_C(11), UINT64_C(231)
};

static const uint8_t expected_stdout[LINUX_SYSCALL_STDOUT_BYTES] = {
    'S', 'A', 'P', 'O', 'T', 'E', '\n'
};

static struct linux_syscall_runtime runtime;

/* Loaded by the second instruction in linux_syscall_entry. */
uint64_t linux_syscall_kernel_stack;

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

static bool syscall_supported(void)
{
    struct cpuid_result root;
    struct cpuid_result features;

    cpu_cpuid(CPUID_EXTENDED_ROOT, 0U, &root);
    if (root.eax < CPUID_EXTENDED_FEATURES) {
        return false;
    }
    cpu_cpuid(CPUID_EXTENDED_FEATURES, 0U, &features);
    return (features.edx & CPUID_SYSCALL_SYSRET) != 0U;
}

static enum linux_syscall_status transition_cpu(
    enum linux_syscall_cpu_state next
)
{
    bool allowed = false;

    if (next >= LINUX_SYSCALL_CPU_STATE_COUNT || runtime.state == next) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    switch (runtime.state) {
    case LINUX_SYSCALL_CPU_CANDIDATE:
        allowed = next == LINUX_SYSCALL_CPU_ARMED;
        break;
    case LINUX_SYSCALL_CPU_ARMED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_ENTERED:
        allowed = next == LINUX_SYSCALL_CPU_RETURNED;
        break;
    case LINUX_SYSCALL_CPU_RETURNED:
        allowed = next == LINUX_SYSCALL_CPU_ENTERED ||
            next == LINUX_SYSCALL_CPU_DISARMED;
        break;
    case LINUX_SYSCALL_CPU_DISARMED:
    case LINUX_SYSCALL_CPU_STATE_COUNT:
        break;
    }
    if (!allowed) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.state = next;
    runtime.result.cpu_state = next;
    return LINUX_SYSCALL_STATUS_OK;
}

static size_t allowlist_index(uint64_t number)
{
    for (size_t index = 0U; index < LINUX_SYSCALL_ALLOWLIST_COUNT; ++index) {
        if (allowlist[index] == number) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool msr_contract_valid(void)
{
    return (cpu_read_msr(IA32_EFER) & EFER_SCE) != 0U &&
        cpu_read_msr(IA32_STAR) == LINUX_STAR_VALUE &&
        cpu_read_msr(IA32_LSTAR) ==
            (uint64_t)(uintptr_t)linux_syscall_entry &&
        cpu_read_msr(IA32_FMASK) == LINUX_FMASK_VALUE;
}

static bool user_writable(uint64_t address, size_t length)
{
    uint64_t cursor = address;
    size_t remaining = length;

    if (length == 0U || !canonical_user(address) ||
        address > UINT64_MAX - (uint64_t)(length - 1U) ||
        !canonical_user(address + (uint64_t)(length - 1U))) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            (translation.permissions & PAGING_WRITE) == 0U ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool copy_from_user(void *destination, uint64_t address, size_t length)
{
    uint8_t *output = destination;
    uint64_t cursor = address;
    size_t remaining = length;

    if (destination == NULL || length == 0U || !canonical_user(address) ||
        address > UINT64_MAX - (uint64_t)(length - 1U) ||
        !canonical_user(address + (uint64_t)(length - 1U))) {
        return false;
    }
    while (remaining > 0U) {
        struct paging_translation translation;
        size_t chunk = (size_t)(PAGING_PAGE_SIZE -
            (cursor & (PAGING_PAGE_SIZE - 1U)));

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (paging_process_translate(runtime.context.address_space, cursor,
                &translation) != PAGING_STATUS_OK || !translation.user ||
            translation.level != 1U ||
            !frame_range_overlaps_allocatable_memory(
                translation.physical_address, chunk)) {
            return false;
        }
        const uint8_t *input =
            (const uint8_t *)(uintptr_t)translation.physical_address;

        for (size_t index = 0U; index < chunk; ++index) {
            output[index] = input[index];
        }
        output += chunk;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

static enum linux_syscall_status validate_entry(
    const struct linux_syscall_frame *frame
)
{
    const uintptr_t stack_pointer = cpu_read_stack_pointer();
    uint8_t instruction[2];

    if (frame == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (!runtime.active || runtime.context.address_space == NULL ||
        runtime.provenance_state != PROVENANCE_CANDIDATE ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.process_generation == 0U ||
        runtime.request_generation != runtime.context.process_generation) {
        return LINUX_SYSCALL_STATUS_BAD_GENERATION;
    }
    if (runtime.context.address_space->state != PAGING_PROCESS_SPACE_ACTIVE ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            runtime.context.address_space->root_physical_address) {
        return LINUX_SYSCALL_STATUS_BAD_CR3;
    }
    if (cpu_read_cs() != CPU_GDT_CODE_SELECTOR ||
        linux_syscall_kernel_stack == 0U ||
        stack_pointer > linux_syscall_kernel_stack ||
        stack_pointer < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES ||
        (uintptr_t)frame > linux_syscall_kernel_stack ||
        (uintptr_t)frame < linux_syscall_kernel_stack -
            LINUX_KERNEL_STACK_BYTES) {
        return LINUX_SYSCALL_STATUS_BAD_STACK;
    }
    if (frame->cs != CPU_GDT_USER_CODE_SELECTOR ||
        frame->ss != CPU_GDT_USER_DATA_SELECTOR ||
        (frame->rflags & UINT64_C(2)) == 0U ||
        (frame->rflags & ~LINUX_USER_RFLAGS_ALLOWED) != 0U ||
        !canonical_user(frame->rip) ||
        frame->rip < runtime.context.executable_start + 2U ||
        frame->rip >= runtime.context.executable_end ||
        !canonical_user(frame->rsp) ||
        frame->rsp < runtime.context.stack_start ||
        frame->rsp >= runtime.context.stack_end) {
        return LINUX_SYSCALL_STATUS_BAD_RETURN;
    }
    if (!copy_from_user(instruction, frame->rip - 2U,
            sizeof(instruction)) || instruction[0] != UINT8_C(0x0F) ||
        instruction[1] != UINT8_C(0x05)) {
        return LINUX_SYSCALL_STATUS_BAD_ENTRY;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static uintptr_t return_to_kernel(enum linux_syscall_status status)
{
    uintptr_t resume_stack;

    runtime.result.status = status;
    if (runtime.state == LINUX_SYSCALL_CPU_ENTERED) {
        (void)transition_cpu(LINUX_SYSCALL_CPU_RETURNED);
    }
    if (runtime.context.address_space != NULL &&
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE &&
        paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    resume_stack = linux_process_resume_stack();
    return resume_stack;
}

bool linux_syscall_cpu_foundation_self_test(size_t *completed_tests)
{
    if (completed_tests == NULL) {
        return false;
    }
    *completed_tests = 0U;
    if (!syscall_supported() || !cpu_user_transition_contract_valid() ||
        cpu_tss_rsp0() == 0U ||
        CPU_GDT_CODE_SELECTOR != UINT16_C(0x08) ||
        CPU_GDT_DATA_SELECTOR != UINT16_C(0x10) ||
        CPU_GDT_USER_DATA_SELECTOR != UINT16_C(0x2B) ||
        CPU_GDT_USER_CODE_SELECTOR != UINT16_C(0x33) ||
        LINUX_STAR_VALUE != ((UINT64_C(0x23) << 48U) |
            (UINT64_C(0x08) << 32U)) ||
        LINUX_FMASK_VALUE != UINT64_C(0x44700) ||
        LINUX_SYSCALL_ALLOWLIST_COUNT > LINUX_SYSCALL_ALLOWLIST_MAX ||
        sizeof(struct linux_syscall_frame) != 144U) {
        return false;
    }
    *completed_tests = LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS;
    return true;
}

bool linux_syscall_enosys_self_test(void)
{
    static const uint64_t refused[] = {
        UINT64_C(0), UINT64_C(2), UINT64_C(60), UINT64_C(999), UINT64_MAX
    };

    for (size_t index = 0U; index < sizeof(refused) / sizeof(refused[0]);
         ++index) {
        if (allowlist_index(refused[index]) != SIZE_MAX) {
            return false;
        }
    }
    return (int64_t)(uint64_t)(int64_t)-LINUX_ERRNO_ENOSYS ==
        -LINUX_ERRNO_ENOSYS;
}

enum linux_syscall_status linux_syscall_arm(
    const struct linux_syscall_context *context
)
{
    const uint64_t kernel_stack = (uint64_t)cpu_tss_rsp0();

    if (context == NULL || context->address_space == NULL) {
        return LINUX_SYSCALL_STATUS_NULL_ARGUMENT;
    }
    if (runtime.active || linux_process_boundary_active()) {
        return LINUX_SYSCALL_STATUS_BUSY;
    }
    if (!syscall_supported()) {
        return LINUX_SYSCALL_STATUS_UNSUPPORTED_CPU;
    }
    if (!cpu_user_transition_contract_valid() || kernel_stack == 0U ||
        cpu_interrupts_enabled() ||
        context->address_space->state != PAGING_PROCESS_SPACE_INSTALLED ||
        context->process_generation == 0U ||
        context->executable_start != UINT64_C(0x0000400001001000) ||
        context->executable_end != UINT64_C(0x0000400001007000) ||
        context->stack_start != PAGING_LINUX_STACK_BASE ||
        context->stack_end != PAGING_LINUX_STACK_END ||
        context->fs_address != UINT64_C(0x0000400001008998) ||
        context->tid_address != UINT64_C(0x0000400001008B34)) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (context->heap_frames[index] == 0U) {
            return LINUX_SYSCALL_STATUS_BAD_PROCESS;
        }
    }
    if (context->anonymous_frame == 0U || context->exit_observed == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_PROCESS;
    }
    zero_bytes(&runtime, sizeof(runtime));
    runtime.context = *context;
    runtime.state = LINUX_SYSCALL_CPU_CANDIDATE;
    runtime.result.cpu_state = runtime.state;
    runtime.stdout_state = STDOUT_SINK_ARMED;
    runtime.provenance_state = PROVENANCE_CANDIDATE;
    runtime.request_generation = context->process_generation;
    runtime.saved_efer = cpu_read_msr(IA32_EFER);
    runtime.saved_star = cpu_read_msr(IA32_STAR);
    runtime.saved_lstar = cpu_read_msr(IA32_LSTAR);
    runtime.saved_fmask = cpu_read_msr(IA32_FMASK);
    runtime.saved_fs_base = cpu_read_msr(IA32_FS_BASE);
    linux_syscall_kernel_stack = kernel_stack;
    cpu_write_msr(IA32_STAR, LINUX_STAR_VALUE);
    cpu_write_msr(IA32_LSTAR, (uint64_t)(uintptr_t)linux_syscall_entry);
    cpu_write_msr(IA32_FMASK, LINUX_FMASK_VALUE);
    cpu_write_msr(IA32_EFER, runtime.saved_efer | EFER_SCE);
    runtime.active = true;
    if (transition_cpu(LINUX_SYSCALL_CPU_ARMED) != LINUX_SYSCALL_STATUS_OK ||
        !msr_contract_valid()) {
        (void)linux_syscall_disarm();
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    runtime.result.status = LINUX_SYSCALL_STATUS_OK;
    return LINUX_SYSCALL_STATUS_OK;
}

enum linux_syscall_status linux_syscall_validate_armed(void)
{
    if (!runtime.active || runtime.state != LINUX_SYSCALL_CPU_ARMED ||
        runtime.context.address_space == NULL) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (!msr_contract_valid() || linux_syscall_kernel_stack != cpu_tss_rsp0()) {
        return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status map_heap(void)
{
    for (size_t index = 0U; index < PAGING_LINUX_HEAP_PAGES; ++index) {
        if (paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE + index * PAGING_PAGE_SIZE,
                runtime.context.heap_frames[index], PAGING_WRITE) !=
                PAGING_STATUS_OK) {
            for (size_t rollback = 0U; rollback < index; ++rollback) {
                (void)paging_process_unmap_user_page(
                    runtime.context.address_space,
                    PAGING_PROCESS_MAPPING_LINUX_HEAP,
                    PAGING_LINUX_HEAP_BASE + rollback * PAGING_PAGE_SIZE);
                runtime.heap_mapped[rollback] = false;
            }
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        runtime.heap_mapped[index] = true;
    }
    return LINUX_SYSCALL_STATUS_OK;
}

static enum linux_syscall_status execute_call(
    struct linux_syscall_frame *frame,
    uint64_t *return_value
)
{
    switch (runtime.call_index) {
    case 0U:
        if (frame->rdi != ARCH_SET_FS ||
            frame->rsi != runtime.context.fs_address ||
            !user_writable(frame->rsi, sizeof(uint64_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        cpu_write_msr(IA32_FS_BASE, frame->rsi);
        if (cpu_read_msr(IA32_FS_BASE) != frame->rsi) {
            return LINUX_SYSCALL_STATUS_MSR_CONTRACT;
        }
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 1U:
        if (frame->rdi != runtime.context.tid_address ||
            !user_writable(frame->rdi, sizeof(uint32_t))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        *return_value = 1U;
        return LINUX_SYSCALL_STATUS_OK;
    case 2U:
        if (frame->rdi != 0U) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 3U:
        if (frame->rdi != PAGING_LINUX_HEAP_BASE +
                PAGING_LINUX_HEAP_PAGES * PAGING_PAGE_SIZE) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        if (map_heap() != LINUX_SYSCALL_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_MAPPING;
        }
        *return_value = frame->rdi;
        return LINUX_SYSCALL_STATUS_OK;
    case 4U:
        if (frame->rdi != PAGING_LINUX_HEAP_BASE ||
            frame->rsi != PAGING_PAGE_SIZE || frame->rdx != 0U ||
            frame->r10 != UINT64_C(0x32) || frame->r8 != UINT64_MAX ||
            frame->r9 != 0U || !runtime.heap_mapped[0] ||
            paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_HEAP,
                PAGING_LINUX_HEAP_BASE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.heap_mapped[0] = false;
        *return_value = PAGING_LINUX_HEAP_BASE;
        return LINUX_SYSCALL_STATUS_OK;
    case 5U:
        if (frame->rdi != 0U || frame->rsi != PAGING_PAGE_SIZE ||
            frame->rdx != UINT64_C(3) || frame->r10 != UINT64_C(0x22) ||
            frame->r8 != UINT64_MAX || frame->r9 != 0U ||
            runtime.anonymous_mapped ||
            paging_process_map_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS, runtime.context.anonymous_frame,
                PAGING_WRITE) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = true;
        *return_value = PAGING_LINUX_ANON_ADDRESS;
        return LINUX_SYSCALL_STATUS_OK;
    case 6U: {
        uint8_t output[LINUX_SYSCALL_STDOUT_BYTES];

        if (frame->rdi != 1U || frame->rdx != LINUX_SYSCALL_STDOUT_BYTES ||
            runtime.stdout_state != STDOUT_SINK_ARMED ||
            !copy_from_user(output, frame->rsi, sizeof(output))) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        for (size_t index = 0U; index < sizeof(output); ++index) {
            if (output[index] != expected_stdout[index]) {
                return LINUX_SYSCALL_STATUS_STDOUT;
            }
        }
        console_write_n((const char *)output, sizeof(output));
        runtime.stdout_state = STDOUT_SINK_WRITTEN;
        runtime.result.stdout_bytes = LINUX_SYSCALL_STDOUT_BYTES;
        runtime.result.stdout_valid = true;
        *return_value = LINUX_SYSCALL_STDOUT_BYTES;
        return LINUX_SYSCALL_STATUS_OK;
    }
    case 7U:
        if (frame->rdi != PAGING_LINUX_ANON_ADDRESS ||
            frame->rsi != PAGING_PAGE_SIZE || !runtime.anonymous_mapped ||
            paging_process_unmap_user_page(runtime.context.address_space,
                PAGING_PROCESS_MAPPING_LINUX_ANON,
                PAGING_LINUX_ANON_ADDRESS) != PAGING_STATUS_OK) {
            return LINUX_SYSCALL_STATUS_BAD_ARGUMENT;
        }
        runtime.anonymous_mapped = false;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    case 8U:
        if (frame->rdi != 0U || runtime.stdout_state != STDOUT_SINK_WRITTEN ||
            runtime.anonymous_mapped ||
            !runtime.context.exit_observed(
                runtime.context.process_generation)) {
            return LINUX_SYSCALL_STATUS_EXIT;
        }
        runtime.result.exit_status = 0U;
        runtime.result.exit_zero = true;
        *return_value = 0U;
        return LINUX_SYSCALL_STATUS_OK;
    default:
        return LINUX_SYSCALL_STATUS_BAD_ORDER;
    }
}

uintptr_t linux_syscall_dispatch(struct linux_syscall_frame *frame)
{
    enum linux_syscall_status status = validate_entry(frame);
    uint64_t return_value = (uint64_t)(int64_t)-LINUX_ERRNO_EINVAL;
    size_t allowed_index;

    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (transition_cpu(LINUX_SYSCALL_CPU_ENTERED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    runtime.provenance_state = PROVENANCE_ENTERED;
    ++runtime.request_ordinal;
    allowed_index = allowlist_index(frame->rax);
    if (allowed_index == SIZE_MAX) {
        frame->rax = (uint64_t)(int64_t)-LINUX_ERRNO_ENOSYS;
        runtime.provenance_state = PROVENANCE_COMPLETED;
        if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
                LINUX_SYSCALL_STATUS_OK) {
            return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
        }
        runtime.provenance_state = PROVENANCE_CANDIDATE;
        return 0U;
    }
    if (runtime.call_index >= LINUX_SYSCALL_EXPECTED_CALLS ||
        frame->rax != expected_calls[runtime.call_index]) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_ORDER);
    }
    status = execute_call(frame, &return_value);
    if (status != LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(status);
    }
    if (!runtime.seen[allowed_index]) {
        runtime.seen[allowed_index] = true;
        ++runtime.result.distinct_syscalls;
    }
    ++runtime.call_index;
    runtime.result.syscall_count = runtime.call_index;
    runtime.result.real_syscall_instruction = true;
    runtime.result.process_authenticated = true;
    runtime.result.cr3_authenticated = true;
    runtime.provenance_state = PROVENANCE_COMPLETED;
    frame->rax = return_value;
    if (transition_cpu(LINUX_SYSCALL_CPU_RETURNED) !=
            LINUX_SYSCALL_STATUS_OK) {
        return return_to_kernel(LINUX_SYSCALL_STATUS_BAD_STATE);
    }
    runtime.provenance_state = PROVENANCE_CANDIDATE;
    if (runtime.call_index != LINUX_SYSCALL_EXPECTED_CALLS) {
        return 0U;
    }
    if (paging_process_restore_kernel(runtime.context.address_space) !=
            PAGING_STATUS_OK) {
        runtime.result.status = LINUX_SYSCALL_STATUS_RESTORE;
    }
    return linux_process_resume_stack();
}

enum linux_syscall_status linux_syscall_disarm(void)
{
    enum linux_syscall_status result = LINUX_SYSCALL_STATUS_OK;

    if (!runtime.active) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    if (runtime.context.address_space == NULL ||
        runtime.context.address_space->state == PAGING_PROCESS_SPACE_ACTIVE ||
        linux_process_boundary_active() || cpu_interrupts_enabled() ||
        (cpu_read_cr3() & ~(PAGING_PAGE_SIZE - 1U)) !=
            paging_get_state().root_physical_address ||
        (runtime.state != LINUX_SYSCALL_CPU_ARMED &&
            runtime.state != LINUX_SYSCALL_CPU_RETURNED)) {
        return LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    cpu_write_msr(IA32_FS_BASE, runtime.saved_fs_base);
    cpu_write_msr(IA32_FMASK, runtime.saved_fmask);
    cpu_write_msr(IA32_LSTAR, runtime.saved_lstar);
    cpu_write_msr(IA32_STAR, runtime.saved_star);
    cpu_write_msr(IA32_EFER, runtime.saved_efer);
    if (cpu_read_msr(IA32_FS_BASE) != runtime.saved_fs_base ||
        cpu_read_msr(IA32_FMASK) != runtime.saved_fmask ||
        cpu_read_msr(IA32_LSTAR) != runtime.saved_lstar ||
        cpu_read_msr(IA32_STAR) != runtime.saved_star ||
        cpu_read_msr(IA32_EFER) != runtime.saved_efer) {
        result = LINUX_SYSCALL_STATUS_MSR_CONTRACT;
    }
    runtime.stdout_state = STDOUT_SINK_RELEASED;
    runtime.provenance_state = PROVENANCE_RELEASED;
    if (transition_cpu(LINUX_SYSCALL_CPU_DISARMED) !=
            LINUX_SYSCALL_STATUS_OK) {
        result = LINUX_SYSCALL_STATUS_BAD_STATE;
    }
    runtime.result.cpu_disarmed = result == LINUX_SYSCALL_STATUS_OK;
    if (runtime.result.status == LINUX_SYSCALL_STATUS_OK) {
        runtime.result.status = result;
    }
    runtime.active = false;
    linux_syscall_kernel_stack = 0U;
    return result;
}

struct linux_syscall_result linux_syscall_get_result(void)
{
    return runtime.result;
}

bool linux_syscall_resources_released(void)
{
    return !runtime.active && linux_syscall_kernel_stack == 0U &&
        !linux_process_boundary_active();
}
