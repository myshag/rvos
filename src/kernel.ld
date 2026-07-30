/* Linker script for rvos on QEMU 'virt' (RAM base 0x80000000, M-mode). */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .text : {
        *(.text.init)      /* _start must be first */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(8);
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    }

    /* Boot stack for hart 0 (grows down from __stack_top). */
    . = ALIGN(16);
    . += 0x4000;           /* 16 KiB */
    __stack_top = .;

    _end = .;
}
