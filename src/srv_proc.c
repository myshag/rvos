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
#include "task.h"
#include "syscall.h"
#include "uart.h"
#include "util.h"
#include "vm.h"
#include "pmm.h"

#define PROC_MAXFD 2
#define PROC_BUFSZ 1024

struct proc_file {
    int  used;
    int  size;
    int  pos;
    char data[PROC_BUFSZ];
};
static struct proc_file p_tab[PROC_MAXFD];

static const char *state_name(enum task_state s)
{
    switch (s) {
    case T_UNUSED:   return "unused";
    case T_RUNNABLE: return "runnable";
    case T_RUNNING:  return "running";
    case T_BLOCKED:  return "blocked";
    default:         return "?";
    }
}

static int append(char *out, int o, const char *s)
{
    int l = (int)strlen(s);
    memcpy(out + o, s, (size_t)l);
    return o + l;
}

static int format_tasks(char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < NTASK && o < cap - 48; i++) {
        if (tasks[i].state == T_UNUSED)
            continue;
        o += utoa((unsigned long)tasks[i].id, out + o);
        o = append(out, o, "  ");
        o = append(out, o, state_name(tasks[i].state));
        /* pad so the names line up regardless of state word length */
        for (int p = (int)strlen(state_name(tasks[i].state)); p < 9; p++)
            out[o++] = ' ';
        o = append(out, o, tasks[i].name);
        if (&tasks[i] == current)
            o = append(out, o, "  (me)");
        out[o++] = '\n';
    }
    return o;
}

/* Three translations worth seeing: an MMIO page mapped at 4 KiB (the walk
   runs all three levels), a kernel address covered by a 2 MiB superpage (the
   walk stops one level early), and the read-only demo page. */
static int format_pagetable(char *out, int cap)
{
    int o = 0;
    o = append(out, o, "satp mode Sv39, root table 0x");
    o += xtoa((r_satp() & 0xfffffffffffULL) << 12, out + o);
    out[o++] = '\n';
    o = append(out, o, "free pages ");
    o += utoa((unsigned long)pmm_free_count(), out + o);
    o = append(out, o, " of ");
    o += utoa((unsigned long)pmm_total_count(), out + o);
    out[o++] = '\n';
    out[o++] = '\n';

    o += vm_dump_walk(UART_BASE_PA, out + o, cap - o);
    out[o++] = '\n';
    o += vm_dump_walk(RAM_BASE + 0x1000, out + o, cap - o);
    out[o++] = '\n';
    o += vm_dump_walk(0x40000000UL, out + o, cap - o);
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
    if (str_has_prefix(r->path, "/proc/tasks"))
        n = format_tasks(f->data, PROC_BUFSZ);
    else if (str_has_prefix(r->path, "/proc/mounts"))
        n = vfs_dump_mounts_of(caller, f->data, PROC_BUFSZ);
    else if (str_has_prefix(r->path, "/proc/pagetable"))
        n = format_pagetable(f->data, PROC_BUFSZ);
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
    kprintf("  [proc] up, publishing kernel state as files\n");

    for (;;) {
        uint64 m;
        int from = sys_recv(&m);
        struct vfs_req *r = (struct vfs_req *)m;
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
            memcpy(r->buf, f->data + f->pos, (size_t)n);
            f->pos += n;
            r->result = n;
            break;
        }
        case VFS_IOCTL:
            if (r->ioctl_cmd == IOCTL_GETSIZE &&
                r->fd >= 0 && r->fd < PROC_MAXFD && p_tab[r->fd].used) {
                *(uint32 *)r->ioctl_arg = (uint32)p_tab[r->fd].size;
                r->result = 0;
            } else {
                r->result = -1;
            }
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
        sys_send(from, 0);
    }
}
