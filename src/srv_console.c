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

/* Whom to kill if the interrupt character arrives, or -1 for nobody. A shell
   nominates the task it is waiting for and takes the nomination back
   afterwards; this server does not know what a shell is and does not need to.
   Unix would call this the foreground process group, and would send it a
   signal that it could catch. There are no signals here. */
#define CON_INTR 3                      /* ETX: what Ctrl-C sends */
static int intr_task = -1;
static int intr_fired;

/* Two things are open-able here and they could not be less alike: the console
   itself, and the directory it lives in. Reading the first waits for a key;
   reading the second must not, or `ls /dev` hangs the shell until somebody
   presses one — which is what it did. */
#define CON_FD_TTY 0
#define CON_FD_DIR 1

static const char con_dir[] = "- 0 console\n";
static int con_dir_pos;

/* "/dev" and "/dev/" name the directory; anything else under it is the
   console, because there is nothing else under it. */
static int is_dev_dir(const char *p)
{
    return p[0] == '/' && p[1] == 'd' && p[2] == 'e' && p[3] == 'v' &&
           (p[4] == 0 || (p[4] == '/' && p[5] == 0));
}

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
            while ((c = con_tryc()) >= 0) { /* drain: the FIFO may hold several */
                /* The interrupt character is acted on rather than delivered:
                   a program being killed is not going to read it. */
                if (c == CON_INTR && intr_task >= 0) {
                    if (sys_alive(intr_task))
                        sys_kill(intr_task);
                    intr_task  = -1;
                    intr_fired = 1;
                    uputs("^C\n");
                    continue;
                }
                ring_put((char)c);
            }
            sys_irq_ack(UART0_IRQ);
            /* A reader that is no longer there must not be answered: the
               reply would either fail or, without generation-tagged ids, land
               on whatever task moved into its slot. */
            if (waiter >= 0 && !sys_alive(waiter))
                waiter = -1;
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
            if (is_dev_dir(r->path)) {
                con_dir_pos = 0;
                r->result = CON_FD_DIR;
            } else {
                r->result = CON_FD_TTY;         /* one stream, local fd 0 */
            }
            break;
        case VFS_WRITE: {
            for (int i = 0; i < r->len && i < VFS_DATA_MAX; i++)
                con_putc(r->data[i]);
            r->result = r->len;
            break;
        }
        case VFS_READ: {
            if (r->fd == CON_FD_DIR) {
                int n = (int)sizeof(con_dir) - 1 - con_dir_pos;
                if (n > r->len) n = r->len;
                if (n < 0) n = 0;
                for (int i = 0; i < n; i++)
                    r->data[i] = con_dir[con_dir_pos + i];
                con_dir_pos += n;
                r->result = n;
                break;
            }
            int c = ring_get();
            if (c >= 0) {
                r->data[0] = (char)c;
                r->result = 1;
            } else if (waiter < 0 || !sys_alive(waiter)) {
                waiter  = from;                 /* keep it; answer on a key */
                waiting = *r;
                continue;                       /* no reply, on purpose */
            } else {
                r->result = 0;                  /* somebody is already waiting */
            }
            break;
        }
        case VFS_IOCTL:
            /* A ping is about this server, not about a file, so it is
               answered before the descriptor is even looked at. */
            if (r->ioctl_cmd == IOCTL_PING) {
                r->result = 0;
            } else if (r->ioctl_cmd == IOCTL_DOC) {
                r->result = vfs_doc_reply(r,
                    "console — the UART, driven from user mode.\n"
                    "\n"
                    "/dev/console  read one keystroke, write bytes.\n"
                    "/dev          the directory it lives in, which lists\n"
                    "              console and nothing else. Opening it does\n"
                    "              not open the keyboard: reading a directory\n"
                    "              must not wait for a key.\n"
                    "\n"
                    "read      answers when a key arrives and not before. The\n"
                    "          request is kept, not refused — blocking here is\n"
                    "          a server declining to answer yet, and the kernel\n"
                    "          never learns the word. One waiter; a second\n"
                    "          reader is told 0.\n"
                    "write     bytes to the line, LF becoming CR LF.\n"
                    "\n"
                    "ioctl     INTR  a task id: if the interrupt character\n"
                    "                arrives, kill that task. 0 takes the\n"
                    "                nomination back and answers whether it\n"
                    "                went off. This is what a tty does with\n"
                    "                SIGINT, without signals or groups.\n"
                    "          PING, DOC\n");
            } else if (r->ioctl_cmd == IOCTL_INTR) {
                /* Clearing the nomination answers whether it went off. */
                r->result  = r->len > 0 ? 0 : intr_fired;
                intr_task  = r->len > 0 ? r->len : -1;
                intr_fired = 0;
            } else {
                r->result = -1;
            }
            break;
        case VFS_CLOSE: r->result = 0;  break;
        default:        r->result = -1; break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
