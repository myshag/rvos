/* Linker script for rvos on QEMU 'virt' (RAM base 0x80000000). */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .init : { *(.text.init) }      /* _start must be first */

    /* ---- user region ------------------------------------------------
       Everything from user.o lands here, page-aligned and contiguous, so it
       can be mapped with the U bit into a user task's address space while
       the kernel around it stays U-less and therefore untouchable from user
       mode. Placing these output sections *before* the kernel ones is what
       captures user.o: ld assigns each input section to the first output
       section that matches it. */
    . = ALIGN(4096);
    __user_start = .;
    .utext : { *user.o(.text .text.*) }
    .urodata : { *user.o(.rodata .rodata.* .srodata .srodata.*) }
    .udata : { *user.o(.data .data.* .sdata .sdata.*) }
    . = ALIGN(8);
    __ubss_start = .;
    .ubss : { *user.o(.sbss .sbss.* .bss .bss.* COMMON) }
    . = ALIGN(8);
    __ubss_end = .;
    . = ALIGN(4096);
    __user_end = .;

    /* ---- kernel ------------------------------------------------------ */
    .text : { *(.text .text.*) }

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
