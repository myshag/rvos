/* srv_fs.c — filesystem module. Owns the FAT16 driver outright: nothing
   outside this file calls fat16_* at all. Clients see only the generic
   open/read/ioctl/close interface, so the fact that a FAT16 on a RAM disk is
   behind it is an implementation detail that could be swapped for anything.

   Reading a directory is just read(): opening "/" streams a text listing,
   exactly like opening a file streams its bytes. */
#include "vfs.h"
#include "servers.h"
#include "fat16.h"
#include "malloc.h"
#include "syscall.h"
#include "ulib.h"

#define FS_MAXFD  3
/* A whole file at a time, because the FAT16 driver only reads forward.

   This used to be a fixed array of 16 and then 32 KiB, which made it the
   largest file the system could open — a limit nobody chose, coupled by hand
   to the loaders' scratch buffer with nothing to enforce the coupling. The
   buffer is allocated now, at the size the directory entry says, and grown by
   half again whenever a write runs past the end of it. The largest file is
   whatever is left of the machine's memory. */
struct fs_file {
    int    used;
    int    owner;               /* who opened it, so a dead one can be noticed */
    uint32 size;
    uint32 pos;
    uint32 cap;                 /* how much of `data` there is */
    int    dirty;               /* written to, and not yet on the disk */
    char   name[VFS_PATH_MAX];  /* what to write it back as */
    char  *data;
};
static struct fs_file fs_tab[FS_MAXFD];

/* Any directory, not just the root: a listing is what read() returns for one,
   and which directory it is has stopped being interesting to this file.

   One line per entry, and the shape is regular on purpose:

       d 0 DOCS
       - 105 README.TXT

   It used to be "README.TXT  (105 bytes)", which reads nicely and parses
   badly — and a name can contain spaces and brackets now. Type and size first
   and the name last means a reader needs no rules at all: two fields, then
   everything to the end of the line. Making it pretty is `ls`'s job, which is
   where presentation belongs. */
static int append_str(char *out, int o, const char *s)
{
    int l = (int)ustrlen(s);
    umemcpy(out + o, s, (unsigned long)l);
    return o + l;
}

static int fs_format_dir(const char *path, char *out, int cap)
{
    /* Not on the stack: a long name is 96 bytes and two dozen of them is more
       than a task's stack wants to carry. It used to be a static array of
       thirty-two, which meant the thirty-third file in a directory did not
       exist as far as anything above this line was concerned — silently, with
       nothing anywhere reporting a truncation. It grows until the driver
       stops filling it, which is the only way to know it did not. */
    static struct dirent *ents;
    static int room;

    int n;
    for (;;) {
        if (!ents) {
            room = 32;
            ents = malloc((unsigned long)room * sizeof *ents);
            if (!ents) { room = 0; return -1; }
        }
        n = fat16_list(path, ents, room);
        if (n < room)
            break;                      /* it stopped early, so it stopped */
        struct dirent *bigger = realloc(ents, (unsigned long)room * 2 * sizeof *ents);
        if (!bigger)
            break;                      /* report what we have rather than nothing */
        ents = bigger;
        room *= 2;
    }
    if (n < 0)
        return -1;
    int o = 0;
    for (int i = 0; i < n && o < cap - (FAT_NAME_MAX + 24); i++) {
        out[o++] = ents[i].is_dir ? 'd' : '-';
        out[o++] = ' ';
        o += uutoa(ents[i].size, out + o);
        out[o++] = ' ';
        int l = (int)ustrlen(ents[i].name);
        umemcpy(out + o, ents[i].name, (unsigned long)l);
        o += l;
        out[o++] = '\n';
    }
    return o;
}

/* A slot, reclaiming one from a task that is no longer there if need be.

   A program that is killed — or that faults — never closes anything, and this
   server would keep its copy of the file for ever. Three slots and three
   killed programs is a filesystem that has stopped: `cat` cannot be loaded,
   because loading it is an open. The net server has had this since a client
   could hold a connection open past its own death; it is the same problem and
   the same answer, done when the pressure appears rather than on a timer.

   What is dropped is dropped: an unclosed file is not written back. That is
   not a new rule, it is the one this server already had — nothing reaches the
   disk until close, so a program that dies leaves the volume as it was. */
static int fs_alloc(void)
{
    for (int i = 0; i < FS_MAXFD; i++)
        if (!fs_tab[i].used)
            return i;

    for (int i = 0; i < FS_MAXFD; i++)
        if (fs_tab[i].used && !sys_alive(fs_tab[i].owner)) {
            uputs("  [fs] reclaiming a file from a task that is gone\n");
            free(fs_tab[i].data);
            fs_tab[i].data  = 0;
            fs_tab[i].cap   = 0;
            fs_tab[i].used  = 0;
            fs_tab[i].dirty = 0;
            return i;
        }
    return -1;
}

/* The 8.3 name, without the leading slash the namespace uses. */
/* The whole path, since the driver walks it. */
static void fs_keep_name(struct fs_file *f, const char *path)
{
    int i = 0;
    while (path[i] && i < (int)sizeof(f->name) - 1) {
        f->name[i] = path[i];
        i++;
    }
    f->name[i] = 0;
}

/* Room for at least `want` bytes, keeping what is already there. */
static int fs_room(struct fs_file *f, uint32 want)
{
    if (f->cap >= want)
        return 0;
    uint32 cap = f->cap ? f->cap : 512;
    while (cap < want)
        cap += cap / 2 + 1;         /* half again, not double: files are big */
    char *p = realloc(f->data, cap);
    if (!p)
        return -1;
    f->data = p;
    f->cap  = cap;
    return 0;
}

static void fs_do_open(struct vfs_req *r, int create, int from)
{
    int fd = fs_alloc();
    if (fd < 0) { r->result = -1; return; }
    struct fs_file *f = &fs_tab[fd];
    f->data = 0;
    f->cap  = 0;

    /* A directory answers read() with its listing and a file with its bytes,
       and the only way to tell them apart is to ask. Listing first: a name
       that is a directory is never also a file.

       A listing has no size until it is made, so that one is grown until it
       stops overflowing; a file has one in its directory entry. */
    int n = -1;
    if (create) {
        n = 0;                                      /* a new, empty file */
    } else if (fat16_list(r->path, 0, 0) >= 0) {   /* room for none: a probe */
        for (uint32 cap = 4096; cap <= (1u << 20); cap *= 2) {
            if (fs_room(f, cap) < 0)
                break;
            n = fs_format_dir(r->path, f->data, (int)cap);
            if (n >= 0 && (uint32)n < cap - (FAT_NAME_MAX + 24))
                break;                              /* it all fitted */
            n = -1;
        }
    } else {
        int size = fat16_size(r->path);
        if (size >= 0 && fs_room(f, (uint32)size + 1) == 0)
            n = size ? fat16_read(r->path, f->data, size) : 0;
    }

    if (n < 0) {
        free(f->data);
        f->data = 0;
        r->result = -1;
        return;
    }
    f->used  = 1;
    f->owner = from;
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
    if (n <= 0) { r->result = 0; return; }
    /* Writing past the end is how a file grows, and the buffer grows with it
       rather than clipping the write — which is what it used to do, silently,
       at whatever the fixed size happened to be. */
    if (fs_room(f, f->pos + (uint32)n) < 0) { r->result = -1; return; }

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
        case VFS_OPEN:   fs_do_open(r, 0, from); break;
        case VFS_CREATE: fs_do_open(r, 1, from); break;
        case VFS_READ:  fs_do_read(r);  break;
        case VFS_IOCTL:
            /* A ping is about this server, not about a file, so it is
               answered before the descriptor is even looked at. */
            if (r->ioctl_cmd == IOCTL_PING) {
                r->result = 0;
            } else if (r->ioctl_cmd == IOCTL_DOC) {
                r->result = vfs_doc_reply(r,
                    "fs — the FAT16 volume, one whole file at a time.\n"
                    "\n"
                    "open      a file gives its bytes; a directory gives its\n"
                    "          listing, one line per entry: <type> <size> <name>,\n"
                    "          type being d or -, and the name running to the\n"
                    "          end of the line so that spaces need no rules.\n"
                    "read      forward only; there is no seek in the interface.\n"
                    "write     at the position reached; the buffer grows.\n"
                    "create    truncates, which is how a file is overwritten.\n"
                    "close     is what puts a file on the disk. Nothing before\n"
                    "          it does — a program that dies leaves the volume\n"
                    "          as it was, and that is deliberate.\n"
                    "\n"
                    "ioctl     GETSIZE  the open file's size\n"
                    "          REMOVE   delete the open file, without writing\n"
                    "                   it back\n"
                    "          MKDIR    on the path, not the descriptor: there\n"
                    "                   is nothing to open yet\n"
                    "          PING     answered before the fd is looked at\n"
                    "          DOC      this\n"
                    "\n"
                    "A file open by a task that has died is reclaimed when the\n"
                    "last slot is wanted, and not written back.\n");
            } else if (r->ioctl_cmd == IOCTL_CONF) {
                r->result = vfs_doc_reply(r,
                    "open files   3    each a whole file in one buffer\n"
                    "file size    as much memory as there is: the buffer is\n"
                    "             allocated at the size the directory entry\n"
                    "             says and grown by half again on a write\n"
                    "             past the end\n"
                    "listing      grows until the driver stops filling it\n"
                    "name         96 bytes, which is a long name in VFAT\n"
                    "sector       512 bytes, one cluster\n");
            } else if (r->ioctl_cmd == IOCTL_HOLDS) {
                char t[VFS_DATA_MAX];
                int o = 0;
                for (int i = 0; i < FS_MAXFD && o < VFS_DATA_MAX - 160; i++)
                    if (fs_tab[i].used && fs_tab[i].owner == r->len) {
                        int l = (int)ustrlen(fs_tab[i].name);
                        umemcpy(t + o, fs_tab[i].name, (unsigned long)l);
                        o += l;
                        t[o++] = ' ';
                        o += uutoa(fs_tab[i].size, t + o);
                        o = append_str(t, o, fs_tab[i].dirty
                                             ? " bytes, not yet on the disk\n"
                                             : " bytes\n");
                    }
                t[o] = 0;
                r->result = vfs_reply_text(r, t);
            } else if (r->ioctl_cmd == IOCTL_MKDIR) {
                /* On the path, not on the descriptor: there is nothing to
                   open yet. The fd is ignored and the path carries it. */
                r->result = fat16_mkdir(r->path);
            } else if (r->ioctl_cmd == IOCTL_REMOVE) {
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
                free(fs_tab[r->fd].data);   /* the copy goes when the disk has it */
                fs_tab[r->fd].data = 0;
                fs_tab[r->fd].cap  = 0;
                fs_tab[r->fd].used = 0;
            }
            r->result = 0;
            break;
        default: r->result = -1; break;
        }
        sys_send(from, r, (int)sizeof(*r));   /* ship the answer back */
    }
}
