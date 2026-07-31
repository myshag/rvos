/* ping.c — how long a message takes to reach a module and come back.

   Every server answers IOCTL_PING immediately and with nothing else, so the
   only thing the answer carries is its own timing. That makes one command do
   two jobs: it is a benchmark of the path to a module, and it is the question
   "is that module still turning its loop", which had no way of being asked at
   all — when the shell wedged, the only recourse was reading a log and
   guessing.

   The path matters more than the name. `ping /` measures a round trip to the
   filesystem server: two context switches and two copies of a 672-byte
   request. `ping /r/` measures the same thing to a machine on the other end
   of a TCP connection, through a proxy that is mounted like any other server
   — same command, same call, four orders of magnitude apart, and the number
   is the only thing that says so.

   usage: /BIN/PING.ELF [path] [count]                                     */
#include "lib.h"

#define DEFAULT_COUNT 1000

/* A tick is 100 ns, so a single round trip is a handful of them and anything
   below that is noise. Everything here is a total over `count`, divided at
   the end — the division is the measurement. */
static void report(const char *what, unsigned long ticks, int count)
{
    say(what);
    say(": ");
    /* Nanoseconds per round trip: ticks * 100 / count. Multiply first, since
       there is no floating point here and nothing that needs one. */
    unsigned long ns = ticks * 100UL / (unsigned long)count;
    sayn(ns / 1000);
    say(".");
    unsigned long frac = (ns % 1000) / 10;
    if (frac < 10)
        say("0");
    sayn(frac);
    say(" us per round trip  (");
    sayn((unsigned long)count);
    say(" of them in ");
    sayn(ticks / 10000);
    say(" ms)\n");
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    int count = DEFAULT_COUNT;
    if (argc > 2) {
        count = 0;
        for (const char *d = argv[2]; *d >= '0' && *d <= '9'; d++)
            count = count * 10 + (*d - '0');
        if (count < 1)
            count = 1;
    }

    /* One first, on its own, because the interesting failure is that there is
       no answer at all — and a thousand of those would take a thousand times
       as long to report it. */
    if (vfs_ioctl_path(path, IOCTL_PING) < 0) {
        err("ping", "no answer from", path);
        sys_exit();
    }

    say("ping ");
    say(path);
    say("\n");

    unsigned long t0 = pticks();
    for (int i = 0; i < count; i++)
        if (vfs_ioctl_path(path, IOCTL_PING) < 0) {
            err("ping", "stopped answering:", path);
            sys_exit();
        }
    unsigned long t1 = pticks();
    report("  ioctl", t1 - t0, count);

    /* And the same path opened and closed, which is two round trips rather
       than one — the difference is what an open costs over a bare message.
       Not every name can be opened: /net/ is a prefix with files under it and
       nothing at the prefix itself, and reporting a number for a loop that
       ran once would be worse than reporting none. */
    t0 = pticks();
    int did = 0;
    for (int i = 0; i < count; i++) {
        int fd = vfs_open(path);
        if (fd < 0)
            break;
        vfs_close(fd);
        did++;
    }
    t1 = pticks();
    if (did == count)
        report("  open+close", t1 - t0, count);
    else
        say("  open+close: that name cannot be opened\n");

    sys_exit();
}
