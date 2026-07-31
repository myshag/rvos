/* srv_proc.c — a third module with no disk and no device behind it at all:
   it publishes kernel state (the task table, the namespace) as readable
   files. Proof of the design claim — this module was added to a running
   system by binding "/proc/" into the namespace, and not one line of the
   kernel changed to make it possible.

     /proc/tasks       task table: id, state, name, and what it waits for
     /proc/ipc         the rendezvous graph: who is holding out to whom
     /proc/<id>/doc    that task's own description of itself, if it is a
                       server — the same text /doc/<name> gives, reached
                       from the task rather than from the name
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
    int   ctl_task;             /* if this is a /proc/<id>/ctl, which id */
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

/* Why a task is not running. "blocked" was all `ps` could say, and blocked
   on what is the only interesting half: a client waiting for its reply and a
   server waiting for its next client are the same word and opposite
   situations. */
static int ipc_words(const struct taskinfo *t, char *out, int cap)
{
    (void)cap;
    int o = 0;
    switch (t->ipc) {
    case IPC_RECV:     o = append(out, o, "recv"); break;
    case IPC_RECVFROM: o = append(out, o, "recv from "); break;
    case IPC_SEND:     o = append(out, o, "send to "); break;
    case IPC_WAIT:     o = append(out, o, "wait for "); break;
    case IPC_ALARM:    o = append(out, o, "alarm"); break;
    default: break;
    }
    if (t->peer >= 0)
        o += uutoa((unsigned long)t->peer, out + o);
    if (t->ipc == IPC_SEND && t->peer < 0)
        o = append(out, o, "?");
    if (t->senders) {
        o = append(out, o, "   senders ");
        o += uutoa((unsigned long)t->senders, out + o);
    }
    out[o] = 0;
    return o;
}

/* The task table is kernel memory, so this asks for it an entry at a time
   rather than reading it. Formatting stays here: policy belongs in the
   server, the kernel only hands over the facts. */
/* As many slots as the kernel has. It was ten, which was the number of tasks
   there were when this was written, and a task in slot eleven was invisible
   to `ps` — which is a worse failure than a long listing. */
#define PROC_NTASK 18
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
        for (int p = (int)ustrlen(ti.name); p < 18; p++)
            out[o++] = ' ';
        if (ti.is_current)
            o = append(out, o, "(me)");
        else
            o += ipc_words(&ti, out + o, cap - o);
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
    int o = append(out, 0, "- 0 tasks\n- 0 mounts\n- 0 pagetable\n- 0 ipc\n");
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
/* The rendezvous graph. Every blocked task in this system is blocked on
   another task, and the pair is what you want to see when nothing is moving:
   a cycle in this list is a deadlock, and until it could be printed the only
   way to find one was to reason about it. */
static int format_ipc(char *out, int cap)
{
    int o = append(out, 0, "task                  state    holding out to\n");
    for (int i = 0; i < PROC_NTASK && o < cap - 80; i++) {
        struct taskinfo ti;
        if (sys_taskinfo(i, &ti) < 0)
            continue;
        o += uutoa((unsigned long)ti.id, out + o);
        out[o++] = ' ';
        o = append(out, o, ti.name);
        /* A program's name is its path, which is long: /BIN/FOREVER.ELF is
           seventeen characters and the column has to hold it or the line
           stops lining up exactly when there is something to read. */
        for (int p = (int)ustrlen(ti.name) + 4; p < 22; p++)
            out[o++] = ' ';
        o = append(out, o, state_name(ti.state));
        for (int p = (int)ustrlen(state_name(ti.state)); p < 9; p++)
            out[o++] = ' ';
        o += ipc_words(&ti, out + o, cap - o);
        if (ti.ipc == IPC_SEND && ti.msglen) {
            o = append(out, o, "  (");
            o += uutoa((unsigned long)ti.msglen, out + o);
            o = append(out, o, " bytes)");
        }
        out[o++] = '\n';
    }
    return o;
}

/* One task's half of it, and who is queued waiting for it. The queue is the
   part `ps` could never show: a server with four clients standing behind it
   looks exactly like a server with none. */
static int format_task_ipc(int who, char *out, int cap)
{
    for (int i = 0; i < PROC_NTASK; i++) {
        struct taskinfo ti;
        if (sys_taskinfo(i, &ti) < 0 || ti.id != who)
            continue;
        int o = append(out, 0, "task     ");
        o += uutoa((unsigned long)ti.id, out + o);
        out[o++] = ' ';
        o = append(out, o, ti.name);
        o = append(out, o, "\nstate    ");
        o = append(out, o, state_name(ti.state));
        o = append(out, o, "\nwaiting  ");
        int k = ipc_words(&ti, out + o, cap - o);
        o += k;
        if (!k)
            o = append(out, o, "nothing");
        o = append(out, o, "\nsenders  ");
        o += uutoa((unsigned long)ti.senders, out + o);
        o = append(out, o, "\n");

        /* And the other direction: who is standing in its queue. The kernel
           does not record it on the sender, so this is read off every task
           that says it is sending to this one. */
        for (int j = 0; j < PROC_NTASK && o < cap - 48; j++) {
            struct taskinfo s2;
            if (sys_taskinfo(j, &s2) < 0)
                continue;
            if (s2.ipc == IPC_SEND && s2.peer == who) {
                o = append(out, o, "  <- ");
                o += uutoa((unsigned long)s2.id, out + o);
                out[o++] = ' ';
                o = append(out, o, s2.name);
                o = append(out, o, " is holding out ");
                o += uutoa((unsigned long)s2.msglen, out + o);
                o = append(out, o, " bytes\n");
            }
        }
        return o;
    }
    return -1;
}

static const char proc_conf[] =
    "open files   2    each a rendering, made at open\n"
    "buffer       4096 bytes, allocated per open\n"
    "tasks shown  18   as many slots as the kernel has\n";

static const char proc_doc[] =
"proc — kernel state as files, and nothing behind it.\n"
                    "\n"
                    "tasks       id, state, name, and what each is waiting for\n"
                    "ipc         the rendezvous graph. A cycle in it is a\n"
                    "            deadlock.\n"
                    "mounts      the namespace of whoever is asking\n"
                    "pagetable   the address space of whoever is asking\n"
                    "<id>/       the same two questions about a named task,\n"
                    "            plus what it waits for and what can be done\n"
                    "            to it\n"
                    "<id>/ctl    write `kill`\n"
                    "<id>/doc    that task's own words, if it is a server. Its\n"
                    "            presence is the shortest way to ask whether\n"
                    "            this task answers for a name at all.\n"
                    "\n"
                    "The unqualified names answer about the caller, because a\n"
                    "message carries its sender: this server is told who is\n"
                    "asking without the path having to say. That is why there\n"
                    "is no /proc/self here.\n"
                    "\n"
                    "A file is rendered when it is opened, so its size is not\n"
                    "known until then and a listing reports 0 for all of them.\n"
                    "\n"
                    "ioctl     GETSIZE, PING, DOC\n";

/* Is that task a server somebody has mounted?

   The question has to be asked before sending it anything, and not out of
   politeness. A message to a task that is not a server is delivered into
   whatever it is doing — and if that task is blocked in a closed receive
   waiting for its own reply, the message queues behind and is never taken,
   so the asker waits for an answer that cannot come. The mount table is the
   list of tasks that have agreed to answer questions of this shape. */
static int is_mounted_server(int id)
{
    char mounts[512];
    int n = sys_mounts(-1, mounts, (int)sizeof(mounts));
    for (int i = 0; i < n; ) {
        int s = i;
        while (i < n && mounts[i] != '\n') i++;
        int e = i;
        if (i < n) i++;
        int a = -1;
        for (int k = s; k + 8 < e; k++)
            if (mounts[k] == '-' && mounts[k+1] == '>' && mounts[k+3] == 't' &&
                mounts[k+4] == 'a' && mounts[k+5] == 's' && mounts[k+6] == 'k')
                a = k + 8;
        if (a < 0)
            continue;                   /* a bind names a name, not a server */
        int got = 0;
        for (int k = a; k < e && mounts[k] >= '0' && mounts[k] <= '9'; k++)
            got = got * 10 + (mounts[k] - '0');
        if (got == id)
            return 1;
    }
    return 0;
}

/* Ask every mounted server the same question about one task, and put the
   answers end to end. This is what having no descriptor table costs and what
   it buys: nobody has to keep the list in step, and finding it out is a
   question asked of everybody who might know. */
static int ask_all(unsigned long cmd, int about, char *out, int cap)
{
    char mounts[512];
    int n = sys_mounts(-1, mounts, (int)sizeof(mounts));
    int o = 0;

    for (int i = 0; i < n && o < cap - VFS_DATA_MAX - 32; ) {
        int s = i;
        while (i < n && mounts[i] != '\n') i++;
        int e = i;
        if (i < n) i++;

        int a = -1;
        for (int k = s; k + 8 < e; k++)
            if (mounts[k] == '-' && mounts[k+1] == '>' && mounts[k+3] == 't' &&
                mounts[k+4] == 'a' && mounts[k+5] == 's' && mounts[k+6] == 'k')
                a = k + 8;
        if (a < 0)
            continue;
        int id = 0;
        for (int k = a; k < e && mounts[k] >= '0' && mounts[k] <= '9'; k++)
            id = id * 10 + (mounts[k] - '0');

        /* One question per server, however many names it answers for. */
        int already = 0;
        for (int k = 0; k < s; ) {
            int t0 = k;
            while (k < n && mounts[k] != '\n') k++;
            int t1 = k;
            if (k < n) k++;
            int a2 = -1;
            for (int q = t0; q + 8 < t1; q++)
                if (mounts[q] == '-' && mounts[q+1] == '>' &&
                    mounts[q+3] == 't' && mounts[q+4] == 'a')
                    a2 = q + 8;
            if (a2 < 0) continue;
            int id2 = 0;
            for (int q = a2; q < t1 && mounts[q] >= '0' && mounts[q] <= '9'; q++)
                id2 = id2 * 10 + (mounts[q] - '0');
            if (id2 == id) already = 1;
        }
        if (already || id == sys_self())
            continue;                   /* itself is asked by the caller */

        struct vfs_req q;
        q.op = VFS_IOCTL;
        q.fd = -1;
        q.ioctl_cmd = cmd;
        q.len = about;
        q.path[0] = 0;
        if (vfs_call(id, &q) <= 0)
            continue;
        int got = q.result;
        if (got > cap - o - 1)
            got = cap - o - 1;
        if (got > 0) {
            umemcpy(out + o, q.data, (unsigned long)got);
            o += got;
        }
    }
    return o;
}

/* That task's own words about itself, fetched a message at a time. The same
   text /doc/<name> gives, reached from the task rather than from the name —
   which is what a namespace is for. */
static int fetch_paged(int id, unsigned long cmd, const char *mine,
                       char *out, int cap)
{
    if (id == sys_self())
        return append(out, 0, mine);        /* nobody can ask themselves */
    if (!is_mounted_server(id))
        return -1;

    int o = 0;
    for (;;) {
        struct vfs_req q;
        q.op = VFS_IOCTL;
        q.fd = -1;
        q.ioctl_cmd = cmd;
        q.len = o;                      /* how far we have got */
        q.path[0] = 0;
        if (vfs_call(id, &q) <= 0)
            break;
        int n = q.result;
        if (n > cap - o - 1)
            n = cap - o - 1;
        if (n <= 0)
            break;
        umemcpy(out + o, q.data, (unsigned long)n);
        o += n;
    }
    if (!o)
        o = append(out, 0, "this server does not answer that\n");
    return o;
}

/* What that task has open, asked of everybody who might be holding it. */
static int format_fd(int who, char *out, int cap)
{
    int o = ask_all(IOCTL_HOLDS, who, out, cap);
    /* And this server's own, which cannot be asked for by message. */
    for (int i = 0; i < PROC_MAXFD && o < cap - 32; i++)
        if (p_tab[i].used && p_tab[i].owner == who)
            o = append(out, o, "a rendering of /proc\n");
    if (!o)
        o = append(out, 0, "nothing\n");
    return o;
}

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
    f->ctl_task = 0;

    int n;
    const char *leaf = "";      /* path_task only sets it when it finds one */
    int who = path_task(r->path, &leaf);

    /* "/proc/" and "/proc" name the directory itself, and a directory that
       will not say what is in it is no use in a union. */
    if (is_name(r->path, "/proc"))
        n = format_procdir(f->data, PROC_BUFSZ);
    else if (ustr_has_prefix(r->path, "/proc/ipc"))
        n = format_ipc(f->data, PROC_BUFSZ);
    else if (who >= 0) {
        /* A task that is not there has no directory. Asking sys_alive first
           means a stale number is refused rather than answered with the state
           of whoever moved into the slot — the generation in the id is what
           makes that distinction possible. */
        if (!sys_alive(who))
            n = -1;
        else if (leaf[0] == 0) {
            n = append(f->data, 0, "- 0 mounts\n- 0 pagetable\n- 0 ipc\n"
                                   "- 0 ctl\n- 0 fd\n");
            /* Only a task that answers for a name has anything to say about
               itself, and its presence here is the shortest way to ask
               whether this task is a server at all. */
            if (who == sys_self() || is_mounted_server(who))
                n = append(f->data, n, "- 0 doc\n- 0 conf\n");
        }
        else if (ustr_has_prefix(leaf, "doc"))
            n = fetch_paged(who, IOCTL_DOC, proc_doc, f->data, PROC_BUFSZ);
        else if (ustr_has_prefix(leaf, "conf"))
            n = fetch_paged(who, IOCTL_CONF, proc_conf, f->data, PROC_BUFSZ);
        else if (ustr_has_prefix(leaf, "fd"))
            n = format_fd(who, f->data, PROC_BUFSZ);
        else if (ustr_has_prefix(leaf, "ipc"))
            n = format_task_ipc(who, f->data, PROC_BUFSZ);
        else if (ustr_has_prefix(leaf, "ctl")) {
            f->ctl_task = who;
            n = append(f->data, 0, "kill\n");   /* what it will accept */
        }
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
        /* A control file: write `kill` to it and the task is gone. Plan 9
           spells it exactly this way, and it is worth spelling the same,
           because it turns a syscall into a name — Ctrl-C nominates a task to
           the terminal, and this is the same act done deliberately, by
           whoever can write the file. There is no permission on it, which is
           the same hole SYS_KILL has and is written down in both places. */
        case VFS_WRITE: {
            if (r->fd < 0 || r->fd >= PROC_MAXFD || !p_tab[r->fd].used) {
                r->result = -1;
                break;
            }
            struct proc_file *f = &p_tab[r->fd];
            if (f->ctl_task <= 0) {
                r->result = -1;         /* not a control file */
                break;
            }
            if (ustr_has_prefix(r->data, "kill"))
                r->result = sys_kill(f->ctl_task) == 0 ? r->len : -1;
            else
                r->result = -1;
            break;
        }
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
            else if (r->ioctl_cmd == IOCTL_DOC)
                r->result = vfs_doc_reply(r, proc_doc);
            else if (r->ioctl_cmd == IOCTL_CONF)
                r->result = vfs_doc_reply(r, proc_conf);
            else if (r->ioctl_cmd == IOCTL_HOLDS) {
                char t[VFS_DATA_MAX];
                int o = 0;
                for (int i = 0; i < PROC_MAXFD; i++)
                    if (p_tab[i].used && p_tab[i].owner == r->len)
                        o = append(t, o, "a rendering of /proc\n");
                t[o] = 0;
                r->result = vfs_reply_text(r, t);
            }
            else if (r->ioctl_cmd == IOCTL_GETSIZE &&
                r->fd >= 0 && r->fd < PROC_MAXFD && p_tab[r->fd].used)
                r->result = p_tab[r->fd].size;
            else
                r->result = -1;
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
