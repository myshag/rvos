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
        *srv_blk.o(.text .text.*)
        *srv_proc.o(.text .text.*)
        *srv_dev.o(.text .text.*)
        *loader.o(.text .text.*)
        */sh.o(.text .text.*)
        *srv_rsh.o(.text .text.*)
        *bench.o(.text .text.*)
        *srv_net.o(.text .text.*)
        *net_ip.o(.text .text.*)
        *fat16.o(.text .text.*)
    }
    .urodata : {
        *ulib.o(.rodata .rodata.* .srodata .srodata.*)
        *user.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_null.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_console.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_fs.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_blk.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_proc.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_dev.o(.rodata .rodata.* .srodata .srodata.*)
        *loader.o(.rodata .rodata.* .srodata .srodata.*)
        */sh.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_rsh.o(.rodata .rodata.* .srodata .srodata.*)
        *bench.o(.rodata .rodata.* .srodata .srodata.*)
        *srv_net.o(.rodata .rodata.* .srodata .srodata.*)
        *net_ip.o(.rodata .rodata.* .srodata .srodata.*)
        *fat16.o(.rodata .rodata.* .srodata .srodata.*)
    }
    . = ALIGN(4096);
    __utext_end = .;

    /* Each program's writable state, page-aligned and private to it.

       Initialised data and zero-initialised data are kept apart on purpose:
       .data arrives with its values already in the image and must be left
       alone, while .bss has to be cleared at boot. Zeroing the pair together
       silently resets every initialiser — a bug that stays invisible until
       some program actually has one. */
    /* the filesystem server (open-file cache, FAT16 volume record) */
    __ufs_start = .;
    .ufs_data : {
        *srv_fs.o(.data .data.* .sdata .sdata.*)
        *srv_blk.o(.data .data.* .sdata .sdata.*)
        *fat16.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __ufs_bss_start = .;
    .ufs_bss : {
        *srv_fs.o(.sbss .sbss.* .bss .bss.* COMMON)
        *srv_blk.o(.sbss .sbss.* .bss .bss.* COMMON)
        *fat16.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __ufs_bss_end = .;
    . = ALIGN(4096);
    __ufs_end = .;

    /* the proc server (its rendered-file buffers) */
    __uproc_start = .;
    .uproc_data : {
        *srv_proc.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __uproc_bss_start = .;
    .uproc_bss : {
        *srv_proc.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __uproc_bss_end = .;
    . = ALIGN(4096);
    __uproc_end = .;

    /* the device server (its rendered-file buffers) */
    __udev_start = .;
    .udev_data : {
        *srv_dev.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __udev_bss_start = .;
    .udev_bss : {
        *srv_dev.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __udev_bss_end = .;
    . = ALIGN(4096);
    __udev_end = .;

    /* the ELF loader (its file buffer) */
    __uload_start = .;
    .uload_data : {
        *loader.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __uload_bss_start = .;
    .uload_bss : {
        *loader.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __uload_bss_end = .;
    . = ALIGN(4096);
    __uload_end = .;

    /* the shell (line buffer and ELF scratch).

       The pattern below is anchored with a directory separator on purpose. ld
       matches these against the whole path it was given, so a bare "star sh.o"
       also matches build/srv_rsh.o — which quietly put the remote shell's
       globals in this region, left its own region empty, and produced a store
       page fault the first time it wrote to one of them. A glob that is
       nearly right is worse than one that is wrong: the build succeeds.

       (And a literal star-slash cannot appear in this comment, which is its
       own small lesson about writing linker scripts.) */
    __ush_start = .;
    .ush_data : {
        */sh.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __ush_bss_start = .;
    .ush_bss : {
        */sh.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __ush_bss_end = .;
    . = ALIGN(4096);
    __ush_end = .;

    /* the shell that arrives over TCP (its own line and ELF buffers: sharing
       the local shell's would be sharing writable state between two tasks) */
    __ursh_start = .;
    .ursh_data : {
        *srv_rsh.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __ursh_bss_start = .;
    .ursh_bss : {
        *srv_rsh.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __ursh_bss_end = .;
    . = ALIGN(4096);
    __ursh_end = .;

    /* the network driver and protocol layer */
    __unet_start = .;
    .unet_data : {
        *srv_net.o(.data .data.* .sdata .sdata.*)
        *net_ip.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __unet_bss_start = .;
    .unet_bss : {
        *srv_net.o(.sbss .sbss.* .bss .bss.* COMMON)
        *net_ip.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __unet_bss_end = .;
    . = ALIGN(4096);
    __unet_end = .;

    /* everyone else (ulib, user.o, console, null) */
    __umisc_start = .;
    .umisc_data : {
        *bench.o(.data .data.* .sdata .sdata.*)
        *ulib.o(.data .data.* .sdata .sdata.*)
        *user.o(.data .data.* .sdata .sdata.*)
        *srv_null.o(.data .data.* .sdata .sdata.*)
        *srv_console.o(.data .data.* .sdata .sdata.*)
    }
    . = ALIGN(8);
    __umisc_bss_start = .;
    .umisc_bss : {
        *bench.o(.sbss .sbss.* .bss .bss.* COMMON)
        *ulib.o(.sbss .sbss.* .bss .bss.* COMMON)
        *user.o(.sbss .sbss.* .bss .bss.* COMMON)
        *srv_null.o(.sbss .sbss.* .bss .bss.* COMMON)
        *srv_console.o(.sbss .sbss.* .bss .bss.* COMMON)
    }
    . = ALIGN(8);
    __umisc_bss_end = .;
    . = ALIGN(4096);
    __umisc_end = .;

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
