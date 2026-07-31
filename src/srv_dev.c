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

#define DEV_BUFSZ 4096

static void dev_do_open(struct vfs_req *r, int from)
{
    int fd = dev_alloc(from);
    if (fd < 0) { r->result = -1; return; }
    struct dev_file *f = &d_tab[fd];
    f->data = malloc(DEV_BUFSZ);
    if (!f->data) { r->result = -1; return; }

    int n = -1;
    if (is_name(r->path, "/dev"))
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
