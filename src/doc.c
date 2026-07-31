/* doc.c — the kernel's own answer to "describe yourself".

   Every server can be asked what it accepts, and /doc collects the answers.
   The kernel could not be asked, and it is the thing whose interface is
   least visible from inside the running system: twenty-nine calls that
   exist, from a program's point of view, only as numbers in a register.

   It cannot answer a message — it is not a task and receives nothing — so it
   answers the way it answers every other question about itself, with a
   syscall that renders text into the caller's buffer. The text is a constant
   here rather than generated, which means it can be wrong; that is the same
   risk every comment carries, and the reason the *servers* do it by answering
   instead. There is no way for the kernel to describe itself by being asked,
   because the asking is the thing being described. */
#include "riscv.h"

const char kernel_doc[] =
"the kernel — twenty-nine calls, and nothing else it will do for you.\n"
"\n"
"Everything below is `ecall` with the number in a7 and arguments in a0..a3.\n"
"There is no library between a program and this: prog/lib.h is inline\n"
"wrappers, and the servers are reached through messages sent by these.\n"
"\n"
"scheduling and time\n"
"  0  yield                       give up the rest of the slice\n"
" 18  alarm(ms)                   wake me later; 0 cancels. The only way a\n"
"                                 task can act on the passage of time\n"
" 23  wait(task)                  block until that task is gone\n"
" 26  kill(task)                  stop it, exactly as a fault would\n"
" 28  self()                      which task am I\n"
" 14  exit()                      never returns\n"
"\n"
"messages — the whole of IPC, and there is no other kind\n"
"  1  send(dst, buf, len)         blocks until dst receives\n"
" 22  trysend(dst, buf, len)      -1 rather than blocking. What a server\n"
"                                 answering a parked request must use: it\n"
"                                 may never wait on a client\n"
"  2  recv(buf, cap)     -> from  anybody. Also returns -2 for an interrupt\n"
"                                 and -3 for a timer, which is how a driver\n"
"                                 waits for three kinds of event in one call\n"
" 24  recvfrom(src, buf, cap)     that task and nobody else. A reply is not\n"
"                                 an event: taking the next message from\n"
"                                 anybody is answering the wrong question\n"
"\n"
"  A message is copied between address spaces, so a pointer in it means\n"
"  nothing on the other side — which is why the file protocol carries its\n"
"  data inline. Sending to yourself is refused: a rendezvous needs two.\n"
"\n"
"names — the namespace is per task, and mount is how a name gets a server\n"
" 20  mount(prefix, task, flags)  put a server behind a name\n"
"  9  bind(old, new, flags)       make one name mean another\n"
" 21  unmount(name)               take the whole union of it back\n"
" 10  nsclone()                   a private copy, so a child can diverge\n"
"  5  resolve(path, out, cap, n)  -> which task answers, and what name to\n"
"                                 ask it about. The nth member of a union\n"
"  7  mounts(task, out, cap)      that task's table as text; -1 means mine\n"
"\n"
"memory\n"
" 25  sbrk(delta)        -> old   move the end of this task's heap. The\n"
"                                 kernel's entire contribution to having an\n"
"                                 allocator: pages, mapped and unmapped\n"
" 17  dma_alloc(out)              one zeroed page, reported with both its\n"
"                                 virtual and physical address — the only\n"
"                                 place a program legitimately needs the\n"
"                                 second, because a device reads by it\n"
"  8  meminfo(int[2])             free and total pages\n"
"\n"
"starting a program — three primitives, matched to what an ELF header says,\n"
"so the loader lives in user space and the kernel never learns what ELF is\n"
" 11  newtask(name)      -> id    an empty address space\n"
" 12  vmload(id, seg)             one PT_LOAD segment into pages\n"
" 13  start(id, info)             an entry point, and let it run\n"
"\n"
"  Deliberately unguarded: any task may build another. Real systems put a\n"
"  capability in front of exactly these three.\n"
"\n"
"devices\n"
" 15  irq_register(irq)           this task becomes that device's driver\n"
" 16  irq_ack(irq)                handled; let it fire again\n"
" 27  devinfo(what, n, out, cap)  the device tree, the PCI bus, or this\n"
"\n"
"looking at the machine\n"
"  6  taskinfo(index, out)        one task: id, state, name, what it waits\n"
"                                 for, how many senders are queued on it\n"
" 19  alive(task)                 is that id still that task. Ids carry a\n"
"                                 generation, so this answers no for a slot\n"
"                                 that has been reused as well as an empty\n"
"                                 one\n"
"  4  pgdump(task, va, out, cap)  one translation, walked and rendered\n"
"  3  putc(c)                     the console, without a server. For the\n"
"                                 boot log and for saying what went wrong\n"
"                                 when the thing that says it is gone\n"
"\n"
"the numbers it was built with\n"
"  tasks         18 slots, ids carrying a generation in the high byte\n"
"  page          4 KiB, Sv39, three levels\n"
"  user stack    16 KiB at 0x30000000, growing down\n"
"  user heap     0x28000000, one page mapped at birth so that an allocator\n"
"                needs no variable of its own\n"
"  dma window    0x40000000\n"
"  program       loaded at 0x20000000\n"
"  message       whatever the sender says; the file protocol uses 672 bytes\n"
"  timer         Sstc, S-mode's own; the machine timer is not delegable\n"
"\n"
"what it does not have\n"
"  no signals — kill is delivered to nobody, it simply retires the task\n"
"  no permissions on any of the above, anywhere\n"
"  no threads: a task is an address space and one thread of control\n"
"  no priority: round robin over the runnable, preempted on the timer\n";

int kernel_doc_len(void)
{
    int n = 0;
    while (kernel_doc[n])
        n++;
    return n;
}
