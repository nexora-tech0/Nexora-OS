/*
 * Nexora OS - init/kernel.ld
 * Kernel image layout: higher-half ELF64 at 0xffffffff80000000 (-mcmodel=kernel range),
 * page-aligned segments with W^X permissions, Limine requests in their own segment.
 * mm/paging.c maps the image by these section bounds: __KernelStart, __TextStart, __RodataStart,
 * __DataStart, __KernelEnd (all page aligned). .hzdrivers holds the built-in driver table
 * (IO_DECLARE_BUILTIN_DRIVER), bounded by __DriverTableStart/End.
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(KiSystemStartup)

PHDRS
{
    limine_requests PT_LOAD FLAGS(4 | 2);  /* R W : Limine writes response pointers */
    text            PT_LOAD FLAGS(4 | 1);  /* R X */
    rodata          PT_LOAD FLAGS(4);      /* R   */
    data            PT_LOAD FLAGS(4 | 2);  /* R W */
}

SECTIONS
{
    . = 0xffffffff80000000;
    __KernelStart = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __TextStart = .;
    .text : {
        *(.text .text.*)
    } :text
    __TextEnd = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __RodataStart = .;
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    .hzdrivers : {
        __DriverTableStart = .;
        KEEP(*(.hzdrivers))
        __DriverTableEnd = .;
    } :rodata

    .init_array : {
        __InitArrayStart = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __InitArrayEnd = .;
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __DataStart = .;
    .data : {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.plt)
    } :data

    .bss (NOLOAD) : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __KernelEnd = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .eh_frame.*)
        *(.note .note.*)
        *(.comment)
        *(.interp .dynamic .dynsym .dynstr .hash .gnu.hash)
    }
}
