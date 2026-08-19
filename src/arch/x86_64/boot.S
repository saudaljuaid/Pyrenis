/* SPDX-License-Identifier: GPL-3.0-only */
/* Seneri OS: Multiboot2 entry and the controlled transition to long mode. */
.set MB2_HEADER_MAGIC, 0xE85250D6
.set MB2_BOOT_MAGIC,   0x36D76289
.set MB2_ARCH_I386,    0
.set PAGE_PRESENT,     1 << 0
.set PAGE_WRITABLE,    1 << 1
.set PAGE_HUGE,        1 << 7
.set CR4_PAE,          1 << 5
.set EFER_MSR,         0xC0000080
.set EFER_LME,         1 << 8
.set CR0_PAGING,       1 << 31

.section .multiboot, "a"
.balign 8
multiboot_header:
    .long MB2_HEADER_MAGIC
    .long MB2_ARCH_I386
    .long multiboot_header_end - multiboot_header
    .long -(MB2_HEADER_MAGIC + MB2_ARCH_I386 + (multiboot_header_end - multiboot_header))
    /* Ask for both ACPI RSDP forms without making either one mandatory alone. */
    .short 1
    .short 1
    .long 16
    .long 14
    .long 15
    /*
     * Ask for a linear framebuffer. Tagged optional, so a loader that cannot
     * set a graphics mode still boots this kernel and simply reports no
     * framebuffer - the same shape as a machine that declares no MCFG. The
     * request is a preference, not a contract: Multiboot2 3.1.10 lets the
     * loader answer with whatever mode it actually has, so nothing downstream
     * may assume these numbers came back.
     */
    .short 5
    .short 1
    .long 20
    .long 1024
    .long 768
    .long 32
    /* Tags are 8-byte aligned and the framebuffer request is 20 bytes long. */
    .long 0
    .short 0
    .short 0
    .long 8
multiboot_header_end:

.section .boot.text, "ax"
.code32
.global _start
.type _start, @function
.extern kernel_main
_start:
    cli
    cld
    movl %eax, boot_magic
    movl %ebx, boot_info
    movl $stack_top, %esp
    cmpl $MB2_BOOT_MAGIC, %eax
    jne .bad_magic

    /* CPUID is mandatory because long-mode discovery depends on it. */
    pushfl
    popl %eax
    movl %eax, %ecx
    xorl $0x00200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    xorl %ecx, %eax
    jz .no_cpuid

    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb .no_long_mode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz .no_long_mode

    /* Zero a PML4, PDPT, and four 512-entry 2 MiB page directories. */
    movl $pml4_table, %edi
    xorl %eax, %eax
    movl $(6 * 4096 / 4), %ecx
    rep stosl
    movl $pdpt_table, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, pml4_table
    movl $pd_tables, %edi
    movl $pdpt_table, %esi
    movl $4, %ecx
.link_directory:
    movl %edi, %eax
    orl $(PAGE_PRESENT | PAGE_WRITABLE), %eax
    movl %eax, (%esi)
    movl $0, 4(%esi)
    addl $4096, %edi
    addl $8, %esi
    loop .link_directory
    movl $pd_tables, %edi
    movl $(PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE), %eax
    movl $2048, %ecx
.map_page:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .map_page

    /* Install paging, enable long mode, and load a known-good GDT. */
    movl $pml4_table, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    orl $CR4_PAE, %eax
    mov %eax, %cr4
    movl $EFER_MSR, %ecx
    rdmsr
    orl $EFER_LME, %eax
    wrmsr
    mov %cr0, %eax
    orl $CR0_PAGING, %eax
    mov %eax, %cr0
    lgdt gdt64_pointer
    ljmp $0x08, $long_mode_start

.bad_magic:
    movb $'M', %al
    jmp .boot_failure
.no_cpuid:
    movb $'C', %al
    jmp .boot_failure
.no_long_mode:
    movb $'L', %al
.boot_failure:
    movb %al, 0xB8000
    movb $0x4F, 0xB8001
    cli
.halt32:
    hlt
    jmp .halt32

.code64
long_mode_start:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    xorq %rbp, %rbp
    movl boot_magic(%rip), %edi
    movl boot_info(%rip), %esi
    call kernel_main
.halt64:
    cli
    hlt
    jmp .halt64
.size _start, . - _start

.section .rodata, "a"
.balign 8
gdt64:
    .quad 0x0000000000000000
    /* Accessed bits start set so descriptor loads never need to write here. */
    .quad 0x00AF9B000000FFFF
    .quad 0x00CF93000000FFFF
gdt64_end:
gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .long gdt64

.section .bss, "aw", @nobits
.balign 4
boot_magic: .skip 4
boot_info:  .skip 4
.balign 4096
pml4_table: .skip 4096
pdpt_table: .skip 4096
pd_tables:  .skip (4 * 4096)
.balign 16
stack_bottom: .skip 16384
stack_top:

.section .note.GNU-stack, "", @progbits
