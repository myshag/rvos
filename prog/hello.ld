/* Link script for a standalone rvos program.

   Fixed load address, so no relocation and no position-independent code is
   needed — the loader maps the segments exactly where the program headers
   say. 0x20000000 is clear of the kernel (0x8xxxxxxx) and of the stack
   (0x30000000).

   Text and data are pushed onto separate pages so each PT_LOAD segment gets
   its own, and the loader can honour r-x versus rw- instead of having to
   widen permissions where two segments share a page. */
OUTPUT_ARCH("riscv")
ENTRY(_start)

SECTIONS
{
    . = 0x20000000;

    .text : {
        *(.text.start)      /* _start first: it is the entry point */
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.* .sdata .sdata.*) }
    .bss  : { *(.sbss .sbss.* .bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
