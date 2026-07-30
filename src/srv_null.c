/* srv_null.c — the bit bucket: writes are accepted and dropped, reads return
   end-of-file. Trivial on its own; it exists so a task can bind it over a
   path that normally reaches something real. That is the classic Plan 9 move
   — you don't silence a program by teaching it about silence, you change
   what its namespace means at that path. */
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"

void null_server(void)
{
    uputs("  [null] up (user mode), discarding whatever is bound to it\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;
        switch (r->op) {
        case VFS_OPEN:  r->result = 0;        break;
        case VFS_WRITE: r->result = r->len;   break;   /* accepted, dropped */
        case VFS_READ:  r->result = 0;        break;   /* EOF */
        case VFS_CLOSE: r->result = 0;        break;
        default:        r->result = -1;       break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
