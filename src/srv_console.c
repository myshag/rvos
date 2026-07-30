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

void console_server(void)
{
    uputs("  [console] up (user mode), driving the UART directly\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
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
            int c = con_tryc();                 /* non-blocking */
            if (c < 0) {
                r->result = 0;
            } else {
                r->data[0] = (char)c;
                r->result = 1;
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
