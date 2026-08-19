/* SPDX-License-Identifier: GPL-3.0-only */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_BASE = 1M;

/*
 * src/kernel/paging.c maps this image one 4 KiB page at a time and gives each
 * section its own permissions, so no two permissions may share a page. Every
 * section therefore starts on a page boundary and pads to one, which makes each
 * __*_start and __*_end pair a whole number of pages and makes the end of one
 * section exactly the start of the next.
 */
PHDRS
{
    multiboot PT_LOAD FLAGS(4);
    text      PT_LOAD FLAGS(5);
    rodata    PT_LOAD FLAGS(4);
    data      PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_BASE;
    __kernel_start = .;

    .multiboot ALIGN(8) :
    {
        __multiboot_header_start = .;
        KEEP(*(.multiboot))
        __multiboot_header_end = .;
    } :multiboot

    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.boot.text)
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    } :text

    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        /*
         * Rust emits constants that would need relocating in a position
         * independent image here. This kernel is linked at a fixed address, so
         * they are fixed at link time and belong with the other constants -
         * read-only, and never in the writable segment.
         */
        *(.data.rel.ro .data.rel.ro.*)

        /*
         * A freestanding kernel linked at a fixed address has no business
         * owning a global offset table or a procedure linkage table. They are
         * named here rather than left as orphans so the linker cannot place
         * them somewhere of its choosing, and asserted empty below so that if
         * anything ever does need one, the build says so instead of quietly
         * growing an indirection layer nothing initialises.
         */
        __got_start = .;
        *(.got .got.plt .igot.plt)
        *(.plt .plt.* .iplt)
        __got_end = .;
        . = ALIGN(4K);
        __rodata_end = .;
    } :rodata

    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4K);
        __data_end = .;
    } :data

    .bss ALIGN(4K) (NOLOAD) :
    {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4K);
        __bss_end = .;
    } :data

    __kernel_end = .;

    /*
     * Everything the toolchains emit that this kernel does not run. The LLVM
     * bitcode and command sections arrive with the Rust static library and are
     * build metadata rather than code; keeping them would put megabytes of it
     * in the image and, worse, leave it to the linker to decide where.
     */
    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
        *(.llvmbc)
        *(.llvmcmd)

        /*
         * Relocations for a fully linked, non-relocatable image. Discarding
         * them is safe only because the GOT assertion below proves nothing
         * needed relocating.
         */
        *(.rela .rela.*)
    }

    /*
     * Debug information, kept but never loaded. Named explicitly because
     * --orphan-handling=error refuses anything this script does not mention,
     * and losing line numbers to keep the linker honest is a bad trade.
     */
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_line_str 0 : { *(.debug_line_str) }
    .debug_loc 0 : { *(.debug_loc) }
    .debug_loclists 0 : { *(.debug_loclists) }
    .debug_macro 0 : { *(.debug_macro) }
    .debug_ranges 0 : { *(.debug_ranges) }
    .debug_rnglists 0 : { *(.debug_rnglists) }
    .debug_str 0 : { *(.debug_str) }
    .debug_str_offsets 0 : { *(.debug_str_offsets) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_pubtypes 0 : { *(.debug_pubtypes) }

    ASSERT(__multiboot_header_end - __multiboot_header_start >= 24,
           "Multiboot2 header is incomplete")
    ASSERT(__multiboot_header_start - KERNEL_BASE < 32768,
           "Multiboot2 header is outside the first 32 KiB")
    ASSERT(__kernel_end < 0x40000000,
           "Day-one identity map no longer covers the kernel")
    ASSERT(interrupt_vector_table_end - interrupt_vector_table == 256 * 8,
           "Interrupt vector table must contain exactly 256 addresses")

    /* Each permission boundary must be a page boundary, or one page would
       carry two permissions and the weaker one would win. */
    ASSERT((__kernel_start & 0xFFF) == 0, "kernel image is not page aligned")
    ASSERT((__text_start & 0xFFF) == 0, "kernel text does not start on a page")
    ASSERT((__text_end & 0xFFF) == 0, "kernel text does not end on a page")
    ASSERT((__rodata_start & 0xFFF) == 0, "kernel rodata starts mid-page")
    ASSERT((__rodata_end & 0xFFF) == 0, "kernel rodata ends mid-page")
    ASSERT((__data_start & 0xFFF) == 0, "kernel data does not start on a page")
    ASSERT((__bss_start & 0xFFF) == 0, "kernel bss does not start on a page")
    ASSERT((__kernel_end & 0xFFF) == 0, "kernel image does not end on a page")

    /* The permission map must cover the image with no gap between sections. */
    ASSERT(__text_end == __rodata_start, "a gap separates text and rodata")
    ASSERT(__rodata_end == __data_start, "a gap separates rodata and data")
    ASSERT(__data_end == __bss_start, "a gap separates kernel data and bss")
    ASSERT(__bss_end == __kernel_end, "kernel bss does not end the image")
    ASSERT(__got_end == __got_start,
           "the kernel gained a global offset table")

    /*
     * paging.c gives the 2 MiB regions holding this image 4 KiB granularity and
     * keeps one fixed-size region descriptor for each, so the image has to stay
     * inside the count PAGING_MAX_KERNEL_FINE_REGIONS statically asserts.
     */
    ASSERT(__kernel_end <= 0x800000,
           "kernel image no longer fits the fine-grained paging window")
}
