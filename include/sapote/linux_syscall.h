/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_LINUX_SYSCALL_H
#define SAPOTE_LINUX_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/paging.h>

#define LINUX_SYSCALL_ALLOWLIST_COUNT 7U
#define LINUX_SYSCALL_ALLOWLIST_MAX 16U
#define LINUX_SYSCALL_EXPECTED_CALLS 9U
#define LINUX_SYSCALL_STDOUT_BYTES 7U
#define LINUX_SYSCALL_CPU_FOUNDATION_CONTROLS 10U
#define LINUX_SYSCALL_SEMANTIC_CONTROLS 11U

enum linux_syscall_cpu_state {
    LINUX_SYSCALL_CPU_CANDIDATE = 0,
    LINUX_SYSCALL_CPU_ARMED,
    LINUX_SYSCALL_CPU_ENTERED,
    LINUX_SYSCALL_CPU_RETURNED,
    LINUX_SYSCALL_CPU_DISARMED,
    LINUX_SYSCALL_CPU_STATE_COUNT
};

enum linux_syscall_status {
    LINUX_SYSCALL_STATUS_OK = 0,
    LINUX_SYSCALL_STATUS_NULL_ARGUMENT,
    LINUX_SYSCALL_STATUS_BUSY,
    LINUX_SYSCALL_STATUS_UNSUPPORTED_CPU,
    LINUX_SYSCALL_STATUS_DESCRIPTOR_CONTRACT,
    LINUX_SYSCALL_STATUS_MSR_CONTRACT,
    LINUX_SYSCALL_STATUS_BAD_STATE,
    LINUX_SYSCALL_STATUS_BAD_PROCESS,
    LINUX_SYSCALL_STATUS_BAD_GENERATION,
    LINUX_SYSCALL_STATUS_BAD_CR3,
    LINUX_SYSCALL_STATUS_BAD_STACK,
    LINUX_SYSCALL_STATUS_BAD_ENTRY,
    LINUX_SYSCALL_STATUS_BAD_RETURN,
    LINUX_SYSCALL_STATUS_BAD_PROVENANCE,
    LINUX_SYSCALL_STATUS_BAD_ORDER,
    LINUX_SYSCALL_STATUS_BAD_ARGUMENT,
    LINUX_SYSCALL_STATUS_BAD_POINTER,
    LINUX_SYSCALL_STATUS_MAPPING,
    LINUX_SYSCALL_STATUS_STDOUT,
    LINUX_SYSCALL_STATUS_EXIT,
    LINUX_SYSCALL_STATUS_RESTORE,
    LINUX_SYSCALL_STATUS_COUNT
};

/* Exact stack image built by src/arch/x86_64/linux_syscall.S. */
struct linux_syscall_frame {
    uint64_t rax;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t r10;
    uint64_t r8;
    uint64_t r9;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

struct linux_syscall_context {
    struct paging_process_space *address_space;
    uint64_t process_generation;
    uint64_t executable_start;
    uint64_t executable_end;
    uint64_t stack_start;
    uint64_t stack_end;
    uint64_t fs_address;
    uint64_t tid_address;
    uintptr_t heap_frames[PAGING_LINUX_HEAP_PAGES];
    uintptr_t anonymous_frame;
    bool (*exit_observed)(uint64_t process_generation);
};

struct linux_syscall_result {
    uint32_t syscall_count;
    uint32_t distinct_syscalls;
    uint32_t stdout_bytes;
    uint32_t exit_status;
    enum linux_syscall_status status;
    enum linux_syscall_cpu_state cpu_state;
    bool stdout_valid;
    bool exit_zero;
    bool real_syscall_instruction;
    bool process_authenticated;
    bool cr3_authenticated;
    bool cpu_disarmed;
};

bool linux_syscall_cpu_foundation_self_test(size_t *completed_tests);
bool linux_syscall_enosys_self_test(void);
enum linux_syscall_status linux_syscall_arm(
    const struct linux_syscall_context *context
);
enum linux_syscall_status linux_syscall_validate_armed(void);
enum linux_syscall_status linux_syscall_disarm(void);
struct linux_syscall_result linux_syscall_get_result(void);
bool linux_syscall_resources_released(void);
uintptr_t linux_syscall_dispatch(struct linux_syscall_frame *frame);

void linux_process_enter_user(uint64_t entry, uint64_t stack_pointer);
uintptr_t linux_process_resume_stack(void);
bool linux_process_boundary_active(void);
extern const uint8_t linux_syscall_entry[];

#endif
