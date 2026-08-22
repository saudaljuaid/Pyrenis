/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_LINUX_ABI_H
#define SAPOTE_LINUX_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINUX_ABI_STACK_FOUNDATION_CONTROLS 8U
#define LINUX_ABI_IMAGE_STACK_FOUNDATION_CONTROLS 32U
#define LINUX_ABI_LIFECYCLE_CONTROLS 7U
#define LINUX_ABI_CONTROLLED_ROBUSTNESS_TESTS 72U

enum linux_process_state {
    LINUX_PROCESS_CANDIDATE = 0,
    LINUX_PROCESS_BUILDING,
    LINUX_PROCESS_INSTALLED,
    LINUX_PROCESS_RUNNING,
    LINUX_PROCESS_EXITING,
    LINUX_PROCESS_STOPPING,
    LINUX_PROCESS_RELEASED,
    LINUX_PROCESS_STATE_COUNT
};

enum linux_executable_state {
    LINUX_EXECUTABLE_CANDIDATE = 0,
    LINUX_EXECUTABLE_VALIDATED,
    LINUX_EXECUTABLE_INSTALLED,
    LINUX_EXECUTABLE_RELEASED,
    LINUX_EXECUTABLE_STATE_COUNT
};

enum linux_stack_state {
    LINUX_STACK_CANDIDATE = 0,
    LINUX_STACK_BUILDING,
    LINUX_STACK_INSTALLED,
    LINUX_STACK_RELEASED,
    LINUX_STACK_STATE_COUNT
};

enum linux_abi_status {
    LINUX_ABI_STATUS_OK = 0,
    LINUX_ABI_STATUS_ABSENT,
    LINUX_ABI_STATUS_NULL_ARGUMENT,
    LINUX_ABI_STATUS_BUSY,
    LINUX_ABI_STATUS_PREREQUISITE,
    LINUX_ABI_STATUS_FILESYSTEM,
    LINUX_ABI_STATUS_ELF,
    LINUX_ABI_STATUS_ELF_INSTALL,
    LINUX_ABI_STATUS_STACK,
    LINUX_ABI_STATUS_FRAME_ALLOCATION,
    LINUX_ABI_STATUS_ADDRESS_SPACE,
    LINUX_ABI_STATUS_ALIAS,
    LINUX_ABI_STATUS_MAPPING,
    LINUX_ABI_STATUS_PERMISSION_AUDIT,
    LINUX_ABI_STATUS_SYSCALL_CPU,
    LINUX_ABI_STATUS_ENTRY,
    LINUX_ABI_STATUS_EXIT,
    LINUX_ABI_STATUS_TRANSITION_REPEATED,
    LINUX_ABI_STATUS_TRANSITION_REVERSED,
    LINUX_ABI_STATUS_TRANSITION_INVALID,
    LINUX_ABI_STATUS_TEARDOWN,
    LINUX_ABI_STATUS_RESOURCE_CENSUS,
    LINUX_ABI_STATUS_ROBUSTNESS,
    LINUX_ABI_STATUS_COUNT
};

struct linux_abi_proof_result {
    uint32_t file_bytes;
    uint32_t program_headers;
    uint32_t load_segments;
    uint32_t file_clusters;
    uint32_t stdout_bytes;
    uint32_t syscall_count;
    uint32_t distinct_syscalls;
    uint32_t exit_status;
    uint32_t robustness_tests;
    bool ring_three;
    bool private_address_space;
    bool real_syscall_instruction;
    bool stdout_valid;
    bool exit_zero;
    bool unknown_enosys;
    bool write_xor_execute;
    bool kernel_cr3_restored;
    bool teardown_complete;
    bool resource_census_equal;
};

bool linux_abi_image_stack_foundation_self_test(size_t *completed_tests);
enum linux_abi_status linux_abi_installed_prove(
    struct linux_abi_proof_result *result
);
struct linux_abi_proof_result linux_abi_get_proof_result(void);
bool linux_abi_resources_released(void);
const char *linux_abi_status_string(enum linux_abi_status status);

#endif
