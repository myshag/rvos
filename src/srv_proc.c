/* srv_proc.c — a third module with no disk and no device behind it at all:
   it publishes kernel state (the task table, the namespace) as readable
   files. Proof of the design claim — this module was added to a running
   system by binding "/proc/" into the namespace, and not one line of the
   kernel changed to make it possible.

     /proc/tasks       task table: id, state, name
     /proc/mounts      the namespace of whoever is asking
     /proc/pagetable   the address space of whoever is asking
     /proc/<id>/       the same two questions about a named task

   The unqualified names answer about the caller, and that is not a shortcut.
   A message carries its sender, so this server is told who is asking without
   the path having to say — which is why Linux needs /proc/self and this does
   not. The numbered directories are for the other question, the one the
   sender cannot ask about itself: what does *that* task see.
*/
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"
#include "malloc.h"

#define PROC_MAXFD 2
#define PROC_BUFSZ 4096         /* one render; the pagetable dump is the big one */

struct proc_file {
    int   used;
    int   owner;                /* so a slot can be taken back from the dead */
    int   size;
    int   pos;
    char *data;
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
static int format_pagetable(int who, char *out, int cap)
{
    int o = 0;
    int mem[2] = { 0, 0 };
    sys_meminfo(mem);

    o = append(out, o, "task ");
    o += uutoa((unsigned long)who, out + o);
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
    o += sys_pgdump(who, USTACK_TOP - PGSIZE, out + o, cap - o);
    out[o++] = '\n';
    o = append(out, o, "its heap:\n");
    o += sys_pgdump(who, UHEAP_BASE, out + o, cap - o);
    out[o++] = '\n';
    o = append(out, o, "the UART (shared):\n");
    o += sys_pgdump(who, UART_BASE_PA, out + o, cap - o);
    return o;
}

/* "/proc/1030/pagetable" -> 1030, and `rest` left pointing at "pagetable".
   Returns -1 for anything that is not a number after /proc/, which is how
   /proc/tasks and /proc/1030 are told apart without a table of names. */
static int path_task(const char *p, const char **rest)
{
    if (!ustr_has_prefix(p, "/proc/"))
        return -1;
    p += 6;
    if (*p < '0' || *p > '9')
        return -1;
    int id = 0;
    while (*p >= '0' && *p <= '9')
        id = id * 10 + (*p++ - '0');
    if (*p == '/')
        p++;
    *rest = p;
    return id;
}

/* Is this path exactly `name`, with or without a trailing slash? A directory
   answers to both spellings: one is what a person types and the other is what
   joining a name onto a prefix produces. */
static int is_name(const char *p, const char *name)
{
    int i = 0;
    while (name[i] && p[i] == name[i])
        i++;
    if (name[i])
        return 0;
    return p[i] == 0 || (p[i] == '/' && p[i + 1] == 0);
}

/* What is in /proc: three files that answer about the caller, and one
   directory per task that answers about that task. A task appears here the
   moment it exists and is gone the moment it exits — the listing is rendered
   from the table, not remembered. */
static int format_procdir(char *out, int cap)
{
    int o = append(out, 0, "- 0 tasks\n- 0 mounts\n- 0 pagetable\n");
    for (int i = 0; i < PROC_NTASK && o < cap - 24; i++) {
        struct taskinfo ti;
        if (sys_taskinfo(i, &ti) < 0)
            continue;
        o = append(out, o, "d 0 ");
        o += uutoa((unsigned long)ti.id, out + o);
        out[o++] = '\n';
    }
    return o;
}

/* Same as the filesystem's: a task that is killed never closes anything, and
   two slots is two killed readers away from a /proc that answers nothing. */
static int proc_alloc(void)
{
    for (int i = 0; i < PROC_MAXFD; i++)
        if (!p_tab[i].used)
            return i;
    for (int i = 0; i < PROC_MAXFD; i++)
        if (p_tab[i].used && !sys_alive(p_tab[i].owner)) {
            free(p_tab[i].data);
            p_tab[i].data = 0;
            p_tab[i].used = 0;
            return i;
        }
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

    f->data = malloc(PROC_BUFSZ);
    if (!f->data) { r->result = -1; return; }

    int n;
    const char *leaf;
    int who = path_task(r->path, &leaf);

    /* "/proc/" and "/proc" name the directory itself, and a directory that
       will not say what is in it is no use in a union. */
    if (is_name(r->path, "/proc"))
        n = format_procdir(f->data, PROC_BUFSZ);
    else if (who >= 0) {
        /* A task that is not there has no directory. Asking sys_alive first
           means a stale number is refused rather than answered with the state
           of whoever moved into the slot — the generation in the id is what
           makes that distinction possible. */
        if (!sys_alive(who))
            n = -1;
        else if (leaf[0] == 0)
            n = append(f->data, 0, "- 0 mounts\n- 0 pagetable\n");
        else if (ustr_has_prefix(leaf, "mounts"))
            n = sys_mounts(who, f->data, PROC_BUFSZ);
        else if (ustr_has_prefix(leaf, "pagetable"))
            n = format_pagetable(who, f->data, PROC_BUFSZ);
        else
            n = -1;
    }
    else if (ustr_has_prefix(r->path, "/proc/tasks"))
        n = format_tasks(f->data, PROC_BUFSZ);
    else if (ustr_has_prefix(r->path, "/proc/mounts"))
        n = sys_mounts(caller, f->data, PROC_BUFSZ);
    else if (ustr_has_prefix(r->path, "/proc/pagetable"))
        n = format_pagetable(caller, f->data, PROC_BUFSZ);
    else
        n = -1;

    if (n < 0) { free(f->data); f->data = 0; r->result = -1; return; }
    f->used  = 1;
    f->owner = caller;
    f->size  = n;
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
            if (r->fd >= 0 && r->fd < PROC_MAXFD) {
                free(p_tab[r->fd].data);
                p_tab[r->fd].data = 0;
                p_tab[r->fd].used = 0;
            }
            r->result = 0;
            break;
        default:
            r->result = -1;
            break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
