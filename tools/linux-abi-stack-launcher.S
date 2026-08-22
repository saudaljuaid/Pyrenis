.section .text
.global linux_abi_enter
.type linux_abi_enter, @function
linux_abi_enter:
    movq %rdi, %rax
    movq %rsi, %rsp
    xorq %rbp, %rbp
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %rdi, %rdi
    xorq %rsi, %rsi
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    jmp *%rax
.size linux_abi_enter, .-linux_abi_enter

.section .note.GNU-stack,"",@progbits
