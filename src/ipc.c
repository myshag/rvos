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

/* The head of the queue for an open receive, or the first entry from a named
   sender for a closed one — which has to unlink from the middle, because
   everybody else stays where they are. That is the whole point: a message
   nobody asked for is kept, not consumed. */
static struct task *dequeue_sender(struct task *recv, int closed, int want)
{
    struct task *s = recv->wait_sender, *prev = 0;
    while (s) {
        if (!closed || s->id == want) {
            if (prev)
                prev->send_next = s->send_next;
            else
                recv->wait_sender = s->send_next;
            s->send_next = 0;
            return s;
        }
        prev = s;
        s = s->send_next;
    }
    return 0;
}

/* Is this receiver waiting for *us*? A task parked in a closed receive is
   blocked, but not available to anyone it did not name. */
static int wants(struct task *dst, struct task *src)
{
    if (dst->state != T_BLOCKED || !dst->waiting_recv)
        return 0;
    return !dst->recv_closed || dst->recv_from == src->id;
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

/* SYS_SEND / SYS_TRYSEND: a0 = dest id, a1 = message address, a2 = length.

   The difference is what happens when the destination is not waiting. send
   parks and blocks; trysend gives up and returns -1.

   That distinction is not a convenience. A rendezvous deadlocks the moment
   two tasks each decide to send to the other: each parks on the other's
   queue, and neither will ever reach a recv to collect it. Every task in this
   system avoided that by accident until now — a client sends, then receives,
   and never has two things outstanding with one server. A proxy that holds a
   read parked on the network while also writing to it has two, and the two
   directions collide. A server answering a request it parked earlier must
   therefore be able to *fail* rather than wait: it is the one that knows the
   client might be busy. */
static void ipc_send(int nonblock)
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

    /* To itself is not a wait, it is a hang: a rendezvous needs two tasks and
       the only one here is the one asking. It parked on its own queue and
       never reached a recv to take the message off — and because the task was
       a server, everything that had asked it anything stopped too, so the
       machine looked wedged rather than the task. Cost of noticing: one
       comparison, on every message. */
    if (dst == current) {
        A0(current) = -1;
        return;
    }

    if (wants(dst, current)) {
        /* Receiver is parked in recv(): copy straight across and free both. */
        if (deliver(current, sva, slen, dst, dst->recv_va, dst->recv_len) < 0) {
            A0(current) = -1;
            return;
        }
        A0(dst)           = current->id;    /* recv() returns the sender id */
        dst->waiting_recv = 0;
        dst->recv_closed  = 0;
        dst->state        = T_RUNNABLE;
        A0(current)       = 0;
        return;
    }

    if (nonblock) {
        A0(current) = -1;               /* not waiting: the caller decides */
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

/* SYS_RECV:     a0 = buffer, a1 = capacity          -> sender id
   SYS_RECVFROM: a0 = sender,  a1 = buffer, a2 = cap -> that sender, or -1

   The closed form exists because a *reply* is not an event. A client that has
   sent a request knows exactly who owes it an answer, and taking the next
   message from anybody is not a race — it is answering the wrong question.
   The open form stays what a server and a driver need: three kinds of event,
   one call. */
static void ipc_recv(int closed)
{
    uint64 rva  = closed ? A1(current) : A0(current);
    int    rlen = (int)(closed ? A2(current) : A1(current));
    int    want = closed ? (int)A0(current) : 0;

    if (closed) {
        /* Naming a task that no longer exists is an error, not a wait. */
        if (!task_by_id(want)) {
            A0(current) = (uint64)-1;
            return;
        }
    } else if (current->irq_pending) {
        /* An interrupt that arrived while we were busy outranks queued
           messages: the device is masked until it is acked. A closed receive
           leaves both flags alone — they are sticky, and the next open
           receive will find them. */
        current->irq_pending = 0;
        A0(current) = (uint64)(long)IRQ_SENDER;
        return;
    }
    if (!closed && current->timer_pending) {
        current->timer_pending = 0;
        A0(current) = (uint64)(long)TIMER_SENDER;
        return;
    }

    struct task *s = dequeue_sender(current, closed, want);
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
    current->recv_closed  = closed;
    current->recv_from    = want;
    current->state        = T_BLOCKED;
    schedule();
}

/* Returns 1 if it handled the syscall number, else 0. */
int ipc_syscall(uint64 num)
{
    switch (num) {
    case SYS_SEND:     ipc_send(0); return 1;
    case SYS_TRYSEND:  ipc_send(1); return 1;
    case SYS_RECV:     ipc_recv(0); return 1;
    case SYS_RECVFROM: ipc_recv(1); return 1;
    default:       return 0;
    }
}
