/* srv_dev.c — what the machine is, as files.

   The kernel asks the board to describe itself and walks the PCI bus; this
   publishes the answers. It is the fourth module with nothing behind it but
   kernel state — after /proc, which does the same for tasks and namespaces —
   and it exists for the same reason: a thing that can only be printed at boot
   is a thing you cannot ask about afterwards.

     /dev/tree        the flattened device tree, rendered
     /dev/pci/        one directory entry per function on the bus
     /dev/pci/00:01.0 that function: vendor, class, bars, irq, driver
     /dev/console     the console server, mounted over this one
     /doc/            one file per server, which that server writes itself
     /doc/net         what /net/ctl accepts, in /net's own words

   That last line is the arrangement worth explaining. Two servers answer for
   names under /dev, and there is no union: the console is mounted at the
   exact name /dev/console, this server at the prefix /dev/, and resolution
   takes the longest match. So the console keeps its own name and everything
   else under /dev comes here. `ls /dev` shows both, because the client adds
   the mount points in a directory to whatever the serving directory said. */
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"
#include "malloc.h"

#define DEV_MAXFD 2

struct dev_file {
    int   used;
    int   owner;                /* so a slot can be taken back from the dead */
    int   size;
    int   pos;
    char *data;
};
static struct dev_file d_tab[DEV_MAXFD];

static int dev_alloc(int from)
{
    for (int i = 0; i < DEV_MAXFD; i++)
        if (!d_tab[i].used)
            return i;
    /* Reclaim from a task that is gone: a killed reader never closes. */
    for (int i = 0; i < DEV_MAXFD; i++)
        if (d_tab[i].used && !sys_alive(d_tab[i].owner)) {
            free(d_tab[i].data);
            d_tab[i].data = 0;
            d_tab[i].used = 0;
            return i;
        }
    (void)from;
    return -1;
}

static int is_name(const char *p, const char *name)
{
    int i = 0;
    while (name[i] && p[i] == name[i])
        i++;
    if (name[i])
        return 0;
    return p[i] == 0 || (p[i] == '/' && p[i + 1] == 0);
}

static int append(char *out, int o, const char *s)
{
    int l = (int)ustrlen(s);
    umemcpy(out + o, s, (unsigned long)l);
    return o + l;
}

/* The bus, as a directory. One entry per function, named the way every tool
   that has ever printed a PCI address names it. */
static int format_pci_dir(char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < 64 && o < cap - 32; i++) {
        char name[16];
        if (sys_devinfo(DEVINFO_NAME, i, name, (int)sizeof(name)) <= 0)
            break;
        o = append(out, o, "- 0 ");
        o = append(out, o, name);
        out[o++] = '\n';
    }
    return o;
}

/* "00:01.0" back to the index it was found at. The kernel keeps the array;
   this server keeps nothing, so it asks until the names match. */
static int pci_index_of(const char *leaf)
{
    for (int i = 0; i < 64; i++) {
        char name[16];
        if (sys_devinfo(DEVINFO_NAME, i, name, (int)sizeof(name)) <= 0)
            break;
        int k = 0;
        while (name[k] && name[k] == leaf[k])
            k++;
        if (!name[k] && !leaf[k])
            return i;
    }
    return -1;
}

static const char dev_doc[] =
"dev — what the board said it is, and what the others say\n"
                    "      about themselves.\n"
                    "\n"
                    "/dev/tree        the flattened device tree the machine\n"
                    "                 handed the kernel in a1 at boot,\n"
                    "                 rendered: where each thing is and which\n"
                    "                 interrupt it raises\n"
                    "/dev/pci/        one name per function on the bus\n"
                    "/dev/pci/<b:d.f> vendor, device, class, bars, irq, and\n"
                    "                 which driver has it — none of them, so\n"
                    "                 far\n"
                    "/doc/            one name per mounted server\n"
                    "/doc/<server>    that server's own answer to IOCTL_DOC.\n"
                    "                 Nothing here knows what any of it says;\n"
                    "                 this only asks and pages.\n"
                    "\n"
                    "/dev/console is not here: the console is mounted at that\n"
                    "exact name and this server at the prefix, and resolution\n"
                    "takes the longest match.\n"
                    "\n"
                    "ioctl     GETSIZE, PING, DOC\n";

/* ---- the documentation, which is not written down anywhere -------------

   A server answers IOCTL_DOC with a description of itself: the ioctls it
   takes, the words its control file accepts, what its names mean. This
   collects them. Nothing here knows what any of it says.

   That is the whole point of doing it this way rather than putting text
   files on the disk. Documentation that lives beside the thing it describes
   goes stale silently; documentation that *is* the thing answering cannot.
   A server that has no answer has no entry — which is a better failure than
   an entry that is wrong.

   The list of servers comes from the mount table, because a server nobody
   has mounted is a server nobody can ask. */
static int server_task(int idx, char *name, int cap)
{
    char mounts[512];
    int n = sys_mounts(-1, mounts, (int)sizeof(mounts));
    int seen = 0;

    for (int i = 0; i < n; ) {
        int s = i;
        while (i < n && mounts[i] != '\n') i++;
        int e = i;
        if (i < n) i++;

        /* "prefix -> task N", and only that form: a bind names another name
           and has no server of its own to ask. */
        int arrow = -1;
        for (int k = s; k + 8 < e; k++)
            if (mounts[k] == '-' && mounts[k+1] == '>' && mounts[k+3] == 't' &&
                mounts[k+4] == 'a' && mounts[k+5] == 's' && mounts[k+6] == 'k')
                arrow = k + 8;
        if (arrow < 0)
            continue;
        int id = 0;
        for (int k = arrow; k < e && mounts[k] >= '0' && mounts[k] <= '9'; k++)
            id = id * 10 + (mounts[k] - '0');

        /* One entry per server, however many names it answers for: /dev/ and
           /dev/console are two mounts and one console. Rather than keep a
           list of what has been seen, walk the lines before this one again —
           this runs once, when a directory of five entries is opened. */
        int already = 0;
        {
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
        }
        if (already)
            continue;

        if (seen++ != idx)
            continue;

        /* The name is the task's, which is what a person would call it. */
        for (int j = 0; j < 24; j++) {
            struct taskinfo ti;
            if (sys_taskinfo(j, &ti) < 0)
                continue;
            if (ti.id != id)
                continue;
            int k = 0;
            while (k < cap - 1 && ti.name[k]) { name[k] = ti.name[k]; k++; }
            name[k] = 0;
            return id;
        }
        return -1;
    }
    return -1;
}

static int format_doc_dir(char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < 16 && o < cap - 32; i++) {
        char name[24];
        if (server_task(i, name, (int)sizeof(name)) < 0)
            break;
        o = append(out, o, "- 0 ");
        o = append(out, o, name);
        out[o++] = '\n';
    }
    return o;
}

/* Ask the server named `leaf` to describe itself, a message at a time until
   it says it has finished. */
static int fetch_doc(const char *leaf, char *out, int cap)
{
    int id = -1;
    for (int i = 0; i < 16; i++) {
        char name[24];
        int t = server_task(i, name, (int)sizeof(name));
        if (t < 0)
            break;
        int k = 0;
        while (name[k] && name[k] == leaf[k]) k++;
        if (!name[k] && !leaf[k]) { id = t; break; }
    }
    if (id < 0)
        return -1;

    /* Itself, without a message. A server cannot ask itself anything: the
       rendezvous would need two tasks and there is one. The kernel refuses it
       now rather than hanging, but the answer still has to come from here. */
    if (id == sys_self())
        return append(out, 0, dev_doc);

    int o = 0;
    for (;;) {
        struct vfs_req q;
        q.op = VFS_IOCTL;
        q.fd = -1;
        q.ioctl_cmd = IOCTL_DOC;
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
        o = append(out, 0, "this server does not describe itself\n");
    return o;
}

#define DEV_BUFSZ 4096

static void dev_do_open(struct vfs_req *r, int from)
{
    int fd = dev_alloc(from);
    if (fd < 0) { r->result = -1; return; }
    struct dev_file *f = &d_tab[fd];
    f->data = malloc(DEV_BUFSZ);
    if (!f->data) { r->result = -1; return; }

    int n = -1;
    if (is_name(r->path, "/doc"))
        n = format_doc_dir(f->data, DEV_BUFSZ);
    else if (ustr_has_prefix(r->path, "/doc/"))
        n = fetch_doc(r->path + 5, f->data, DEV_BUFSZ);
    else if (is_name(r->path, "/dev"))
        n = append(f->data, 0, "- 0 tree\nd 0 pci\n");
    else if (is_name(r->path, "/dev/pci"))
        n = format_pci_dir(f->data, DEV_BUFSZ);
    else if (ustr_has_prefix(r->path, "/dev/tree"))
        n = sys_devinfo(DEVINFO_TREE, 0, f->data, DEV_BUFSZ);
    else if (ustr_has_prefix(r->path, "/dev/pci/")) {
        int i = pci_index_of(r->path + 9);
        if (i >= 0)
            n = sys_devinfo(DEVINFO_PCI, i, f->data, DEV_BUFSZ);
    }

    if (n < 0) {
        free(f->data);
        f->data = 0;
        r->result = -1;
        return;
    }
    f->used  = 1;
    f->owner = from;
    f->size  = n;
    f->pos   = 0;
    r->result = fd;
}

void dev_server(void)
{
    uputs("  [dev] up (user mode), publishing what the board said it is\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;

        switch (r->op) {
        case VFS_OPEN:
            dev_do_open(r, from);
            break;
        case VFS_READ: {
            if (r->fd < 0 || r->fd >= DEV_MAXFD || !d_tab[r->fd].used) {
                r->result = -1;
                break;
            }
            struct dev_file *f = &d_tab[r->fd];
            int n = f->size - f->pos;
            if (n > r->len) n = r->len;
            if (n < 0) n = 0;
            umemcpy(r->data, f->data + f->pos, (unsigned long)n);
            f->pos += n;
            r->result = n;
            break;
        }
        case VFS_IOCTL:
            if (r->ioctl_cmd == IOCTL_PING)
                r->result = 0;
            else if (r->ioctl_cmd == IOCTL_DOC)
                r->result = vfs_doc_reply(r, dev_doc);
            else if (r->ioctl_cmd == IOCTL_GETSIZE &&
                     r->fd >= 0 && r->fd < DEV_MAXFD && d_tab[r->fd].used)
                r->result = d_tab[r->fd].size;
            else
                r->result = -1;
            break;
        case VFS_CLOSE:
            if (r->fd >= 0 && r->fd < DEV_MAXFD) {
                free(d_tab[r->fd].data);
                d_tab[r->fd].data = 0;
                d_tab[r->fd].used = 0;
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
