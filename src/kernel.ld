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

    /* The small-data sections matter: RISC-V GCC puts small objects in
       .sdata/.sbss, and if they are not collected here they land past _end —
       outside the kernel range each task's page table maps, which faults the
       moment a trap touches one. */
    .rodata : { *(.rodata .rodata.*) *(.srodata .srodata.*) }
    .data   : { *(.data .data.*) *(.sdata .sdata.*) }

    . = ALIGN(8);
    .bss : {
        __bss_start = .;
        *(.sbss .sbss.*)
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
