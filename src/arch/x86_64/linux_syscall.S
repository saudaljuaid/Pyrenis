/* SPDX-License-Identifier: GPL-3.0-only */
/* Reviewed Linux SYSCALL entry and checked IRETQ return boundary. */

.set KERNEL_DATA_SELECTOR, 0x10
.set USER_DATA_SELECTOR, 0x2B
.set USER_CODE_SELECTOR, 0x33
.set USER_RFLAGS, 0x2

.section .text, "ax"
.code64

.global linux_process_enter_user
.type linux_process_enter_user, @function
linux_process_enter_user:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, linux_process_saved_resume(%rip)
    movb $1, linux_process_active(%rip)

    movq %rdi, %r10
    movq %rsi, %r11
    pushq $USER_DATA_SELECTOR
    pushq %r11
    pushq $USER_RFLAGS
    pushq $USER_CODE_SELECTOR
    pushq %r10

    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %ebp, %ebp
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq
    ud2
.size linux_process_enter_user, . - linux_process_enter_user

/*
 * SYSCALL leaves the user RSP live and does not load a kernel stack.  The
 * first instruction saves it without a push; the second selects the already
 * validated TSS/RSP0 stack.  FMASK and the user entry flags keep IF clear
 * throughout this incomplete-frame window.
 */
.global linux_syscall_entry
.type linux_syscall_entry, @function
linux_syscall_entry:
    movq %rsp, linux_syscall_user_rsp(%rip)
    movq linux_syscall_kernel_stack(%rip), %rsp
    cld

    pushq $USER_DATA_SELECTOR
    pushq linux_syscall_user_rsp(%rip)
    pushq %r11
    pushq $USER_CODE_SELECTOR
    pushq %rcx

    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbp
    pushq %rbx
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax

    movq %rsp, %rdi
    call linux_syscall_dispatch
    testq %rax, %rax
    jne .return_to_kernel

    popq %rax
    popq %rdi
    popq %rsi
    popq %rdx
    popq %r10
    popq %r8
    popq %r9
    popq %rbx
    popq %rbp
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    movq 0(%rsp), %rcx
    movq 16(%rsp), %r11
    iretq
    ud2

.return_to_kernel:
    movq %rax, %rsp
    movb $0, linux_process_active(%rip)
    movq $0, linux_process_saved_resume(%rip)
    movq $0, linux_syscall_user_rsp(%rip)
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size linux_syscall_entry, . - linux_syscall_entry

.global linux_process_resume_stack
.type linux_process_resume_stack, @function
linux_process_resume_stack:
    cmpb $0, linux_process_active(%rip)
    je .resume_inactive
    movq linux_process_saved_resume(%rip), %rax
    ret
.resume_inactive:
    xorl %eax, %eax
    ret
.size linux_process_resume_stack, . - linux_process_resume_stack

.global linux_process_boundary_active
.type linux_process_boundary_active, @function
linux_process_boundary_active:
    xorl %eax, %eax
    cmpb $0, linux_process_active(%rip)
    setne %al
    ret
.size linux_process_boundary_active, . - linux_process_boundary_active

.section .bss, "aw", @nobits
.balign 8
linux_process_saved_resume:
    .skip 8
linux_syscall_user_rsp:
    .skip 8
linux_process_active:
    .skip 1

.section .note.GNU-stack, "", @progbits
