/* netd.c — a network service, in the ordinary sense: a program on the FAT16
   volume, loaded into an address space of its own, holding a TCP port open and
   answering whoever calls.

   It is worth being clear about what this program does *not* contain. It has
   no notion of ethernet, ARP, sequence numbers, windows or retransmission. It
   opens files, writes commands to one and bytes to another, and reads what
   comes back — the same five calls it would use for a disk file or the
   console. Every stage of this project has been building the interface that
   makes that possible; this is the first program to take it up.

   Nor does the stack contain any notion of this program. Until now the stack
   listened on a port and greeted callers, because nothing else would. Deciding
   what to say to a caller was never its business, and it does not do it any
   more: the port belongs to whoever asks for it. */
#include "syscall.h"
#include "vfs.h"

static int nlen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void say(const char *s)
{
    while (*s)
        _ecall1(SYS_PUTC, *s++);
}

/* Write a command to /net/ctl and read the one-line answer back off the same
   descriptor. `accept` does not return until somebody calls, which costs this
   program nothing: it is asleep in the read, not spinning on it. */
static int ctl(const char *cmd, char *answer, int cap)
{
    int fd = vfs_open("/net/ctl");
    if (fd < 0)
        return -1;
    if (vfs_write(fd, cmd, nlen(cmd)) < 0) {
        vfs_close(fd);
        return -1;
    }
    int n = vfs_read(fd, answer, cap - 1);
    vfs_close(fd);
    if (n <= 0)
        return -1;
    answer[n] = 0;
    return n;
}

/* "ok 2\n" -> 2 */
static int slot_of(const char *answer)
{
    if (answer[0] != 'o' || answer[1] != 'k')
        return -1;
    const char *s = answer + 2;
    while (*s == ' ')
        s++;
    if (*s < '0' || *s > '9')
        return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static void conn_path(char *out, int slot)
{
    const char *p = "/net/tcp/";
    int n = 0;
    while (*p)
        out[n++] = *p++;
    if (slot >= 10)
        out[n++] = (char)('0' + slot / 10);
    out[n++] = (char)('0' + slot % 10);
    out[n] = 0;
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    (void)argc; (void)argv;

    char answer[64];
    if (ctl("listen 7", answer, sizeof(answer)) < 0) {
        say("  [netd] /net/ctl refused to listen\n");
        sys_exit();
    }
    int lslot = slot_of(answer);
    if (lslot < 0) {
        say("  [netd] listen: ");
        say(answer);
        sys_exit();
    }
    say("  [netd] listening on port 7; try nc localhost 5555 from the host\n");

    for (;;) {
        char cmd[24] = "accept ";
        int k = 7;
        if (lslot >= 10)
            cmd[k++] = (char)('0' + lslot / 10);
        cmd[k++] = (char)('0' + lslot % 10);
        cmd[k] = 0;

        if (ctl(cmd, answer, sizeof(answer)) < 0)
            break;
        int slot = slot_of(answer);
        if (slot < 0)
            break;

        char path[24];
        conn_path(path, slot);
        int fd = vfs_open(path);
        if (fd < 0)
            continue;

        say("  [netd] a caller, on ");
        say(path);
        say("\n");

        const char *hello = "rvos: netd here. type something and I will "
                            "send it back.\n";
        vfs_write(fd, hello, nlen(hello));

        /* An echo server, and the loop is the whole of it. read() does not
           return until there are bytes or the peer has gone: no polling, no
           yielding, no asking again. */
        for (;;) {
            char buf[VFS_DATA_MAX];
            int n = vfs_read(fd, buf, (int)sizeof(buf));
            if (n <= 0)
                break;                  /* 0 = the peer closed its half */
            vfs_write(fd, buf, n);
        }

        say("  [netd] caller hung up\n");
        vfs_close(fd);                  /* closing the last fd closes the
                                           connection: our half goes too */
    }

    say("  [netd] stopping\n");
    sys_exit();
}
