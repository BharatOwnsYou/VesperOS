; ============================================================
; VesperOS 4 - boot.asm
; Multiboot2 entry stub, x86_32 protected mode.
; GRUB (BIOS or UEFI) loads this and jumps here with:
;   eax = 0x36d76289 (multiboot2 magic)
;   ebx = physical address of the multiboot2 info structure
; ============================================================

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0
MB2_HDR_LEN     equ (multiboot_header_end - multiboot_header)
MB2_CHECKSUM    equ -(MB2_MAGIC + MB2_ARCH_I386 + MB2_HDR_LEN)

section .multiboot
align 8
multiboot_header:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd MB2_HDR_LEN
    dd MB2_CHECKSUM

    ; EGA text support tag: tells GRUB we'd prefer legacy VGA text mode
    ; (0xB8000) over a UEFI GOP linear framebuffer. flags=1 => optional -
    ; if firmware genuinely can't provide it, GRUB proceeds anyway instead
    ; of refusing to boot outright. kernel_main() already detects and warns
    ; about a non-text framebuffer at runtime.
    align 8
    dw 4    ; type = EGA text support
    dw 1    ; flags = optional
    dd 8    ; size

    ; end tag
    align 8
    dw 0    ; type
    dw 0    ; flags
    dd 8    ; size
multiboot_header_end:

section .bss
align 16
stack_bottom:
    resb 65536      ; 64 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    cli
    mov esp, stack_top

    ; pass multiboot magic + info pointer to kernel_main(eax, ebx)
    push ebx
    push eax
    call kernel_main

    ; kernel_main should never return, but just in case:
.hang:
    cli
    hlt
    jmp .hang
