/* bench.c — what a message costs, with nothing on top of it.

   `ping` measures a round trip through the whole interface: resolve the name,
   build a request, cross to a server, have it dispatch and answer. This
   measures the floor underneath that — two tasks, one message, nothing else —
   so the two numbers can be subtracted and the difference is what the
   interface costs over the primitive it is built on.

   It is the oldest benchmark a microkernel has, and it is here for the oldest
   reason: after two stages of surgery on the IPC there was not a single
   number anywhere saying what any of it cost. */
#include "syscall.h"
#include "ulib.h"
#include "vfs.h"
#include "servers.h"

struct pingmsg {
    int  len;                       /* what to echo back */
    int  pad0;
    char pad[sizeof(struct vfs_req) - 8];
};

static struct pingmsg msg;

void pong_main(void)
{
    static struct pingmsg m;
    for (;;) {
        int from = sys_recv(&m, (int)sizeof(m));
        if (from < 0)
            continue;
        sys_send(from, &m, m.len);
    }
}

static void report(const char *what, unsigned long ticks, int n)
{
    /* A tick is 100 ns. Everything is a total over n, divided here; the
       division is the measurement, because one round trip is a handful of
       ticks and a handful is noise. */
    unsigned long ns = ticks * 100UL / (unsigned long)n;
    char b[24];
    int k = uutoa(ns / 1000, b); b[k] = 0;
    uputs("  "); uputs(what); uputs(": "); uputs(b); uputs(".");
    k = uutoa((ns % 1000) / 10, b); b[k] = 0;
    if ((ns % 1000) / 10 < 10) uputs("0");
    uputs(b); uputs(" us\n");
}

#define ROUNDS 2000

/* Run when asked, not at boot. The first version measured during the boot
   demonstration — while the network was resolving a name, a program was
   being loaded and a retransmission timer was running — and reported the bare
   primitive as *slower* than the whole interface built on top of it. The
   number was not wrong about anything except what it was measuring. */
void bench_main(void)
{
  for (;;) {
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    uputs("\n--- what a message costs ------------------------------\n");

    /* Smallest useful message, open receive: the primitive by itself. */
    msg.len = 8;
    unsigned long t0 = r_time();
    for (int i = 0; i < ROUNDS; i++) {
        sys_send(PONG_TASK_ID, &msg, 8);
        sys_recv(&msg, 8);
    }
    unsigned long t1 = r_time();
    report("8 bytes, open recv  ", t1 - t0, ROUNDS);

    /* The same, waiting on one named sender — what vfs_call does now. */
    t0 = r_time();
    for (int i = 0; i < ROUNDS; i++) {
        sys_send(PONG_TASK_ID, &msg, 8);
        sys_recv_from(PONG_TASK_ID, &msg, 8);
    }
    t1 = r_time();
    report("8 bytes, closed recv", t1 - t0, ROUNDS);

    /* A whole vfs_req, which is what every open, read and write carries.
       The difference from the line above is the copy, twice, across two
       address spaces. */
    msg.len = (int)sizeof(struct vfs_req);
    t0 = r_time();
    for (int i = 0; i < ROUNDS; i++) {
        sys_send(PONG_TASK_ID, &msg, (int)sizeof(struct vfs_req));
        sys_recv_from(PONG_TASK_ID, &msg, (int)sizeof(struct vfs_req));
    }
    t1 = r_time();
    {
        char b[24];
        int k = uutoa(sizeof(struct vfs_req), b); b[k] = 0;
        uputs("  ("); uputs(b); uputs(" bytes is one vfs_req)\n");
    }
    report("vfs_req, closed recv", t1 - t0, ROUNDS);

  }
}
