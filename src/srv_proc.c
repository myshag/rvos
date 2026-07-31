/* srv_proc.c — a third module with no disk and no device behind it at all:
   it publishes kernel state (the task table, the namespace) as readable
   files. Proof of the design claim — this module was added to a running
   system by binding "/proc/" into the namespace, and not one line of the
   kernel changed to make it possible.

     /proc/tasks   task table: id, state, name
     /proc/mounts  the namespace itself, as text
*/
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"

#define PROC_MAXFD 2
#define PROC_BUFSZ 1024

struct proc_file {
    int  used;
    int  size;
    int  pos;
    char data[PROC_BUFSZ];
};
static struct proc_file p_tab[PROC_MAXFD];

/* Mirrors enum task_state; the server no longer sees the kernel's header. */
static const char *state_name(int s)
{
    switch (s) {
    case 1:  return "runnable";
    case 2:  return "running";
    case 3:  return "blocked";
    default: return "?";
    }
}

static int append(char *out, int o, const char *s)
{
    int l = (int)ustrlen(s);
    umemcpy(out + o, s, (unsigned long)l);
    return o + l;
}

/* The task table is kernel memory, so this asks for it an entry at a time
   rather than reading it. Formatting stays here: policy belongs in the
   server, the kernel only hands over the facts. */
#define PROC_NTASK 10
static int format_tasks(char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < PROC_NTASK && o < cap - 48; i++) {
        struct taskinfo ti;
        if (sys_taskinfo(i, &ti) < 0)
            continue;
        o += uutoa((unsigned long)ti.id, out + o);
        o = append(out, o, "  ");
        o = append(out, o, state_name(ti.state));
        for (int p = ustrlen(state_name(ti.state)); p < 9; p++)
            out[o++] = ' ';
        o = append(out, o, ti.name);
        if (ti.is_current)
            o = append(out, o, "  (me)");
        out[o++] = '\n';
    }
    return o;
}

/* Rendered in the *caller's* address space, not the server's — with a page
   table per task there is no single "the" page table to report, the same way
   there is no single namespace. Two tasks reading this file see their own
   stack land on different physical pages, and see the disk mapped or not
   depending on whether they are the filesystem. */
static int format_pagetable(int caller, char *out, int cap)
{
    int o = 0;
    int mem[2] = { 0, 0 };
    sys_meminfo(mem);

    o = append(out, o, "task ");
    o += uutoa((unsigned long)caller, out + o);
    out[o++] = '\n';
    o = append(out, o, "free pages ");
    o += uutoa((unsigned long)mem[0], out + o);
    o = append(out, o, " of ");
    o += uutoa((unsigned long)mem[1], out + o);
    out[o++] = '\n';
    out[o++] = '\n';

    /* sys_pgdump, not a direct walk: the page-table pages live in the arena,
       which this server has no mapping for. Asking the kernel is now the only
       way to see a translation. */
    o = append(out, o, "its stack:\n");
    o += sys_pgdump(caller, USTACK_TOP - PGSIZE, out + o, cap - o);
    out[o++] = '\n';
    o = append(out, o, "the UART (shared):\n");
    o += sys_pgdump(caller, UART_BASE_PA, out + o, cap - o);
    out[o++] = '\n';
    o = append(out, o, "the FAT16 image:\n");
    o += sys_pgdump(caller, DISK_PA, out + o, cap - o);
    return o;
}

static int proc_alloc(void)
{
    for (int i = 0; i < PROC_MAXFD; i++)
        if (!p_tab[i].used)
            return i;
    return -1;
}

/* `caller` matters: a namespace belongs to a task, so "the mount table" is
   not a thing this server can look up on its own — it must render the
   caller's. Two tasks reading /proc/mounts legitimately get different text. */
static void proc_do_open(struct vfs_req *r, int caller)
{
    int fd = proc_alloc();
    if (fd < 0) { r->result = -1; return; }
    struct proc_file *f = &p_tab[fd];

    int n;
    /* "/proc/" and "/proc" name the directory itself, and a directory that
       will not say what is in it is no use in a union. */
    if (r->path[5] == 0 || r->path[6] == 0)
        n = append(f->data, 0, "tasks\nmounts\npagetable\n");
    else if (ustr_has_prefix(r->path, "/proc/tasks"))
        n = format_tasks(f->data, PROC_BUFSZ);
    else if (ustr_has_prefix(r->path, "/proc/mounts"))
        n = sys_mounts(caller, f->data, PROC_BUFSZ);
    else if (ustr_has_prefix(r->path, "/proc/pagetable"))
        n = format_pagetable(caller, f->data, PROC_BUFSZ);
    else
        n = -1;

    if (n < 0) { r->result = -1; return; }
    f->used = 1;
    f->size = n;
    f->pos  = 0;
    r->result = fd;
}

void proc_server(void)
{
    uputs("  [proc] up (user mode), publishing kernel state as files\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;
        switch (r->op) {
        case VFS_OPEN:
            proc_do_open(r, from);
            break;
        case VFS_READ: {
            if (r->fd < 0 || r->fd >= PROC_MAXFD || !p_tab[r->fd].used) {
                r->result = -1;
                break;
            }
            struct proc_file *f = &p_tab[r->fd];
            int n = f->size - f->pos;
            if (n > r->len) n = r->len;
            if (n < 0) n = 0;
            umemcpy(r->data, f->data + f->pos, (unsigned long)n);
            f->pos += n;
            r->result = n;
            break;
        }
        case VFS_IOCTL:
            /* A ping is about this server, not about a file, so it is
               answered before the descriptor is even looked at. */
            if (r->ioctl_cmd == IOCTL_PING)
                r->result = 0;
            else if (r->ioctl_cmd == IOCTL_GETSIZE &&
                r->fd >= 0 && r->fd < PROC_MAXFD && p_tab[r->fd].used)
                r->result = p_tab[r->fd].size;
            else
                r->result = -1;
            break;
        case VFS_WRITE:
            r->result = -1;                     /* read-only view */
            break;
        case VFS_CLOSE:
            if (r->fd >= 0 && r->fd < PROC_MAXFD)
                p_tab[r->fd].used = 0;
            r->result = 0;
            break;
        default:
            r->result = -1;
            break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
