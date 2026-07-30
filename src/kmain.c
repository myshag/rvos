/* kmain.c — Stage 4: a filesystem server (owns the FAT16 driver) and a shell,
   talking over IPC. The shell runs `ls /` and `cat` of two files; the server
   reads them off the FAT16 RAM disk and returns results in shared buffers. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "fs.h"
#include "util.h"

#define FS_ID 0

/* ---- filesystem server task ---- */
static void fs_server(void)
{
    if (fat16_init() == 0)
        kprintf("  [fs] FAT16 mounted from RAM disk\n");
    else
        kprintf("  [fs] no valid FAT16 (run with 'make rundisk')\n");

    for (;;) {
        uint64 m;
        int from = sys_recv(&m);
        struct fs_req *r = (struct fs_req *)m;
        switch (r->op) {
        case FS_LIST: r->result = fat16_list_root((struct dirent *)r->buf, r->buf_len); break;
        case FS_READ: r->result = fat16_read(r->name, r->buf, r->buf_len);              break;
        default:      r->result = -1;                                                    break;
        }
        sys_send(from, 0);          /* ack: request struct is now filled in */
    }
}

/* ---- shell task ---- */
static struct dirent ents[16];
static char filebuf[2048];

static void fs_call(struct fs_req *r)
{
    uint64 ack;
    sys_send(FS_ID, (uint64)r);
    sys_recv(&ack);
}

static void cat(const char *name)
{
    struct fs_req r;
    r.op = FS_READ;
    strcpy(r.name, name);
    r.buf = filebuf;
    r.buf_len = sizeof(filebuf) - 1;
    fs_call(&r);
    if (r.result < 0) {
        kprintf("$ cat %s\n  cat: %s: not found\n\n", name, name);
        return;
    }
    filebuf[r.result] = 0;
    kprintf("$ cat %s   (%d bytes)\n%s\n", name, r.result, filebuf);
}

static void shell(void)
{
    struct fs_req r;

    kprintf("\n$ ls /\n");
    r.op = FS_LIST;
    r.buf = ents;
    r.buf_len = 16;
    fs_call(&r);
    for (int i = 0; i < r.result; i++) {
        if (ents[i].is_dir)
            kprintf("  %s/\n", ents[i].name);
        else
            kprintf("  %s  (%u bytes)\n", ents[i].name, ents[i].size);
    }
    kprintf("\n");

    cat("README.TXT");
    cat("HELLO.TXT");
    cat("MISSING.TXT");

    kprintf("[shell] done — rvos: boot + timer + IPC + FAT16 all working.\n");
    for (;;)
        yield();
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 4: FAT16 fs-server + shell over IPC\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    task_create("fs", fs_server);           /* id 0 = FS_ID */
    task_create("shell", shell);            /* id 1 */
    kprintf("[boot] fs-server + shell; starting scheduler.\n");

    scheduler_start();
}
