/* kmain.c — kernel entry after boot.S. Stage 1: bring up the console. */
#include "uart.h"

void kmain(void)
{
    uart_init();
    kprintf("\n");
    kprintf("=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 1: boot + UART on QEMU 'virt' (M-mode)\n");
    kprintf("=============================================\n");
    kprintf("hartid 0 alive, console up. hex=%x dec=%d ptr=%p\n",
            0xcafe, -123, (void *)0x80000000UL);
    kprintf("[ok] kmain reached; halting.\n");

    for (;;)
        __asm__ volatile("wfi");
}
