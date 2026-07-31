/* ipc.c — synchronous rendezvous IPC across isolated address spaces.

   send(dst, buf, len) blocks until dst receives; recv(buf, len) blocks until
   someone sends. The rendezvous itself is unchanged from the shared-memory
   version; what changed is that a message is now copied. Each task has its
   own page table, so the sender's address means nothing in the receiver's
   space — the kernel translates both sides and moves the bytes. That is the
   price of isolation, and it is why the vfs protocol had to grow an inline
   data area instead of handing over a pointer to a buffer. */
#include "task.h"
#include "syscall.h"
#include "uart.h"
#include "vm.h"

/* Registers in the saved context we read/write to shuttle syscall args/rets. */
#define A0(t) ((t)->ctx.x[10])
#define A1(t) ((t)->ctx.x[11])
#define A2(t) ((t)->ctx.x[12])

static void enqueue_sender(struct task *recv, struct task *snd)
{
    snd->send_next = 0;
    if (!recv->wait_sender) {
        recv->wait_sender = snd;
    } else {
        struct task *p = recv->wait_sender;
        while (p->send_next)
            p = p->send_next;
        p->send_next = snd;
    }
}

static struct task *dequeue_sender(struct task *recv)
{
    struct task *s = recv->wait_sender;
    if (s)
        recv->wait_sender = s->send_next;
    return s;
}

/* Move a message from one address space to the other, truncating to whatever
   the receiver was willing to accept. */
static int deliver(struct task *snd, uint64 sva, int slen,
                   struct task *rcv, uint64 rva, int rlen)
{
    int n = slen < rlen ? slen : rlen;
    if (n <= 0)
        return 0;
    if (vm_copy_across(rcv->pt, rva, snd->pt, sva, (uint64)n) < 0)
        return -1;
    return n;
}

/* SYS_SEND: a0 = dest id, a1 = message address, a2 = length. */
static void ipc_send(void)
{
    int    dst_id = (int)A0(current);
    uint64 sva    = A1(current);
    int    slen   = (int)A2(current);

    /* An id that names nobody — a slot never used, or one whose task has
       died and been replaced — fails the send instead of blocking on it or,
       worse, delivering to whoever moved in. */
    struct task *dst = task_by_id(dst_id);
    if (!dst || dst->state == T_UNUSED) {
        A0(current) = -1;
        return;
    }

    if (dst->state == T_BLOCKED && dst->waiting_recv) {
        /* Receiver is parked in recv(): copy straight across and free both. */
        if (deliver(current, sva, slen, dst, dst->recv_va, dst->recv_len) < 0) {
            A0(current) = -1;
            return;
        }
        A0(dst)           = current->id;    /* recv() returns the sender id */
        dst->waiting_recv = 0;
        dst->state        = T_RUNNABLE;
        A0(current)       = 0;
        return;
    }

    /* Nobody waiting: park, remembering where our message lives. It stays in
       our address space untouched until a receiver shows up to copy it. */
    current->send_va      = sva;
    current->send_len     = slen;
    current->waiting_recv = 0;
    enqueue_sender(dst, current);
    current->state = T_BLOCKED;
    schedule();
}

/* SYS_RECV: a0 = buffer address, a1 = capacity. Returns sender id in a0. */
static void ipc_recv(void)
{
    uint64 rva  = A0(current);
    int    rlen = (int)A1(current);

    /* An interrupt that arrived while we were busy outranks queued messages:
       the device is masked until it is acked. */
    if (current->irq_pending) {
        current->irq_pending = 0;
        A0(current) = (uint64)(long)IRQ_SENDER;
        return;
    }
    if (current->timer_pending) {
        current->timer_pending = 0;
        A0(current) = (uint64)(long)TIMER_SENDER;
        return;
    }

    struct task *s = dequeue_sender(current);
    if (s) {
        /* A sender was already parked: complete the rendezvous now. */
        if (deliver(s, s->send_va, s->send_len, current, rva, rlen) < 0) {
            A0(current) = -1;
            return;
        }
        A0(current) = s->id;
        A0(s)       = 0;                    /* that sender's send() returns 0 */
        s->state    = T_RUNNABLE;
        return;
    }

    current->recv_va      = rva;
    current->recv_len     = rlen;
    current->waiting_recv = 1;
    current->state        = T_BLOCKED;
    schedule();
}

/* Returns 1 if it handled the syscall number, else 0. */
int ipc_syscall(uint64 num)
{
    switch (num) {
    case SYS_SEND: ipc_send(); return 1;
    case SYS_RECV: ipc_recv(); return 1;
    default:       return 0;
    }
}
