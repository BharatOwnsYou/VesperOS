/* x86_64 multiboot2 entry. grub drops us in 32-bit pm with paging off:
 *   eax = 0x36d76289 (mb2 magic), ebx = phys addr of the mb2 info block
 * switch to long mode here then call kernel_main(magic, info).
 * page tables are prebuilt (boot_tables.S) so this is all straight-line, order is
 * CR4.PAE -> CR3 -> EFER.LME -> CR0.PG -> lgdt -> far jmp to a 64-bit code seg.
 * progress goes out port 0x80 (post card) and 0xE9 (qemu debugcon), harmless on real hw.
 * 'x86_64 required' error also goes to the vga text buffer for bios boots */

    .set STAGE_ENTER,   0x10
    .set STAGE_CPUOK,   0x11
    .set STAGE_PAGING,  0x12
    .set STAGE_LONG,    0x13
    .set STAGE_NOLM,    0xEE

/* stage markers are plain instrs, no macros, so gas and clang see the same input */

/* multiboot2 header (same as the 32-bit boot.asm) */
    .section .multiboot, "a"
    .balign 8
multiboot_header:
    .long   0xE85250D6              /* magic */
    .long   0                       /* architecture 0 = i386 (GRUB always enters in 32-bit protected mode) */
    .long   32                      /* header length: 16 + 8 (EGA text tag) + 8 (end tag) */
    .long   0x17ADAF0A              /* checksum = -(magic + arch + length) mod 2^32 */
    .balign 8
    .short  4                       /* EGA text support tag, flags=1: optional */
    .short  1
    .long   8
    .balign 8
    .short  0                       /* end tag */
    .short  0
    .long   8
multiboot_header_end:

/* state */
    .section .bss
    .balign 16
    .globl boot_stack_bottom
boot_stack_bottom:
    .skip   65536                   /* 64 KiB boot/kernel stack */
    .globl boot_stack_top
boot_stack_top:
    .balign 8
mb_magic:   .skip 4
mb_info:    .skip 4

    .section .data
    .balign 16
boot_gdt:
    .quad   0x0000000000000000      /* 0x00 null */
    .quad   0x00AF9A000000FFFF      /* 0x08 64-bit code, DPL0 (L=1, D=0) */
    .quad   0x00CF92000000FFFF      /* 0x10 data, DPL0 */
boot_gdt_end:
boot_gdt_ptr:
    .word   boot_gdt_end - boot_gdt - 1
    .long   boot_gdt

msg_nolm:
    .asciz  "VesperOS: this CPU cannot run x86_64 code. Reboot and choose the i686 entry."

/* 32-bit entry */
    .section .text
    .code32
    .globl _start
    .type _start, @function
_start:
    cli
    cld
    movl    %eax, mb_magic              /* save before anything clobbers eax/ebx */
    movl    %ebx, mb_info
    movl    $boot_stack_top, %esp
    movb    $STAGE_ENTER, %al
    outb    %al, $0x80
    movb    $0x31, %al        /* '1' */
    outb    %al, $0xE9

    /* CPUID present? (EFLAGS.ID, bit 21, must be toggleable) */
    pushfl
    popl    %eax
    movl    %eax, %ecx
    xorl    $0x00200000, %eax
    pushl   %eax
    popfl
    pushfl
    popl    %eax
    pushl   %ecx
    popfl
    xorl    %ecx, %eax
    testl   $0x00200000, %eax
    jz      .Lno_long_mode

    /* extended CPUID leaves present, and long-mode bit (EDX bit 29 of leaf 0x80000001) set? */
    movl    $0x80000000, %eax
    cpuid
    cmpl    $0x80000001, %eax
    jb      .Lno_long_mode
    movl    $0x80000001, %eax
    cpuid
    testl   $0x20000000, %edx
    jz      .Lno_long_mode
    movb    $STAGE_CPUOK, %al
    outb    %al, $0x80
    movb    $0x32, %al        /* '2' */
    outb    %al, $0xE9

    /* 1. PAE */
    movl    %cr4, %eax
    orl     $0x00000020, %eax
    movl    %eax, %cr4
    /* 2. page tables (static, 0..4 GiB identity) */
    movl    $boot_pml4, %eax
    movl    %eax, %cr3
    /* 3. long mode enable in EFER */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x00000100, %eax
    wrmsr
    /* 4. GDT with a 64-bit code segment, then paging on => the CPU is now in IA-32e *compatibility* mode */
    lgdt    boot_gdt_ptr
    movl    %cr0, %eax
    orl     $0x80000000, %eax
    movl    %eax, %cr0
    movb    $STAGE_PAGING, %al
    outb    %al, $0x80
    movb    $0x33, %al        /* '3' */
    outb    %al, $0xE9
    /* 5. far jump into the 64-bit code segment => true long mode */
    ljmpl   $0x08, $.Llong_mode

.Lno_long_mode:
    movb    $STAGE_NOLM, %al
    outb    %al, $0x80
    movb    $0x21, %al        /* '!' */
    outb    %al, $0xE9
    movl    $msg_nolm, %esi
    movl    $0xB8000, %edi
1:  lodsb
    testb   %al, %al
    jz      2f
    movb    $0x4F, %ah                  /* attribute: white on red */
    movw    %ax, (%edi)
    addl    $2, %edi
    jmp     1b
2:  cli
    hlt
    jmp     2b

/* 64-bit entry */
    .code64
.Llong_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorl    %eax, %eax
    movw    %ax, %fs
    movw    %ax, %gs
    leaq    boot_stack_top(%rip), %rsp
    xorl    %ebp, %ebp
    movl    mb_magic(%rip), %edi        /* kernel_main(u32 magic, u32 mb_info) per the SysV ABI */
    movl    mb_info(%rip), %esi
    movb    $STAGE_LONG, %al
    outb    %al, $0x80
    movb    $0x34, %al        /* '4' */
    outb    %al, $0xE9
    call    kernel_main
.Lhang:
    cli
    hlt
    jmp     .Lhang

    .size _start, . - _start
    .section .note.GNU-stack,"",@progbits
