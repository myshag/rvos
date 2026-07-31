# rvos — educational RISC-V microkernel

A small teaching OS for `qemu-system-riscv64` (`virt` board), written in C and
a little assembly. It boots in machine mode, hands the machine to supervisor
mode with Sv39 paging enabled, preempts tasks on a timer, and passes messages
between them. On top of that raw message passing sits a single
Plan-9-style interface — `open/read/write/ioctl/close` — through which every
module is reached: a FAT16 filesystem, the console, and a view of kernel state.

Everything runs **headless** — serial console only, no graphics.

## The idea

The kernel does scheduling, message passing and page tables. That is all.

The filesystem, the console driver, the view of kernel state and the bit
bucket are **user-mode programs**: unprivileged tasks that reach the machine
only through syscalls, or through a device mapped into their own address
space and nobody else's. Paths are routed to them by a per-task mount table.
So a new module — a pipe, a socket, a network stack — is added without
touching the kernel at all: write a program, bind it into the tree.

The mount table is per task, so a path is not a global fact. Two programs can
run the identical call on the identical path and reach different modules,
which is how the sandbox in the demo is silenced without knowing it.

```
     shell (task 4)                      sandbox (task 5)
        |                                   |
        |  open/read/write/ioctl/close  (vfs.h — a library, not syscalls)
        v                                   v
   its namespace                       its own namespace
   "/"      -> fs                      "/"            -> fs
   "/dev/"  -> console                 "/dev/"        -> console
   "/proc/" -> proc                    "/proc/"       -> proc
        |                              "/dev/console" -> null   <— rebound
        |                                   |
        |          send/recv  (the only kernel interface)
        v                                   v
   +---------+  +-------------+  +------------+  +----------+
   | fs      |  | console     |  | proc       |  | null     |
   | FAT16   |  | UART        |  | task table |  | discards |
   +---------+  +-------------+  +------------+  +----------+
```

## Layout

| File | Role |
|------|------|
| `src/boot.S`       | reset entry: park secondary harts, clear bss, call `mstart` |
| `src/mstart.c`     | the only M-mode code: delegation, PMP, Sstc, `mret` to S-mode |
| `src/pmm.c`        | physical page allocator (free list threaded through the pages) |
| `src/vm.c`         | **Sv39**: the three-level walk, mapping, superpages, per-task spaces |
| `src/entry.S`      | supervisor-mode trap save/restore (full integer context) |
| `src/kernel.ld`    | link at `0x80000000`, boot stack |
| `src/trap.c`       | timer setup, trap/`ecall` dispatch, interrupt ownership |
| `src/plic.c`       | the interrupt controller — how a device reaches the kernel |
| `src/srv_net.c`    | **virtio-net driver** — user mode, owns the card |
| `src/net_ip.c`     | ARP, IPv4, UDP and a minimal TCP client |
| `src/task.c`       | task table, preemptive round-robin scheduler, syscalls |
| `src/ipc.c`        | synchronous rendezvous `send`/`recv` |
| `src/vfs.h`        | **the one interface** + client wrappers |
| `src/vfs.c`        | the namespace: mount table, `bind`, longest-prefix routing |
| `src/srv_fs.c`     | filesystem server — **user mode**, owns the disk mapping |
| `src/srv_console.c`| console server — **user mode**, drives the UART itself |
| `src/srv_proc.c`   | kernel state as files — **user mode**, asks via syscalls |
| `src/srv_null.c`   | the bit bucket — **user mode** |
| `src/fat16.c`      | read-only FAT16 over a RAM-backed block device |
| `src/uart.c`       | NS16550 driver + tiny `kprintf` |
| `src/ulib.c`       | the user side's own libc (kernel's is unreachable) |
| `src/user.c`       | user-mode demo programs |
| `src/loader.c`     | **ELF loader** — user mode; the kernel never parses ELF |
| `src/sh.c`         | an interactive shell — reads a line, runs what you typed |
| `prog/hello.c`     | a real program: its own ELF, loaded from the filesystem |
| `src/kmain.c`      | boot, initial namespace, the demo shell |

## Design notes

- **One address space per task.** Each task owns an Sv39 page table holding
  the kernel image and the console — kernel code has to run when a trap lands
  — plus a private stack, and nothing else. The free-page arena is mapped
  nowhere, so no task holds a translation for another's memory. The FAT16
  image is mapped into the filesystem server alone, which turns "only fs
  touches the disk" from a convention into something the MMU enforces.
- **IPC copies.** A message used to be a pointer; with separate address
  spaces an address means nothing on the far side, so `send`/`recv` name a
  buffer and the kernel translates both ends and moves the bytes. That is
  why `vfs_req` carries an inline data area and reads arrive in chunks.
- **User mode.** User code is linked into page-aligned regions and mapped
  with the U bit. No trampoline page is needed: the kernel image stays mapped
  in a user address space *without* U, which is enough for the trap vector to
  run (S-mode may touch U-less pages) and enough to keep the program out
  (U-mode may not).
- **The servers are user programs.** They share one text region, like a C
  library, but each one with writable state gets a private, page-aligned data
  region mapped only into its own address space — so servers are isolated
  from each other as well as from the kernel. The console server drives the
  UART with ordinary loads and stores because the device is mapped into *its*
  space alone; the filesystem server reads the disk the same way. Neither has
  the kernel on its data path. What the kernel does mediate is the state only
  it owns: the task table, the mount tables and the page allocator, reached
  through `SYS_TASKINFO`, `SYS_MOUNTS`, `SYS_MEMINFO`, `SYS_BIND`,
  `SYS_NSCLONE`, `SYS_ROUTE` and `SYS_PGDUMP`.
- **Scheduling** is preemptive round-robin driven by the Sstc timer; `ecall`
  provides `yield`, `send`, `recv`. Since servers spend their lives blocked in
  `recv`, most switching actually happens at IPC boundaries — the timer is
  there so a CPU-bound task can't wedge the system.
- **Namespace** is data, not policy, and it belongs to a *task*:
  `vfs_bind(prefix, task)` works on a running system, `vfs_ns_clone()` gives
  the caller a private copy to diverge (Plan 9's `rfork(RFNAMEG)`), and
  resolution is longest-prefix-wins. Because the namespace is the caller's,
  a server asked to report one has to be told whose — hence
  `vfs_dump_mounts_of(task_id, …)` behind `/proc/mounts`.
- **Filesystem** image is loaded into guest RAM at `0x84000000` via
  `-device loader`; the driver treats that region as a flat block device.

## How Sv39 works

A virtual address is 39 bits wide, carved into three page-table indices and an
offset. The top 25 bits are not free: they must all copy bit 38, which is why
the address space has a hole in the middle.

```
 63    39 38    30 29    21 20    12 11         0
[ sign  ][ VPN[2] ][ VPN[1] ][ VPN[0] ][  offset  ]
  25 bit   9 bit     9 bit     9 bit     12 bit
```

`satp` holds the physical page number of the root table. Translation walks
three levels, each index selecting one of 512 entries — 512 because a table is
one 4 KiB page of eight-byte PTEs, and 4096/8 = 2^9.

```
satp.PPN -> root --VPN[2]--> level 1 --VPN[1]--> level 0 --VPN[0]--> page + offset
```

A PTE is 64 bits:

| bits | field | meaning |
|------|-------|---------|
| 0 | V | entry is valid |
| 1,2,3 | R,W,X | readable / writable / executable |
| 4 | U | reachable from user mode |
| 5 | G | global (survives an ASID switch) |
| 6,7 | A,D | accessed / dirty |
| 53:10 | PPN | physical page number (44 bits, so PA is 56 bits) |

The rule that shapes everything: **R=W=X=0 means the entry points at the next
table**; any of them set makes it a leaf and the walk stops. So a leaf placed
at level 1 covers 2 MiB and one at level 2 covers 1 GiB, the unused VPN bits
simply folding into the offset. Superpages are not a separate feature, just an
early exit — `vm.c` uses 2 MiB leaves to cover RAM in 64 entries while mapping
the UART with a 4 KiB leaf so one path exercises all three levels.

### Three bugs worth keeping

Adding initialised data to a user program broke it, and the cause was in the
linker script. Each program's writable state was one region, cleared wholesale
at boot to zero its `.bss` — which also reset every `.data` initialiser. It
stayed invisible for seven stages because no user program had one, and
surfaced as a test hook that refused to fire. `.data` and `.bss` are now
separate spans and only the second is cleared.


The second one cost the most time. `struct task` gained a field, and
`make` rebuilt only the files that were edited — because the Makefile had no
header dependencies. One stale object file went on believing the old layout,
so it read `ns` where `namebuf` now lived. Every task's `namebuf` happened to
be zeroed, and a null namespace fell back to the root one, so the system
behaved perfectly — until a task was created whose name was not empty and the
letters of `"hello"` became a pointer. The fix is `-MMD -MP`; the lesson is
that a build that silently under-rebuilds produces bugs that look like memory
corruption.

### A bug worth keeping

Isolation immediately exposed a latent linker-script bug. RISC-V GCC puts
small objects in `.sdata`/`.sbss`, and the script collected only `.bss` and
`COMMON` — so `.sbss` (holding `current`, among others) landed *past* `_end`.
While all of RAM was identity-mapped that was invisible. The moment a task's
page table stopped at `PGROUNDUP(_end)`, the first trap tried to load
`current`, faulted, and refaulted forever. Narrowing what is mapped is what
made the mistake observable.

### Why this forced the kernel into S-mode

Paging does not exist in machine mode: `satp` is ignored there and every
address is physical. Enabling Sv39 therefore meant moving the kernel down a
privilege level, which drags in four things `mstart.c` has to arrange before
`mret` — miss any one and the machine wedges immediately:

- **`medeleg`/`mideleg`** — otherwise S-mode traps escalate to M-mode, where
  there is no longer a handler.
- **PMP** — physical memory protection defaults to *S-mode may touch nothing*,
  so an unconfigured PMP faults on the first instruction after `mret`.
- **`mcounteren`** — so S-mode may execute `rdtime`.
- **`menvcfg.STCE`** — enables Sstc, i.e. the `stimecmp` CSR. CLINT's
  `mtimecmp` is M-mode only and the machine timer interrupt cannot be
  delegated, so without Sstc an S-mode kernel has no timer of its own and
  preemption quietly degrades to cooperative scheduling.

## Build & run

Needs `gcc-riscv64-unknown-elf`, `qemu-system-misc`, `mtools`, `dosfstools`.

```bash
make            # build kernel.elf
make disk       # build the FAT16 image, including /HELLO.ELF
make rundisk    # run headless with the disk mapped into RAM
```

The boot demo runs first; when it settles you get a prompt and can type:

```
rvos$ /HELLO.ELF one two
rvos$ mem
rvos$ cat /net/status
```

The guest is reachable while it runs — port 5555 on the host is forwarded to
the port the stack listens on:

```bash
nc localhost 5555
```

Exit QEMU with `Ctrl-A` then `X`.

## What you'll see

Two tasks run the *same* call on the *same* path and reach different modules,
because one rebound that path in its own namespace; and the identical stack
address in each resolves to a different physical page, because each has its
own address space.

```
$ cat /proc/pagetable  -- shell's own space          $ ... -- sandbox's
task 4 root table 0x82fce000                         task 5 root table 0x82fc3000

its stack:                                           its stack:
va 0x2ffff000                                        va 0x2ffff000
  L2 idx 0   -> table 0x82fcb000                       L2 idx 0   -> table 0x82fc0000
  L1 idx 383 -> table 0x82fc7000                       L1 idx 383 -> table 0x82fbc000
  L0 idx 511 leaf rw-- pa 0x82fc8000                   L0 idx 511 leaf rw-- pa 0x82fbd000
```

Same virtual address, different physical page. And a module's memory really
does belong to it — the FAT16 image is mapped only into the filesystem
server, so anyone else reaching for it is stopped by the hardware:

```
the FAT16 image:
va 0x84000000
  L2 idx 2   pte 0x20bf3401  -> table 0x82fcd000
  L1 idx 32  pte 0x0  invalid -> unmapped

--- snooper (task 6) ----------------------------------
$ read *(char*)0x84000000  -- FAT16 image, mapped only into fs
[trap] load page fault in task 'snooper'
       scause=13  stval=0x84000000  sepc=0x80000764
       -> the MMU refused it; retiring the task
```

`/proc/tasks` is still a live snapshot of the scheduler, and `/proc/mounts`
and `/proc/pagetable` still answer differently depending on who reads them —
now for two independent reasons, the caller's namespace and its address space.

Finally, a program running in user mode reaches the whole system through
syscalls alone — and stops at the kernel's edge:

```
--- user program (U-mode) ------------------------------
$ write(/dev/console)   -- namespace + IPC, all via syscalls
  printed by the console server on our behalf

$ cat /proc/tasks
0  blocked  fs
2  running  proc  (me)
7  blocked  user
...

$ read *(char*)0x80000000   -- kernel text, mapped but U-less
[trap] load page fault in task 'user'
       scause=13  stval=0x80000000  sepc=0x8000128e
```

The faulting instruction is inside the user region; the address it reached
for is kernel text. Same page table, one bit apart.

Servers are isolated from each other too. They share their code, but a server
with state gets a private data region, so reaching for another's is refused:

```
--- peeker (U-mode) ------------------------------------
$ read the fs server's private data region
[trap] load page fault in task 'peeker'
       scause=13  stval=0x80003000
```

## Development stages (git history)

1. boot + UART console
2. traps, CLINT timer, preemptive round-robin + `ecall` syscalls
3. synchronous rendezvous IPC
4. FAT16 filesystem server + shell
5. one interface for every module: `open/read/write/ioctl/close`
6. dynamic namespace (`bind`) + `/proc` module
7. per-task namespaces (`vfs_ns_clone`) + `/dev/null` module
8. Sv39 paging: M-mode handover, page allocator, page tables, `/proc/pagetable`
9. isolation: an address space per task, copying IPC, device ownership
10. user mode: an unprivileged program reaching the system only via syscalls
11. the servers themselves move to user mode; the kernel keeps only
    scheduling, IPC and page tables
12. an ELF loader in user space, and a program loaded from the filesystem
13. reclaiming a dead task's memory, argv, and a shell that runs what you type
14. the PLIC, and interrupts delivered to a user-mode driver
15. DMA memory, and a virtio-net driver that sends and receives
16. interrupt-driven networking, and a ping that gets an answer
17. UDP, and enough TCP to open a connection, talk and close it
18. alarms in the kernel, and TCP retransmission that survives a lost segment
19. the network bound into the namespace: `/net/status`, `/net/tcp`
20. a connection table and the RFC 793 state machine; passive open, ARP and
    ICMP answered, DNS, and a page fetched from a real server
21. a send buffer and several segments in flight, out-of-order reassembly,
    an RTO measured rather than guessed, and congestion control

## Loading a program

`prog/hello.c` is compiled and linked on its own, at a fixed address, and
copied onto the FAT16 volume as `/HELLO.ELF`. Nothing about it is known when
the kernel is built.

The loader is a user program. It reads the file through the ordinary
filesystem interface, parses the program headers itself, and then asks the
kernel for the three things it cannot do unprivileged:

| syscall | what it does |
|---------|--------------|
| `SYS_NEWTASK` | an empty address space — kernel image for the trap path, a stack, nothing else |
| `SYS_VMLOAD`  | one `PT_LOAD` segment: map `memsz` bytes at `p_vaddr`, copy `filesz` of them, leave the rest zero |
| `SYS_START`   | set the entry point and make it runnable |

The kernel is never told what ELF is; it is handed the numbers a program
header already contains. `.bss` costs nothing to implement — pages arrive
zeroed from the allocator, so `memsz` beyond `filesz` is simply never written.
The fixed link address means no relocation and no position-independent code.

```
$ exec /HELLO.ELF
  read 5320 bytes from the fs server
  entry 0x20000000, program headers: 2
  new address space for task 11
  segment -> 0x20000000 filesz 784 memsz 784 r-x
  started

  [hello] loaded from /HELLO.ELF into a fresh address space
  [hello] nothing here was linked into the kernel
  [hello] talking to the console server over IPC
  [hello] first bytes of /README.TXT via the fs server:
    rvos readme ...
```

The loaded program is a first-class citizen: the namespace and the servers
work for it exactly as for anything else.

`argv` is planted before the program runs: the loader lays out the pointer
array and strings in a buffer, and `SYS_VMLOAD` copies it into the stack the
kernel already mapped — the same call that loads a segment, aimed at memory
that is mapped already, so it only copies. `SYS_START` then sets `sp`, `a0`
and `a1`, which is where the calling convention expects `argc` and `argv`.

The three task-building syscalls are deliberately unguarded — any task may
build another. That is the one place this system is obviously not
production-shaped; a real design puts a capability in front of exactly these.

## Reclaiming memory

A task that faults, or calls `sys_exit`, gives its pages back. The subtlety is
telling apart what it owns from what it borrows: the kernel image, the UART,
the CLINT and the disk are mapped *identically* (`pa == va`) and shared with
every other address space, while a stack page or a loaded segment sits at a
virtual address unrelated to its physical one. So `pa != va` is exactly the
test for "this page is mine"; page tables are always private and always go.
The task slot is released too, and the next `SYS_NEWTASK` reuses it.

```
rvos$ mem
free pages: 12131
rvos$ /HELLO.ELF a
  [hello] argv: /HELLO.ELF a
rvos$ /HELLO.ELF b
  [hello] argv: /HELLO.ELF b
rvos$ mem
free pages: 12131
```

## The shell

`sh.c` is a user program. It reads characters from the console server, splits
the line, and calls `spawn()` — the same function the boot-time loader uses,
living in the shared user text. Its scratch buffer is its own, because user
programs share their code but not their writable state.

## Interrupts

Only the kernel can take a trap, but the drivers live in user space, so an
interrupt has to be handed across that boundary. The split is:

- the kernel claims the interrupt from the PLIC and looks up whoever
  registered for it (`SYS_IRQ_REG`). It does **not** touch the device — it has
  no idea what the device is;
- it leaves the source masked, which is the back-pressure that stops an
  interrupt storm while a user-mode driver is merely *runnable*;
- the driver learns of it through its ordinary `sys_recv` loop: the kernel
  reports `IRQ_SENDER` instead of a task id, so one blocking primitive serves
  both messages and interrupts;
- the driver reads the device itself and calls `SYS_IRQ_ACK`, which completes
  at the PLIC and unmasks.

The console server uses this: keystrokes arrive on IRQ 10, are drained into a
ring buffer, and `read()` serves from there. The PLIC's register map is
per *context* — a (hart, privilege) pair, where hart 0 supervisor mode is
context 1 — and using the machine-mode context by mistake yields a controller
that looks configured and delivers nothing.

## The network driver

`srv_net.c` is an unprivileged program that owns the card: the virtio-mmio
window is mapped into its address space alone, so register access is ordinary
loads and stores with no syscall on the data path.

What it cannot arrange for itself is memory the *device* can reach. A device
is programmed with physical addresses, and a user program has no business
knowing one — except here. `SYS_DMA_ALLOC` hands back a zeroed page together
with both halves of its mapping, and that is the only place in the system
where a physical address crosses into user space.

The split virtqueue is three shared arrays: descriptors saying where the
buffers are, an available ring that is the driver's outbox, a used ring that
is the device's. `VQ_SIZE` is 8, so all three areas of a queue fit inside one
page at aligned offsets — which is fortunate, because the page allocator is a
free list and cannot promise physically contiguous runs. `fence` instructions
separate writing a descriptor from publishing it and publishing it from
ringing the doorbell; without them the device can read a half-built ring.

```
--- net (U-mode virtio-net driver) ---------------------
  found virtio-net in mmio slot 7, version 2
  mac 52:54:00:12:34:56
  driver ok; queues live
  sent an ARP request for 10.0.2.2, 42 bytes
  arp: 10.0.2.2 is at 52:55:0a:00:02:02
  sent an ICMP echo request, 50 bytes
  ping reply from 10.0.2.2, seq 1, 50 bytes
```

The driver does not poll. It blocks in `sys_recv`, the card's interrupt (mmio
slot *N* is wired to line *N+1*) wakes it, and both acknowledgements have to
happen: `VIRTIO_INT_ACK` tells the card, `SYS_IRQ_ACK` tells the PLIC. Miss
either and that is the last interrupt the system ever sees.

Checking it honestly means not believing the driver's own account of what it
did. `make runpcap` writes every frame to `build/net.pcap`; decoding that
capture confirms the request left, the reply arrived, and — recomputed from
the bytes rather than taken on trust — the IPv4 and ICMP checksums are right.

Two things worth knowing. QEMU presents virtio-mmio as a **legacy** transport
unless told otherwise, and its queue registers are laid out differently; the
driver speaks virtio 1.x, so `-global virtio-mmio.force-legacy=false` is not
optional. And receive buffers must be published before the device is told to
run, or the first frames arrive with nowhere to go.

`make runpcap` writes every frame to `build/net.pcap`, which is how to check
the driver without trusting its own report of what it did.

## UDP and TCP

`srv_net.c` handles virtqueues and frames; `net_ip.c` handles addresses and
protocols. They are still one task — splitting them would cost a message per
packet — but they are separate files, so neither half can quietly reach into
the other.

**UDP is complete.** It is an eight-byte header and a checksum, and both are
here. The checksum covers a *pseudo-header* that never goes on the wire — the
two addresses, the protocol and the length — which is what makes a datagram
delivered to the wrong host fail rather than be accepted.

**At this stage TCP was not complete, and it is worth being exact about what
was missing.** What worked was a client that opens a connection, sends,
receives, closes in order, and *retransmits what goes unacknowledged*:

```
tcp: SYN -> 10.0.2.2:9998
tcp: SYN-ACK received, connection established
tcp: sent 16 bytes
tcp: received 16 bytes: "hello-from-host"
tcp: closing
tcp: closed
```

Absent at this point: any second connection, any way for a host to call *in*,
out-of-order reassembly, window management beyond a fixed advertised window,
congestion control, and a fixed initial sequence number instead of a clock-
derived one. Most of that is the subject of
[TCP that answers](#tcp-that-answers) below; reassembly and congestion control
are still on the list at the end.

Retransmission needed something the system did not have — a way for a task to
act on the passage of time. `SYS_ALARM` wakes a task after a delay, and it
arrives the same way everything else does: `sys_recv` reports `TIMER_SENDER`
instead of a task id. Three kinds of event — a message, an interrupt, a
timeout — and still one blocking call.

On a virtual link nothing is ever lost, so the retransmit path would never run
and its correctness would be a matter of opinion. `net_ip.c` therefore drops
exactly one segment on purpose, and the connection only completes because the
timer recovers it:

```
tcp: [test] dropping this segment before it reaches the card
tcp: SYN -> 10.0.2.2:9998
tcp: timeout, retransmitting (attempt 1)
tcp: SYN-ACK received, connection established
```

The retransmitted segment is the *stored bytes*, not a rebuilt one: it has to
carry the same sequence number it carried the first time.

Both are verified from the far side rather than from the driver's own
account: `nc -u -l 9999` and `nc -l 9998` on the host receive what the guest
claims to have sent, and the host's reply arrives back in the guest.

## The network as files

The stack is a server now, bound at `/net/`, and that closes the circle the
project has been drawing since stage 5. Two names:

| path | read | write |
|------|------|-------|
| `/net/status` | interface, gateway, connection state, bytes queued | — |
| `/net/tcp` | bytes received on the connection | send bytes on it |

Nothing new was needed to make this work. The network task already blocked in
`sys_recv`, and the kernel already reported who woke it — a device
(`IRQ_SENDER`), the clock (`TIMER_SENDER`), or a task. Serving clients was a
third case in a loop that already had two.

`prog/hello.c`, an ELF loaded off the FAT16 volume at run time, uses it:

```
[hello] cat /net/status:
mac      52:54:00:12:34:56
address  10.0.2.15
gateway  10.0.2.2 (resolved)
tcp      established -> 10.0.2.2:9998
[hello] wrote to /net/tcp
[hello] read back from /net/tcp: from-host
```

That program contains no notion of ethernet, virtqueues or TCP. It calls
`open`, `write`, `read`, `close` — the same four calls it uses for a file and
the console — and the host on the other side receives the bytes.

One change was needed inside the driver: transmit no longer waits for the
card to return the descriptor. That wait sat inside `sys_recv`, and would have
swallowed whatever a client happened to send at that moment. Eight transmit
buffers used round-robin remove the wait entirely.

## TCP that answers

Up to here "the TCP connection" was a set of globals: one peer, one pair of
ports, one sequence number. That is enough to *make* a call and nothing else.
Three things had to change together to make the stack something a host can
call, and none of them is optional on its own.

**The globals became a table.** A control block per connection, found by the
four numbers that identify it: local port, remote address, remote port — and,
failing an exact match, a listener on that local port. The order matters.
Match the listener first and an established connection's segments get handed
to the listener, which looks from the outside like a peer that has gone mad.

**The if-ladder became the state machine.** All eleven states, named as RFC 793
names them, including the three that only exist so that a close can be
completed by either side in either order:

```
listen  syn-sent  syn-rcvd  established
fin-wait-1  fin-wait-2  closing  time-wait  close-wait  last-ack
```

A SYN arriving at a listener does not consume the listener — it allocates a
second control block and leaves the first listening. That single decision is
the difference between a program that can be called once and a server.

**ARP grew its other half.** A host that wants to reach us asks who has our
address first, and if nothing answers, the connection never gets as far as
TCP. So ARP is a small cache now, filled from both directions: from replies to
our own requests, and from requests addressed to us — the latter is not an
optimisation, because a host that asks for us is about to send to us, and we
will need its address to answer. ICMP echo is answered the same way, for the
same reason: a stack that only initiates is not reachable.

### Three smaller things that turned out to be load-bearing

*The initial sequence number.* RFC 793 wants it taken from a clock ticking
every four microseconds, so that a stray segment from an older incarnation of
the same connection cannot be mistaken for a current one. A fixed 1000 makes
that failure a certainty rather than a risk.

*A clock in user mode.* One alarm exists per task, and there are now several
deadlines competing for it — a retransmission timer per connection, plus
TIME-WAIT. Picking the nearest requires knowing what time it is, not merely
being able to ask for a wake-up. `scounteren.TM` makes `rdtime` legal in user
mode, so the stack reads the clock in one instruction with no trap, and
`sys_alarm` is set to whichever deadline comes first.

*A window that means something.* The advertised window is the space actually
left in the read queue, so a peer that outruns its reader is told to stop.
That is only half of it: when a program drains the queue the window opens
again, and a peer that has been told to stop will not restart until it is told
so. A stack that throttles a connection and then forgets to send the window
update deadlocks it.

### Calling in

`hostfwd` makes the guest reachable — QEMU's user-mode network is a NAT, so
without it nothing on the host can open a connection inward. `localhost:5555`
becomes `10.0.2.15:7`, which is the port the stack listens on:

```
$ nc localhost 5555
rvos: you have reached the guest on port 7
knock knock
```

and in the guest:

```
tcp: listening on port 7
arp: told 10.0.2.2 where we are
tcp: SYN from 10.0.2.2:53060, accepted; SYN-ACK sent
tcp: connection established (inbound)
tcp: received 12 bytes, queued for readers
tcp: peer closed its half
tcp: closing (peer went first)
tcp: closed
```

The greeting is the demo's, not the stack's, and it is in the file's demo
section with a note saying so. It exists because nothing in the guest is yet
running to answer for itself — the connection is at `/net/tcp/2` waiting to be
read, and a program to read it is the next stage's business.

### Checking it against something that does not have to be kind

A local `nc` is a forgiving examiner. QEMU's user network is a NAT onto the
machine's real one, so a segment addressed outside `10.0.2.0/24` leaves for
the actual internet. That needs a name turned into an address, which needs
DNS — a single UDP datagram out and a single one back, which is exactly what
the UDP already here is for:

```
arp: who has 10.0.2.3?
arp: 10.0.2.3 is at 52:55:0a:00:02:03
dns: who is example.com?
dns: it is at 172.66.147.243
tcp: SYN -> 172.66.147.243:80
tcp: connection established
http: GET / -> example.com
HTTP/1.1 200 OK
Date: Fri, 31 Jul 2026 00:26:30 GMT
Content-Type: text/html
Server: cloudflare
...
<!doctype html><html lang="en"><head><title>Example Domain</title>...
tcp: peer closed its half
tcp: closed
```

The DNS reply is worth one note. A name on the wire is a chain of
length-prefixed labels ending in a zero byte — except that a length byte with
its top two bits set is not a length at all but a pointer to a name earlier in
the same message, which is how an answer repeats the question without
repeating the bytes. A parser that does not expect that walks off the end of
the packet.

And one piece of honesty about what this does and does not prove. The bytes
are genuinely from `example.com`, and the address genuinely came from a real
resolver. The *TCP peer*, however, is QEMU's slirp: it terminates the
connection and relays over a host socket, so the sequence numbers and segment
sizes are its, not Cloudflare's. What the exchange demonstrates is a stack
talking to an implementation it has never seen, over a route it did not have
hardcoded, to fetch content nobody put in the source tree. A real remote
stack on the other end needs a TAP device and a routed host, which needs
privileges this project does not ask for.

The capture confirms it independently — `make runpcap`, then decode
`build/net.pcap` and recompute every checksum from the bytes rather than
trusting the driver's account:

```
 8 TCP 10.0.2.15:40002 -> 172.66.147.243:80 SYN      seq=26870 win=512  ip-ok sum-ok
 9 TCP 172.66.147.243:80 -> 10.0.2.15:40002 SYN|ACK  seq=1 ack=26871    ip-ok sum-ok
11 TCP 10.0.2.15:40002 -> 172.66.147.243:80 PSH|ACK  len=56  'GET / HTTP/1.0...'
13 TCP 172.66.147.243:80 -> 10.0.2.15:40002 ACK      len=512 'HTTP/1.1 200 OK...'
15 TCP 172.66.147.243:80 -> 10.0.2.15:40002 FIN|PSH|ACK len=316
18 TCP 10.0.2.15:40002 -> 172.66.147.243:80 FIN|ACK  seq=26927 ack=831
```

Note the two data segments: 512 bytes and then 316. The peer had more to send
and sent 512, because 512 is what we advertised. The window is not decoration.

### The files

| path | read | write |
|------|------|-------|
| `/net/status` | interface, ARP cache, every connection in the table | — |
| `/net/tcp` | the connection this system opened | send on it |
| `/net/tcp/N` | connection *N*, including the ones it answered | send on it |

```
rvos$ cat /net/status
mac      52:54:00:12:34:56
address  10.0.2.15/24
gateway  10.0.2.2
arp      10.0.2.2 -> 52:55:0a:00:02:02
arp      10.0.2.3 -> 52:55:0a:00:02:03
tcp 0 listen       :7
tcp 1 established  :40001 <-> 10.0.2.2:9998  rx 0
```

`cat` is a shell builtin as of this stage, which is less trivial than it
sounds: none of the interesting paths are files. `/proc/tasks` is rendered by
a server and `/net/status` by the protocol stack, and the shell cannot tell.
It did expose one thing the stack had wrong — a rendered report that hands
back its whole text on every read never ends, and `cat` reads until a read
returns nothing. A status file needs an offset per open, like any other file.

Closing the last descriptor on a connection closes the connection. That is
what a file interface means by close, and it happens to be what TCP means by
it too — with the distinction TCP makes and a file does not: if the peer
closed first the close is a LAST-ACK, and if we go first it is a FIN-WAIT.

## TCP that is reliable rather than hopeful

The stack could be called, but it kept one segment. A segment was "sent and
not yet given up on", stored whole, and until the peer acknowledged it nothing
else could go — a `write` while it was outstanding returned 0 and the program
was expected to try again. Three things follow from replacing that single slot
with a buffer, and they are the difference between a stack that works on a
quiet link and one that works.

**The send side is a byte stream now.** `snd_una` is the oldest unacknowledged
byte, `snd_nxt` the first not yet sent; what lies between them is in flight
and what lies after it is waiting for room. The SYN takes the sequence number
just below the buffer and the FIN the one just above the last byte, so a
handshake and a close take part in the same arithmetic as the data — which is
what lets one routine, `tcp_output`, decide what may go, whether it is being
called after a write, after an acknowledgement, or after a timeout. First
transmission and retransmission stop being different code:

```c
c->snd_nxt = c->snd_una;    /* a timeout is just "go back and send again" */
tcp_output(c);
```

A program sees the difference immediately:

```
[hello] wrote to /net/tcp
tcp: queued 12 bytes from a program
tcp: queued 11 bytes from a program
[hello] two more writes, neither waiting on the first
```

All three arrive at the host. Under the old scheme the second returned 0.

**The receive side reassembles.** A segment that arrives before the one in
front of it is not an error and not a loss — it is what a network that
reorders does. Dropping it costs a round trip to fetch again something already
in hand. So a segment ahead of `rcv_nxt` is held aside, and every time the gap
in front closes, the held segments that now fit are folded in.

This needed a test hook of its own, for the same reason retransmission did:
the virtual link never reorders, and code that never runs is code nobody has
checked. The first data segment of an inbound connection is held back and the
next one allowed to overtake it. Type two lines into `nc localhost 5555`:

```
tcp: [test] holding a segment back so the next overtakes it
tcp: segment ahead of the stream, held aside (4 bytes)
tcp: [test] releasing the held segment
tcp: received 4 bytes, queued for readers
tcp: gap closed, 4 held bytes delivered
```

and the reader gets them the right way round:

```
rvos$ cat /net/tcp/1
one
two
```

**The timeout is measured, not guessed.** 300 ms was a number in the source.
Jacobson's estimator keeps a smoothed round-trip time and its variation and
sets the timeout to `srtt + 4·rttvar`, because what matters is not the average
delay but how far past the average a sample can reasonably fall. Karn's rule
comes with it: a round-trip time may only be measured from a segment sent
once, so every retransmission cancels the measurement in progress — otherwise
a retransmitted segment's ack is credited to the wrong transmission and the
estimate collapses in exactly the conditions that need it most.

`cwnd` and `ssthresh` arrive at the same time, since the send loop has to
consult something: slow start until the threshold, congestion avoidance after
it, and on a timeout the threshold halves and the window drops to one segment.
Three duplicate acknowledgements retransmit at once rather than waiting for
the timer.

None of these numbers can be checked by reading the code, so they are in the
status file:

```
rvos$ cat /net/status
tcp 0 listen       :7
tcp 1 established  :7 <-> 10.0.2.2:42982
        rx 8  unacked 0  srtt 2ms  rto 200ms  cwnd 4096
```

`srtt 2ms` is measured — the estimator ran. `rto 200ms` is the floor, because
`srtt + 4·rttvar` on a link this fast comes out below it. `cwnd 4096` is two
increments of slow start above where it started.

**One option is sent and one is read.** A SYN now carries a maximum segment
size, which is why the TCP header length is a field rather than the constant
20 it had been. A peer that offers none is assumed to take 536 bytes, as
RFC 1122 requires; the capture shows both sides declaring:

```
34 TCP 10.0.2.2:42982 -> 10.0.2.15:7 SYN     win=65535 mss=1460
35 TCP 10.0.2.15:7 -> 10.0.2.2:42982 SYN|ACK win=2048  mss=1024
```

### What this still does not do

Honest limits, since the point of the exercise is not to claim more than the
code does. Congestion control and fast retransmit are written and are correct
by construction, but this link loses nothing that was not deliberately dropped,
so neither has been *observed* doing its job — the deliberate loss exercises
the timeout path only. The reassembly queue is three segments deep and a
fourth is dropped. There is no window scaling, no selective acknowledgement,
no Nagle and no delayed acknowledgement, so a small write becomes a small
segment and every segment is acknowledged at once.

## Next steps

- `/net/ctl`, so a program can open a connection or listen on a port instead
  of the stack's demo deciding
- a blocking `read()`, so a program serving a connection stops polling — and
  with it a program in the guest that answers the inbound call for itself,
  instead of the greeting the demo sends
- delayed acknowledgements and Nagle's algorithm: this stack answers every
  segment at once and sends every write as its own segment
- selective acknowledgement, so a loss costs one segment rather than
  everything after it
- capabilities on the task-building syscalls, which are currently open to all
- freeing a retired task's pages and page table (nothing is reclaimed yet)
- a `virtio-blk` driver, replacing the RAM image with a real disk
- FAT16 writes; subdirectory traversal
- run under OpenSBI in supervisor mode

## License

BSD 3-Clause. See [LICENSE](LICENSE).
