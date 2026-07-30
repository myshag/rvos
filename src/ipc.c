/* ipc.c — synchronous rendezvous IPC, the microkernel's core primitive.
   send(dst, msg) blocks until dst receives; recv(&msg) blocks until someone
   sends. One machine word is transferred; larger payloads pass a pointer to a
   shared request struct (rvos has no per-task isolation yet). */
#include "task.h"
#include "syscall.h"
#include "uart.h"

extern struct task tasks[NTASK];

/* Registers in the saved context we read/write to shuttle syscall args/rets. */
#define A0(t) ((t)->ctx.x[10])
#define A1(t) ((t)->ctx.x[11])

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

/* SYS_SEND: a0 = dest id, a1 = message word. */
static void ipc_send(void)
{
    int dst_id  = (int)A0(current);
    uint64 msg  = A1(current);

    if (dst_id < 0 || dst_id >= NTASK || tasks[dst_id].state == T_UNUSED) {
        A0(current) = -1;               /* bad destination */
        return;
    }
    struct task *dst = &tasks[dst_id];

    if (dst->state == T_BLOCKED && dst->waiting_recv) {
        /* Receiver is waiting: deliver now, both stay/become runnable. */
        *dst->recv_ptr   = msg;
        A0(dst)          = current->id;     /* recv() returns sender id */
        dst->waiting_recv = 0;
        dst->state       = T_RUNNABLE;
        A0(current)      = 0;               /* send() returns 0 */
        return;
    }

    /* Receiver not ready: block the sender on the receiver's queue. */
    current->ipc_msg = msg;
    enqueue_sender(dst, current);
    current->waiting_recv = 0;
    current->state = T_BLOCKED;
    schedule();                              /* run someone else */
}

/* SYS_RECV: a0 = uint64* out. Returns sender id in a0; message written to *out. */
static void ipc_recv(void)
{
    uint64 *out = (uint64 *)A0(current);
    struct task *s = dequeue_sender(current);

    if (s) {
        /* A sender was already waiting: complete the rendezvous immediately. */
        *out        = s->ipc_msg;
        A0(current) = s->id;                 /* recv() returns sender id */
        A0(s)       = 0;                     /* that sender's send() returns 0 */
        s->state    = T_RUNNABLE;
        return;
    }

    /* No sender yet: block until one arrives. */
    current->recv_ptr    = out;
    current->waiting_recv = 1;
    current->state       = T_BLOCKED;
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
