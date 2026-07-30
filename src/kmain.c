/* kmain.c — Stage 5: one interface (open/read/write/ioctl/close, vfs.h) for
   every module. Two servers implement it against completely different
   backends — FAT16 files and the UART — and the shell talks to both through
   the exact same four functions, never knowing which server or backend is on
   the other end. Directories are "files too": reading "/" yields a text
   listing, Plan-9 style. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "fat16.h"
#include "util.h"

/* ---------------------------------------------------------------------
 * fs server (id 0 == FS_TASK_ID): owns FAT16 entirely. Nothing outside
 * this function ever calls fat16_* directly.
 * --------------------------------------------------------------------- */
#define FS_MAXFD   4
#define FS_BUFSZ   700

struct fs_file {
    int    used;
    uint32 size;
    uint32 pos;
    char   data[FS_BUFSZ];
};
static struct fs_file fs_tab[FS_MAXFD];

static int utoa(unsigned long v, char *out)
{
    char tmp[20];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Build the root directory listing as a plain-text stream: "everything is a
   file", even a directory — reading it just yields bytes like any other. */
static int fs_format_root(char *out, int cap)
{
    struct dirent ents[16];
    int n = fat16_list_root(ents, 16);
    int o = 0;
    for (int i = 0; i < n && o < cap - 40; i++) {
        int l = strlen(ents[i].name);
        memcpy(out + o, ents[i].name, l); o += l;
        if (ents[i].is_dir) {
            out[o++] = '/';
        } else {
            out[o++] = ' '; out[o++] = ' '; out[o++] = '(';
            o += utoa(ents[i].size, out + o);
            memcpy(out + o, " bytes)", 7); o += 7;
        }
        out[o++] = '\n';
    }
    return o;
}

static int fs_alloc(void)
{
    for (int i = 0; i < FS_MAXFD; i++)
        if (!fs_tab[i].used) return i;
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
    f->used = 1; f->size = (uint32)n; f->pos = 0;
    r->result = fd;
}

static void fs_do_read(struct vfs_req *r)
{
    struct fs_file *f = &fs_tab[r->fd];
    if (r->fd < 0 || r->fd >= FS_MAXFD || !f->used) { r->result = -1; return; }
    int n = (int)(f->size - f->pos);
    if (n > r->len) n = r->len;
    if (n < 0) n = 0;
    memcpy(r->buf, f->data + f->pos, (size_t)n);
    f->pos += (uint32)n;
    r->result = n;
}

static void fs_do_ioctl(struct vfs_req *r)
{
    struct fs_file *f = &fs_tab[r->fd];
    if (r->fd < 0 || r->fd >= FS_MAXFD || !f->used) { r->result = -1; return; }
    if (r->ioctl_cmd == IOCTL_GETSIZE) {
        *(uint32 *)r->ioctl_arg = f->size;
        r->result = 0;
    } else {
        r->result = -1;
    }
}

static void fs_server(void)
{
    if (fat16_init() == 0)
        kprintf("  [fs] FAT16 mounted, serving open/read/ioctl/close\n");
    else
        kprintf("  [fs] no valid FAT16 (run with 'make rundisk')\n");

    for (;;) {
        uint64 m;
        int from = sys_recv(&m);
        struct vfs_req *r = (struct vfs_req *)m;
        switch (r->op) {
        case VFS_OPEN:  fs_do_open(r); break;
        case VFS_READ:  fs_do_read(r); break;
        case VFS_IOCTL: fs_do_ioctl(r); break;
        case VFS_WRITE: r->result = -1; break;          /* read-only fs */
        case VFS_CLOSE:
            if (r->fd >= 0 && r->fd < FS_MAXFD) fs_tab[r->fd].used = 0;
            r->result = 0;
            break;
        default: r->result = -1; break;
        }
        sys_send(from, 0);
    }
}

/* ---------------------------------------------------------------------
 * console server (id 1 == CONSOLE_TASK_ID): same vfs_req protocol, totally
 * different backend (UART instead of FAT16) — proves the interface is
 * actually abstract, not just an FS-shaped API.
 * --------------------------------------------------------------------- */
static void console_server(void)
{
    kprintf("  [console] up, serving open/read/write/close over UART\n");
    for (;;) {
        uint64 m;
        int from = sys_recv(&m);
        struct vfs_req *r = (struct vfs_req *)m;
        switch (r->op) {
        case VFS_OPEN:
            r->result = 0;                     /* single stream, fd 0 */
            break;
        case VFS_WRITE: {
            const char *p = (const char *)r->buf;
            for (int i = 0; i < r->len; i++)
                uart_putc(p[i]);
            r->result = r->len;
            break;
        }
        case VFS_READ: {
            int c = uart_tryc();
            if (c < 0) { r->result = 0; }              /* no byte waiting */
            else       { ((char *)r->buf)[0] = (char)c; r->result = 1; }
            break;
        }
        case VFS_IOCTL: r->result = -1; break;          /* nothing defined yet */
        case VFS_CLOSE: r->result = 0; break;
        default:        r->result = -1; break;
        }
        sys_send(from, 0);
    }
}

/* ---------------------------------------------------------------------
 * shell: knows nothing about FAT16 or UART registers. Every interaction
 * goes through vfs_open/read/write/ioctl/close — the one interface.
 * --------------------------------------------------------------------- */
static void cat(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("$ cat %s\n  cat: %s: not found\n\n", path, path);
        return;
    }
    char buf[512];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    kprintf("$ cat %s   (%d bytes)\n%s\n", path, n, buf);
    vfs_close(fd);
}

static void shell(void)
{
    kprintf("\n$ ls /            (reading a directory is just read())\n");
    int fd = vfs_open("/");
    if (fd >= 0) {
        char buf[512];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        kprintf("%s", buf);
        vfs_close(fd);
    }

    cat("/README.TXT");
    cat("/HELLO.TXT");
    cat("/MISSING.TXT");

    kprintf("$ ioctl(README.TXT, IOCTL_GETSIZE)\n");
    fd = vfs_open("/README.TXT");
    if (fd >= 0) {
        uint32 sz = 0;
        vfs_ioctl(fd, IOCTL_GETSIZE, (unsigned long)&sz);
        kprintf("  size = %u bytes (fetched without reading the file)\n\n", sz);
        vfs_close(fd);
    }

    kprintf("$ open(/dev/console) + write()   -- same interface, different module\n");
    fd = vfs_open("/dev/console");
    const char *msg = "  hello via vfs_write, routed to the console server\n\n";
    vfs_write(fd, msg, strlen(msg));
    vfs_close(fd);

    kprintf("[shell] done — one interface (open/read/write/ioctl/close) served\n"
            "        both the filesystem and the console.\n");
    for (;;)
        yield();
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 5: one interface — open/read/write/ioctl\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    task_create("fs",      fs_server);        /* id 0 = FS_TASK_ID */
    task_create("console", console_server);   /* id 1 = CONSOLE_TASK_ID */
    task_create("shell",   shell);            /* id 2 */
    kprintf("[boot] fs + console servers, shell; starting scheduler.\n");

    scheduler_start();
}
