/* srv_console.c — console module. Same vfs_req protocol as the filesystem,
   but the backend is the UART rather than a disk: write() emits bytes,
   read() returns a keystroke if one is waiting. Having two modules with
   nothing in common answer the identical interface is the whole point — the
   interface is abstract, not filesystem-shaped. */
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "uart.h"

void console_server(void)
{
    kprintf("  [console] up, serving open/read/write/close over UART\n");

    for (;;) {
        uint64 m;
        int from = sys_recv(&m);
        struct vfs_req *r = (struct vfs_req *)m;
        switch (r->op) {
        case VFS_OPEN:
            r->result = 0;                      /* one stream, local fd 0 */
            break;
        case VFS_WRITE: {
            const char *p = (const char *)r->buf;
            for (int i = 0; i < r->len; i++)
                uart_putc(p[i]);
            r->result = r->len;
            break;
        }
        case VFS_READ: {
            int c = uart_tryc();                /* non-blocking */
            if (c < 0) {
                r->result = 0;
            } else {
                ((char *)r->buf)[0] = (char)c;
                r->result = 1;
            }
            break;
        }
        case VFS_IOCTL: r->result = -1; break;  /* no tty commands yet */
        case VFS_CLOSE: r->result = 0;  break;
        default:        r->result = -1; break;
        }
        sys_send(from, 0);
    }
}
