/* get.c — fetch a page over HTTP, from a program.

   Until now this system reached the outside world because a demo inside the
   protocol stack decided to: the name was a #define in net_ip.c, the request
   was a string literal next to the TCP state machine, and the reply was
   printed by the code that reassembled it. All of that is policy, and none of
   it belongs there.

   Here it is instead, in a program on the disk that knows no more about
   networking than `cat` does:

     write "resolve example.com" to /net/ctl   -> ok 172.66.147.243
     write "connect 172.66.147.243 80"         -> ok 2
     open /net/tcp/2, write a request, read until the far end is done.

   Both of those writes block, and that is the point: there is nothing useful
   to do with a name that has not resolved or a connection that is not up, so
   the program does not spin waiting for either. It has no timers, no polling
   loop and no retry logic, because the stack is where those live.

   usage: /GET.ELF [host] [path]                                          */
#include "syscall.h"
#include "vfs.h"

static int glen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

/* To a path, not a syscall: run from the shell over TCP, the page this
   fetches comes back down the caller's connection. */
static void say(const char *s) { vfs_say(s); }

static char *append(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

/* One command to /net/ctl, one answer back. */
static int ctl(const char *cmd, char *answer, int cap)
{
    int fd = vfs_open("/net/ctl");
    if (fd < 0)
        return -1;
    if (vfs_write(fd, cmd, glen(cmd)) < 0) {
        vfs_close(fd);
        return -1;
    }
    int n = vfs_read(fd, answer, cap - 1);
    vfs_close(fd);
    if (n <= 0)
        return -1;
    answer[n] = 0;
    while (n > 0 && (answer[n - 1] == '\n' || answer[n - 1] == '\r'))
        answer[--n] = 0;
    return n;
}

/* Every answer is "ok <something>" or "error <why>". */
static const char *ok_value(const char *answer)
{
    if (answer[0] != 'o' || answer[1] != 'k')
        return 0;
    const char *s = answer + 2;
    while (*s == ' ')
        s++;
    return s;
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "example.com";
    const char *path = argc > 2 ? argv[2] : "/";
    char answer[80], cmd[128];

    say("  [get] resolving ");
    say(host);
    say("\n");

    char *p = append(cmd, "resolve ");
    p = append(p, host);
    *p = 0;
    if (ctl(cmd, answer, sizeof(answer)) < 0 || !ok_value(answer)) {
        say("  [get] ");
        say(answer);
        say("\n");
        sys_exit();
    }

    char addr[24];
    {
        const char *v = ok_value(answer);
        int i = 0;
        while (v[i] && i < (int)sizeof(addr) - 1) {
            addr[i] = v[i];
            i++;
        }
        addr[i] = 0;
    }
    say("  [get] ");
    say(host);
    say(" is ");
    say(addr);
    say("\n");

    p = append(cmd, "connect ");
    p = append(p, addr);
    p = append(p, " 80");
    *p = 0;
    if (ctl(cmd, answer, sizeof(answer)) < 0 || !ok_value(answer)) {
        say("  [get] ");
        say(answer);
        say("\n");
        sys_exit();
    }

    char conn[24];
    {
        char *q = append(conn, "/net/tcp/");
        const char *v = ok_value(answer);
        while (*v >= '0' && *v <= '9')
            *q++ = *v++;
        *q = 0;
    }

    int fd = vfs_open(conn);
    if (fd < 0) {
        say("  [get] cannot open the connection\n");
        sys_exit();
    }

    /* HTTP/1.0 with an explicit close, so the far end ends the body by
       hanging up and this program needs no notion of content length. */
    p = append(cmd, "GET ");
    p = append(p, path);
    p = append(p, " HTTP/1.0\r\nHost: ");
    p = append(p, host);
    p = append(p, "\r\nConnection: close\r\n\r\n");
    *p = 0;
    vfs_write(fd, cmd, glen(cmd));
    say("  [get] request sent; reading the reply\n\n");

    int total = 0;
    for (;;) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)                     /* 0 = the server closed its half */
            break;
        buf[n] = 0;
        say(buf);
        total += n;
    }
    vfs_close(fd);

    say("\n  [get] ");
    {
        char num[16];
        int k = 0, v = total;
        char tmp[16];
        int t = 0;
        if (v == 0)
            tmp[t++] = '0';
        while (v) {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (t)
            num[k++] = tmp[--t];
        num[k] = 0;
        say(num);
    }
    say(" bytes\n");
    sys_exit();
}
