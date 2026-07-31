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

#define FS_MAXFD  3
/* Large enough for a small executable: the loader reads whole ELF files. */
/* A whole file at a time, because the FAT16 driver only reads forward. That
   makes this the largest file the system can open — and it has to be at least
   as big as the loaders' scratch buffers, or a program on the disk cannot be
   run. The two numbers are coupled and there is nothing to enforce it. */
#define FS_BUFSZ  16384

struct fs_file {
    int    used;
    uint32 size;
    uint32 pos;
    int    dirty;               /* written to, and not yet on the disk */
    char   name[16];            /* what to write it back as */
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

/* The 8.3 name, without the leading slash the namespace uses. */
static void fs_keep_name(struct fs_file *f, const char *path)
{
    const char *p = path + (path[0] == '/' ? 1 : 0);
    int i = 0;
    while (p[i] && i < 15) {
        f->name[i] = p[i];
        i++;
    }
    f->name[i] = 0;
}

static void fs_do_open(struct vfs_req *r, int create)
{
    int fd = fs_alloc();
    if (fd < 0) { r->result = -1; return; }
    struct fs_file *f = &fs_tab[fd];

    int n;
    if (r->path[0] == '/' && r->path[1] == 0) {
        if (create) { r->result = -1; return; }     /* not a file */
        n = fs_format_root(f->data, FS_BUFSZ);
    } else if (create) {
        n = 0;                                      /* a new, empty file */
    } else {
        n = fat16_read(r->path + (r->path[0] == '/' ? 1 : 0), f->data, FS_BUFSZ);
    }

    if (n < 0) { r->result = -1; return; }
    f->used  = 1;
    f->size  = (uint32)n;
    f->pos   = 0;
    f->dirty = create;              /* an empty file still has to be created */
    fs_keep_name(f, r->path);
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

/* Into the buffer, at this descriptor's position. The disk is not touched:
   the file goes back whole when it is closed. */
static void fs_do_write(struct vfs_req *r)
{
    if (r->fd < 0 || r->fd >= FS_MAXFD || !fs_tab[r->fd].used) {
        r->result = -1;
        return;
    }
    struct fs_file *f = &fs_tab[r->fd];
    int n = r->len;
    if (n < 0 || !f->name[0]) { r->result = -1; return; }
    if (n > VFS_DATA_MAX)
        n = VFS_DATA_MAX;
    if (f->pos + (uint32)n > FS_BUFSZ)
        n = (int)(FS_BUFSZ - f->pos);       /* the file cannot outgrow this */
    if (n <= 0) { r->result = 0; return; }

    umemcpy(f->data + f->pos, r->data, (unsigned long)n);
    f->pos += (uint32)n;
    if (f->pos > f->size)
        f->size = f->pos;
    f->dirty = 1;
    r->result = n;
}

/* A file is written back whole, on close. Nothing is on the disk until then,
   which is a real property and not an accident: a program that writes and
   then faults leaves the volume as it was. It also means a file that is never
   closed is never written, and there is nobody to notice — see the README. */
static void fs_flush(struct fs_file *f)
{
    if (!f->dirty)
        return;
    f->dirty = 0;
    if (fat16_write(f->name, f->data, (int)f->size) < 0)
        uputs("  [fs] write failed; the volume is unchanged\n");
}

void fs_server(void)
{
    if (fat16_init() == 0)
        uputs("  [fs] up (user mode), FAT16 on a virtio-blk disk it drives itself\n");
    else
        uputs("  [fs] no valid FAT16 on the disk\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;          /* our own copy, not the client's */
        switch (r->op) {
        case VFS_OPEN:   fs_do_open(r, 0); break;
        case VFS_CREATE: fs_do_open(r, 1); break;
        case VFS_READ:  fs_do_read(r);  break;
        case VFS_IOCTL:
            if (r->ioctl_cmd == IOCTL_REMOVE) {
                if (r->fd >= 0 && r->fd < FS_MAXFD && fs_tab[r->fd].used) {
                    struct fs_file *f = &fs_tab[r->fd];
                    f->dirty = 0;               /* do not write back what we
                                                   are about to delete */
                    r->result = fat16_remove(f->name);
                } else {
                    r->result = -1;
                }
            } else {
                fs_do_ioctl(r);
            }
            break;
        case VFS_WRITE: fs_do_write(r); break;
        case VFS_CLOSE:
            if (r->fd >= 0 && r->fd < FS_MAXFD && fs_tab[r->fd].used) {
                fs_flush(&fs_tab[r->fd]);
                fs_tab[r->fd].used = 0;
            }
            r->result = 0;
            break;
        default: r->result = -1; break;
        }
        sys_send(from, r, (int)sizeof(*r));   /* ship the answer back */
    }
}
