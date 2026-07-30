/* Linker script for rvos on QEMU 'virt' (RAM base 0x80000000). */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x80000000;

    .init : { *(.text.init) }      /* _start must be first */

    /* ---- user space --------------------------------------------------
       Everything that runs unprivileged lives here, page-aligned, so it can
       be mapped with the U bit while the kernel around it stays U-less and
       therefore untouchable from user mode. Placing these output sections
       *before* the kernel ones is what captures the objects: ld assigns each
       input section to the first output section that matches it.

       Text and rodata are shared by all user programs, like a C library.
       Writable state is not: a server with globals gets its own page-aligned
       region, mapped only into its own address space, so one server cannot
       reach another's buffers. */
    . = ALIGN(4096);
    __utext_start = .;
    .utext : {
        *ulib.o(.text .text.*)
        *user.o(.text .text.*)
        *srv_null.o(.text .text.*)
        *srv_console.o(.text .text.*)
        *srv_fs.o(.text .text.*)
        *srv_proc.o(.text .text.*)
        *loader.o(.text .text.*)
        *sh.o(.text .text.*)
        *srv_net.o(.text .text.*)
        *fat16.o(.text .text.*)
    }
    .urodata : {
        *ulib.o(.rodata .rodata.* .srodata .srodata.*)
        *user.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_null.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_console.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_fs.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_proc.o(.rodata .rodata.* .srodata .srodata.*)
        *loader.o(.rodata .rodata.* .srodata .srodata.*)
        *sh.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_net.o(.rodata .rodata.* .srodata .srodata.*)
        *fat16.o(.rodata .rodata.* .srodata .srodata.*)
    }
    . = ALIGN(4096);
    __utext_end = .;

    /* private state: the filesystem server (its open-file cache and the
       FAT16 driver's mounted-volume record) */
    __ufs_start = .;
    .ufs : {
        *srv_fs.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
        *fat16.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __ufs_end = .;

    /* private state: the proc server (its rendered-file buffers) */
    __uproc_start = .;
    .uproc : {
        *srv_proc.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __uproc_end = .;

    /* private state: the ELF loader (its file buffer) */
    __uload_start = .;
    .uload : {
        *loader.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __uload_end = .;

    /* private state: the shell (its line buffer and its own ELF scratch) */
    __ush_start = .;
    .ush : {
        *sh.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __ush_end = .;

    /* private state: the network driver (its ring and buffer pointers) */
    __unet_start = .;
    .unet : {
        *srv_net.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __unet_end = .;

    /* private state: everyone else (ulib, user.o) — usually empty */
    __umisc_start = .;
    .umisc : {
        *ulib.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
        *user.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
        *srv_null.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
        *srv_console.o(.data .data.* .sdata .sdata.* .sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(4096);
    __umisc_end = .;

    /* All of the above is zeroed at boot: these regions hold only .bss in
       practice, and boot.S clears the kernel's bss alone. */
    __udata_start = __ufs_start;
    __udata_end   = __umisc_end;

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
