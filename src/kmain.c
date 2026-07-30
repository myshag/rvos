/* kmain.c — Stage 3: synchronous IPC. A server task blocks in recv(); two
   client tasks send it numbered messages. Ordering proves the rendezvous
   (senders block until the server picks them up). */
#include "uart.h"
#include "task.h"
#include "syscall.h"

#define SERVER_ID 0

static void spin(volatile unsigned long n) { while (n--) __asm__ volatile("nop"); }

static void server(void)
{
    kprintf("  [server] up, waiting for messages\n");
    for (;;) {
        uint64 msg;
        int from = sys_recv(&msg);
        kprintf("  [server] got 0x%lx from task %d\n", msg, from);
    }
}

static void client1(void)
{
    for (uint64 i = 0;; i++) {
        sys_send(SERVER_ID, 0x1000 + i);     /* blocks until server recvs */
        spin(4000000);
    }
}

static void client2(void)
{
    for (uint64 i = 0;; i++) {
        sys_send(SERVER_ID, 0x2000 + i);
        spin(6000000);
    }
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 3: synchronous IPC (send/recv)\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    task_create("server", server);          /* id 0 */
    task_create("client1", client1);        /* id 1 */
    task_create("client2", client2);        /* id 2 */
    kprintf("[boot] server + 2 clients; starting scheduler.\n");

    scheduler_start();
}
