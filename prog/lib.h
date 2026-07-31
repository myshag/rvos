#pragma once
#include "syscall.h"
#include "vfs.h"

/* lib.h — what a program on the disk has instead of a library.

   It cannot link against ulib: that lives in the shared user text of
   kernel.elf, mapped without the U bit for anything loaded at run time. So
   every program carries its own copy of the half-dozen functions it needs,
   and this header is how they stop being written out nine times. Included,
   not linked — there is no linker step for these beyond their own object.

   Everything writes through vfs_say, which means a path, which means the
   output follows whatever /dev/console is bound to where the program was
   started: the serial line from the local shell, the caller's connection from
   the one over TCP. */

static inline int plen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static inline int peq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static inline void pcpy(void *d, const void *s, int n)
{
    char *a = d; const char *b = s;
    while (n-- > 0) *a++ = *b++;
}

static inline void say(const char *s) { vfs_say(s); }

/* The clock, in one instruction and no syscall: smain sets scounteren.TM, so
   rdtime is legal in user mode. QEMU's time base is 10 MHz, which is 100 ns a
   tick — coarse for one round trip, which is why anything measured here is
   measured a thousand times. */
static inline unsigned long pticks(void)
{
    unsigned long t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

static inline void sayn(unsigned long v)
{
    char tmp[24], out[25];
    int t = 0, k = 0;
    if (v == 0)
        tmp[t++] = '0';
    while (v) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t)
        out[k++] = tmp[--t];
    out[k] = 0;
    say(out);
}

static inline void err(const char *who, const char *what, const char *path)
{
    say(who);
    say(": ");
    say(what);
    if (path) {
        say(" ");
        say(path);
    }
    say("\n");
}

/* Copy a whole file through the interface, in the chunks it arrives in. Used
   by cp and mv, and it is the entire difference between them. */
static inline int pcopy(const char *src, const char *dst)
{
    int in = vfs_open(src);
    if (in < 0)
        return -1;
    int out = vfs_create(dst);
    if (out < 0) {
        vfs_close(in);
        return -1;
    }
    int total = 0, bad = 0;
    for (;;) {
        char buf[VFS_DATA_MAX];
        int n = vfs_read(in, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        int off = 0;
        while (off < n) {
            int k = vfs_write(out, buf + off, n - off);
            if (k < 0) { bad = 1; break; }
            if (k == 0) { sys_yield(); continue; }
            off += k;
        }
        if (bad)
            break;
        total += n;
    }
    vfs_close(in);
    vfs_close(out);                     /* this is what puts it on the disk */
    return bad ? -1 : total;
}
