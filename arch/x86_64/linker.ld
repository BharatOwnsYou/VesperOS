/* VesperOS 4 - x86_64 kernel image. Linked at 1 MiB, virtual == physical (identity-mapped by boot_tables.S).
 * ELF64 so that GRUB's multiboot2 loader accepts it; _start is 32-bit code (see boot.S). Link with
 * -z max-page-size=0x1000 so the multiboot header lands inside GRUB's 32 KiB search window. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* Segments are pinned explicitly so GNU ld and lld produce the same layout: no segment for the ELF headers (lld may
 * otherwise place one just below 1 MiB, inside the legacy BIOS/VGA hole, which GRUB cannot load). */
PHDRS
{
    boot   PT_LOAD FLAGS(4);        /* R   : multiboot header          */
    text   PT_LOAD FLAGS(5);        /* R+X : code                      */
    rodata PT_LOAD FLAGS(4);        /* R   : constants                 */
    data   PT_LOAD FLAGS(6);        /* R+W : data, page tables, .bss   */
}

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot : ALIGN(8) { *(.multiboot) } :boot

    .text : ALIGN(4K) { *(.text .text.*) } :text

    .rodata : ALIGN(4K) { *(.rodata .rodata.*) } :rodata

    .data : ALIGN(4K) { *(.data .data.*) } :data

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    } :data
    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note.*) }
}
