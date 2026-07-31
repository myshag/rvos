/* srv_console.c — console module. Same vfs_req protocol as the filesystem,
   but the backend is the UART rather than a disk: write() emits bytes,
   read() returns a keystroke if one is waiting. Having two modules with
   nothing in common answer the identical interface is the whole point — the
   interface is abstract, not filesystem-shaped. */
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"

/* A user-space device driver: the UART is mapped into this address space and
   nobody else's, so these are ordinary loads and stores from an unprivileged
   program — no syscall, no kernel involvement on the data path. */
#define UART ((volatile unsigned char *)0x10000000UL)
enum { UART_RBR = 0, UART_THR = 0, UART_LSR = 5 };
#define UART_LSR_THRE (1u << 5)
#define UART_LSR_DR   (1u << 0)

static void con_putc(char c)
{
    if (c == '\n')
        con_putc('\r');
    while (!(UART[UART_LSR] & UART_LSR_THRE))
        ;
    UART[UART_THR] = (unsigned char)c;
}

static int con_tryc(void)
{
    if (!(UART[UART_LSR] & UART_LSR_DR))
        return -1;
    return (int)UART[UART_RBR];
}

/* Keystrokes arrive on an interrupt and are parked here until somebody
   read()s them. Without this the data would be lost: the interrupt must be
   serviced promptly, but the reader may not have asked yet. */
#define RING 256
static char ring[RING];
static int  rhead, rtail;

static void ring_put(char c)
{
    int next = (rhead + 1) % RING;
    if (next != rtail) {          /* drop on overflow rather than overwrite */
        ring[rhead] = c;
        rhead = next;
    }
}

static int ring_get(void)
{
    if (rtail == rhead)
        return -1;
    int c = (unsigned char)ring[rtail];
    rtail = (rtail + 1) % RING;
    return c;
}

/* A read that arrived when nothing had been typed. The driver has been
   interrupt-driven since stage 14 but its clients were not: the shell spun,
   asking again and again, because the server always answered at once —
   with 0 when there was nothing to say.

   Answering later costs nothing and needs no new mechanism. The caller is
   already blocked in the sys_recv that follows its sys_send, so the server
   simply keeps the request and replies when a key arrives. Blocking I/O is a
   server declining to answer yet; the kernel never learns the word.

   One waiter, because two programs sharing a keyboard is not a question this
   system has to answer; a second reader is told 0 as before. */
static int             waiter = -1;
static struct vfs_req  waiting;

void console_server(void)
{
    uputs("  [console] up (user mode), UART driven by interrupts\n");
    sys_irq_register(UART0_IRQ);

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));

        /* The same receive loop takes both client requests and interrupts;
           the kernel distinguishes them by the sender it reports. */
        if (from == IRQ_SENDER) {
            int c;
            while ((c = con_tryc()) >= 0)   /* drain: the FIFO may hold several */
                ring_put((char)c);
            sys_irq_ack(UART0_IRQ);
            if (waiter >= 0) {
                int k = ring_get();
                if (k >= 0) {
                    waiting.data[0] = (char)k;
                    waiting.result  = 1;
                    int to = waiter;
                    waiter = -1;
                    sys_send(to, &waiting, (int)sizeof(waiting));
                }
            }
            continue;
        }

        struct vfs_req *r = &req;
        switch (r->op) {
        case VFS_OPEN:
            r->result = 0;                      /* one stream, local fd 0 */
            break;
        case VFS_WRITE: {
            for (int i = 0; i < r->len && i < VFS_DATA_MAX; i++)
                con_putc(r->data[i]);
            r->result = r->len;
            break;
        }
        case VFS_READ: {
            int c = ring_get();
            if (c >= 0) {
                r->data[0] = (char)c;
                r->result = 1;
            } else if (waiter < 0) {
                waiter  = from;                 /* keep it; answer on a key */
                waiting = *r;
                continue;                       /* no reply, on purpose */
            } else {
                r->result = 0;                  /* somebody is already waiting */
            }
            break;
        }
        case VFS_IOCTL: r->result = -1; break;  /* no tty commands yet */
        case VFS_CLOSE: r->result = 0;  break;
        default:        r->result = -1; break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
