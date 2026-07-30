/* srv_fs.c — filesystem module. Owns the FAT16 driver outright: nothing
   outside this file calls fat16_* at all. Clients see only the generic
   open/read/ioctl/close interface, so the fact that a FAT16 on a RAM disk is
   behind it is an implementation detail that could be swapped for anything.

   Reading a directory is just read(): opening "/" streams a text listing,
   exactly like opening a file streams its bytes. */
#include "vfs.h"
#include "servers.h"
#include "fat16.h"
#include "syscall.h"
#include "ulib.h"

#define FS_MAXFD  4
#define FS_BUFSZ  700

struct fs_file {
    int    used;
    uint32 size;
    uint32 pos;
    char   data[FS_BUFSZ];
};
static struct fs_file fs_tab[FS_MAXFD];

static int fs_format_root(char *out, int cap)
{
    struct dirent ents[16];
    int n = fat16_list_root(ents, 16);
    int o = 0;
    for (int i = 0; i < n && o < cap - 40; i++) {
        int l = (int)ustrlen(ents[i].name);
        umemcpy(out + o, ents[i].name, (unsigned long)l);
        o += l;
        if (ents[i].is_dir) {
            out[o++] = '/';
        } else {
            umemcpy(out + o, "  (", 3); o += 3;
            o += uutoa(ents[i].size, out + o);
            umemcpy(out + o, " bytes)", 7); o += 7;
        }
        out[o++] = '\n';
    }
    return o;
}

static int fs_alloc(void)
{
    for (int i = 0; i < FS_MAXFD; i++)
        if (!fs_tab[i].used)
            return i;
    return -1;
}

static void fs_do_open(struct vfs_req *r)
{
    int fd = fs_alloc();
    if (fd < 0) { r->result = -1; return; }
    struct fs_file *f = &fs_tab[fd];

    int n;
    if (r->path[0] == '/' && r->path[1] == 0)
        n = fs_format_root(f->data, FS_BUFSZ);
    else
        n = fat16_read(r->path + (r->path[0] == '/' ? 1 : 0), f->data, FS_BUFSZ);

    if (n < 0) { r->result = -1; return; }
    f->used = 1;
    f->size = (uint32)n;
    f->pos  = 0;
    r->result = fd;
}

static void fs_do_read(struct vfs_req *r)
{
    if (r->fd < 0 || r->fd >= FS_MAXFD || !fs_tab[r->fd].used) { r->result = -1; return; }
    struct fs_file *f = &fs_tab[r->fd];
    int n = (int)(f->size - f->pos);
    if (n > r->len) n = r->len;
    if (n < 0) n = 0;
    umemcpy(r->data, f->data + f->pos, (unsigned long)n);
    f->pos += (uint32)n;
    r->result = n;
}

static void fs_do_ioctl(struct vfs_req *r)
{
    if (r->fd < 0 || r->fd >= FS_MAXFD || !fs_tab[r->fd].used) { r->result = -1; return; }
    if (r->ioctl_cmd == IOCTL_GETSIZE)
        r->result = (int)fs_tab[r->fd].size;   /* the answer rides home */
    else
        r->result = -1;
}

void fs_server(void)
{
    if (fat16_init() == 0)
        uputs("  [fs] up (user mode), FAT16 mounted from its own mapping\n");
    else
        uputs("  [fs] no valid FAT16 (run with 'make rundisk')\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;          /* our own copy, not the client's */
        switch (r->op) {
        case VFS_OPEN:  fs_do_open(r);  break;
        case VFS_READ:  fs_do_read(r);  break;
        case VFS_IOCTL: fs_do_ioctl(r); break;
        case VFS_WRITE: r->result = -1; break;      /* read-only for now */
        case VFS_CLOSE:
            if (r->fd >= 0 && r->fd < FS_MAXFD)
                fs_tab[r->fd].used = 0;
            r->result = 0;
            break;
        default: r->result = -1; break;
        }
        sys_send(from, r, (int)sizeof(*r));   /* ship the answer back */
    }
}
