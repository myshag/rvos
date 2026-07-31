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
| `src/net_ip.c`     | ARP, IPv4, ICMP, UDP, DNS and TCP; the network as files |
| `src/task.c`       | task table, preemptive round-robin scheduler, syscalls |
| `src/ipc.c`        | synchronous rendezvous `send`/`recv` |
| `src/vfs.h`        | **the one interface** + client wrappers |
| `src/vfs.c`        | the namespace: `mount` and Plan 9 `bind`, longest-prefix routing |
| `src/srv_fs.c`     | filesystem server — **user mode**, owns the disk, reads and writes |
| `src/srv_blk.c`    | **virtio-blk driver** — user mode, in the fs server's task |
| `src/srv_console.c`| console server — **user mode**, drives the UART itself |
| `src/srv_proc.c`   | kernel state as files — **user mode**, asks via syscalls |
| `src/srv_null.c`   | the bit bucket — **user mode** |
| `src/fat16.c`      | FAT16: paths, directories, reading and writing |
| `src/uart.c`       | NS16550 driver + tiny `kprintf` |
| `src/ulib.c`       | the user side's own libc (kernel's is unreachable) |
| `src/user.c`       | user-mode demo programs |
| `src/loader.c`     | **ELF loader** — user mode; the kernel never parses ELF |
| `src/sh.c`         | an interactive shell — reads a line, runs what you typed |
| `src/srv_rsh.c`    | the same shell, over TCP on port 23, and over telnet |
| `src/bench.c`      | two tasks and one message: the floor under everything else |
| `prog/lib.h`       | what a disk program has instead of a library: included, not linked |
| `prog/*.c`         | `ls cat echo mkdir rm cp mv wc ps free` — /BIN, and the services |
| `prog/hello.c`     | a real program: its own ELF, loaded from the filesystem |
| `prog/netd.c`      | a program that owns TCP port 7 and answers callers |
| `prog/get.c`       | an HTTP client: resolve, connect, fetch — all through files |
| `prog/mc.c`        | two panels in colour, over the serial line or over telnet |
| `prog/exportfs.c`  | hands this machine's namespace to whoever connects |
| `prog/import.c`    | mounts another machine's namespace into this one |
| `src/fsproto.h`    | the file protocol on a wire: explicit fields, not a struct |
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
- **Namespace** is data, not policy, and it belongs to a *task*. Plan 9's two
  operations are kept apart: `vfs_mount(prefix, task)` puts a server behind a
  name, `vfs_bind(old, new)` makes one name mean another. Both work on a
  running system, `vfs_ns_clone()` gives the caller a private copy to diverge
  (Plan 9's `rfork(RFNAMEG)`), and resolution is longest-prefix-wins with a
  bind rewriting the head of the path and resolving again. Because the
  namespace is the caller's, a server asked to report one has to be told
  whose — hence `vfs_dump_mounts_of(task_id, …)` behind `/proc/mounts`.
- **The disk is a disk.** A virtio-blk device, driven by `srv_blk.c` inside
  the filesystem server's own task — the same arrangement as the network,
  where `srv_net.c` drives the card and `net_ip.c` speaks the protocols.
  It used to be a FAT16 image copied into guest RAM with `-device loader`
  and read with a memcpy.

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
make disk       # build the FAT16 image, including the programs
make run        # run headless, with the image attached as a virtio-blk disk
```

The boot demo runs first; when it settles you get a prompt and can type:

```
rvos$ /HELLO.ELF one two
rvos$ mem
rvos$ cat /net/status
```

The guest is reachable while it runs. Port 5555 on the host is forwarded
inward, but nothing answers on it until a program in the guest asks for the
port — so start one, then call it:

```
rvos$ /NETD.ELF
```
```bash
nc localhost 5555            # on the host: talks to /NETD.ELF in the guest
nc localhost 5556            # a shell on the guest, over its own TCP stack
```

Port 5556 needs no program started for it: the remote shell is a task and
takes port 23 at boot.

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
22. blocking reads as a server declining to answer; `/net/ctl`, and a program
    on the disk that owns a port and serves callers itself
23. `resolve` and a blocking `connect`; the HTTP fetch leaves the stack and
    becomes `/GET.ELF`, a program that names its own host
24. generation-tagged task ids, a `send` that reports a dead destination, and
    servers that reclaim what a dead client was holding
25. a shell over TCP: the same shell, reading and writing a connection
    instead of the console
26. output as a path, not a syscall: a namespace inherited by children, and a
    program's output arriving wherever it was started from
27. `bind` as Plan 9 means it — a name onto a name — which deletes the
    machinery stage 26 needed to work around not having it
28. `unmount`, and namespaces that come back when the task holding one dies
29. union mounts: `bind -a` / `-b`, two directories appearing as one, and the
    search for which member has a name
30. a namespace from another machine: a wire protocol, `exportfs`, `import`,
    and the non-blocking send the whole thing turned out to need
31. two machines on one wire: a runtime address, and one writing to the
    other's control files through a mounted namespace
32. a virtio-blk driver: the RAM image becomes a disk the filesystem server
    drives itself
33. writing to it: `create` and `rm`, whole-file rewrites, both FAT copies
    kept in step, checked with `fsck.fat`
34. directories: walking a path, listing any of them, `mkdir`, and the root
    that is not one
35. the commands leave the shell and become programs in `/BIN`, which needs
    `wait` first
36. enough of the telnet protocol to be talked to by `telnet`
37. VFAT long names: UTF-16 on disk, UTF-8 in the system, and quotes in the
    shells because a file can have a space in it now
38. a closed receive, because a reply is not an event
39. `ping` and `bench`: what a message costs, and which module stopped
    answering
40. a directory listing a program can read, and a two-panel file manager in
    colour that reads it

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

## A program that owns a port

Everything so far has been the stack talking to itself. It listened on port 7
because a line in `net_start` said so, and it greeted callers because nothing
else would. Both of those are policy, and neither belongs to a protocol stack.
Handing them to a program needed two things.

### Blocking is a server declining to answer

The console driver has been interrupt-driven since stage 14, but its clients
were not. The shell spun:

```c
for (;;) {
    int n = vfs_read(fd, &c, 1);
    if (n > 0) return c;
    yield();                    /* ask again, and again */
}
```

because the server always answered at once — with 0 when there was nothing to
say. The fix needs no new mechanism and no kernel change at all. The caller is
*already* blocked, in the `sys_recv` that follows its `sys_send`; a server that
simply does not reply leaves it there. So a read with nothing to read is kept,
and answered when a key arrives:

```c
case VFS_READ:
    c = ring_get();
    if (c >= 0)          { r->data[0] = c; r->result = 1; }
    else if (waiter < 0) { waiter = from; waiting = *r; continue; }  /* no reply */
```

That `continue` is the whole of blocking I/O in this system. It is worth
noticing what did *not* happen: the kernel did not learn what a network is,
what a keyboard is, or what "wait for data" means. A microkernel that needed a
blocking primitive per kind of data would not stay small for long.

The network server does the same, with one more case: a connection whose peer
has closed and whose queue is empty is at end of file, and a reader must be
told 0 rather than parked forever.

### /net/ctl

A file to write commands to and read the answer from. Four of them:

| command | answer |
|---------|--------|
| `connect <a.b.c.d> <port>` | `ok <slot>` |
| `listen <port>` | `ok <slot>` |
| `accept <slot>` | `ok <slot>`, when somebody calls |
| `close <slot>` | `ok` |

`accept` is the interesting one, and it needed nothing that reads did not
already need: it parks the caller until a connection on that listener reaches
ESTABLISHED, or answers at once if one is already sitting there unclaimed.

### netd

`prog/netd.c` is a second ELF on the FAT16 volume. It listens on port 7,
accepts, greets, echoes, and goes back to accepting:

```c
ctl("listen 7", answer, sizeof answer);
for (;;) {
    ctl(accept_cmd, answer, sizeof answer);      /* sleeps here */
    int fd = vfs_open(conn_path);                /* /net/tcp/N */
    vfs_write(fd, hello, len);
    for (;;) {
        int n = vfs_read(fd, buf, sizeof buf);   /* and here */
        if (n <= 0) break;                       /* 0 = the peer is done */
        vfs_write(fd, buf, n);
    }
    vfs_close(fd);
}
```

There is no polling and no yielding in it, and there is no networking in it
either: no ethernet, no ARP, no sequence numbers, no windows, no
retransmission. It opens files, writes bytes and reads bytes — the five calls
it would use for a file on the disk or for the console. The stack, for its
part, no longer contains a port number or a greeting.

```
rvos$ /NETD.ELF
  tcp: listening on port 7
  [netd] listening on port 7; try nc localhost 5555 from the host
```

and from the host:

```
$ nc localhost 5555
rvos: netd here. type something and I will send it back.
hello netd
hello netd
again
again
```

with the guest reporting, in the same run, that those bytes went through the
reassembly path on the way in and came back out in the right order anyway:

```
tcp: SYN from 10.0.2.2:51114, accepted; SYN-ACK sent
tcp: connection established (inbound)
tcp: [test] holding a segment back so the next overtakes it
  [netd] a caller, on /net/tcp/1
tcp: segment ahead of the stream, held aside (6 bytes)
tcp: [test] releasing the held segment
tcp: received 11 bytes, queued for readers
tcp: gap closed, 6 held bytes delivered
tcp: peer closed its half
  [netd] caller hung up
tcp: closing (peer went first)
tcp: closed
```

This is the circle the project has been drawing since stage 5 closing on
itself. `open`, `read`, `write`, `close` reached a FAT16 file, then a console,
then `/proc`, then a network interface, and now a TCP connection that a
program in the guest serves for itself — with the same four calls, and with
neither side knowing anything about the other.

### Honest limits

One waiter per connection, and one for the console: a second reader is told 0
rather than queued. Nothing notices when a client dies holding server-side
state, so a program that exits without closing leaks it. And there is no
`poll`, so a program that wants to wait on two things at once cannot; `netd`
handles one caller at a time for exactly that reason.

The second of those is dealt with in [Knowing who is
gone](#knowing-who-is-gone) below.

## Fetching a page, from the operating system

The stack reached the outside world at stage 20, but it was the stack that
decided to: the host name was a `#define` in `net_ip.c`, the request was a
string literal next to the TCP state machine, and the reply was printed by the
code that reassembled it. That was the last policy left inside the protocol
layer, and it is gone now.

Two more `/net/ctl` commands were what it took:

| command | answer |
|---------|--------|
| `resolve <name>` | `ok <a.b.c.d>`, when the resolver replies |
| `connect <a.b.c.d> <port>` | `ok <slot>`, when the handshake finishes |

Both wait, for the same reason a read waits: there is nothing useful to do
with a name that has not resolved or a connection that is not up, and telling
a program otherwise only invites it to poll. `resolve` also gave DNS the one
thing it had been missing — UDP does not retransmit, so a question with no
answer needs a timer of its own, and it now shares the alarm with everything
else.

`prog/get.c` is then the whole of an HTTP client:

```c
ctl("resolve example.com", answer);   /* -> ok 172.66.147.243  */
ctl("connect 172.66.147.243 80", answer);  /* -> ok 2          */
fd = vfs_open("/net/tcp/2");
vfs_write(fd, "GET / HTTP/1.0\r\nHost: ...\r\nConnection: close\r\n\r\n", n);
while ((n = vfs_read(fd, buf, sizeof buf)) > 0)  say(buf);
```

It has no timers, no retry logic and no polling loop, because those live in
the stack; and it has no notion of ethernet, ARP or sequence numbers, because
those do too. HTTP/1.0 with an explicit close means the far end ends the body
by hanging up, so the program does not need to understand content length
either — `read` returning 0 is the whole protocol.

```
rvos$ /GET.ELF example.com
  [get] resolving example.com
  dns: who is example.com?
  dns: it is at 172.66.147.243
  tcp: SYN -> 172.66.147.243:80
  tcp: connection established
  [get] request sent; reading the reply

HTTP/1.1 200 OK
Content-Type: text/html
Server: cloudflare
...
<!doctype html><html lang="en"><head><title>Example Domain</title>...
  [get] 829 bytes
```

The name is an argument, so a different one goes somewhere else — and doing
that turned up the first thing on this project that loss recovery had to
handle without being asked to:

```
rvos$ /GET.ELF neverssl.com
  dns: it is at 34.223.124.45
  tcp: SYN -> 34.223.124.45:80
  tcp: timeout, retransmitting (attempt 1)
  tcp: timeout, retransmitting (attempt 2)
  tcp: timeout, retransmitting (attempt 3)
  tcp: connection established
```

Nothing was deliberately dropped there. The initial retransmission timeout is
300 ms and the path to that host is slower than that, so the backoff ran for
real: 300, 600, 1200, and the connection came up on the fourth attempt. Up to
this point every retransmission in this project had been staged.

## Knowing who is gone

Everything above assumed programs behave. A program that closes what it opened
costs a server nothing; one that exits — or faults, which from the server's
side is the same event — while holding a descriptor leaves state behind that
nothing will ever reclaim. Following that through turned up three separate
problems, only one of which was the one that had been written down.

**A task id named a slot, and slots are reused.** `tasks[]` is a fixed table
and `task_retire` hands the slot back; the next `SYS_NEWTASK` takes it, with
the same id. Anything that had remembered that id — a server holding an
unanswered request, a mount table entry, another task about to send — went on
believing it. The failure is not a hang, which would at least be visible: it
is a message delivered correctly to the wrong task.

An id is now a slot and a generation together, and the generation is bumped
when the slot is released:

```c
tasks[i].id = i | (tasks[i].gen << 8);       /* on allocation */
t->gen++;                                    /* on retirement */
```

so `task_by_id()` can tell "gone" from "someone else". The ids the boot tasks
get are unchanged, because generation 0 leaves them equal to the slot — which
is why `servers.h` still names them by number. In the transcript below,
`task 519` is slot 7, generation 2: the same slot as tasks 7 and 263 before
it, and provably not the same task.

**`sys_send` returned nothing.** The kernel already refused to send to a dead
task, but the wrapper threw the answer away, so `vfs_call` sent, ignored the
failure, and then blocked in `recv` waiting for a reply that could not come.
Calling a server that had died hung the caller permanently. It returns `int`
now and `vfs_call` gives up instead.

**Nothing told a server that a client was gone — and nothing should.** The
obvious fix is a message from the kernel when a task dies, which means the
kernel keeping a list of who cares, which means the kernel knowing what a
server is. The cheaper answer is to let servers ask:

```c
SYS_ALIVE   /* a0 = task id -> 1 if that id still names that task */
```

Every open now records the task that made it, and the network server sweeps
its own tables before serving a request and on every timer tick. A connection
whose last holder has died is closed exactly as that program would have closed
it — which, since it is TCP, means a real FIN and a real TIME-WAIT, not a
table entry silently dropped.

Ownership starts earlier than the first `open`: a connection belongs to
whoever asked `/net/ctl` for it, from the moment it exists. Waiting until the
program opens `/net/tcp/N` would leave a window in which the connection
belonged to nobody and would outlive its creator.

`/HELLO.ELF leak` is a deliberate bad citizen — it opens a control file and a
connection and exits holding both:

```
rvos$ /HELLO.ELF leak
  [hello] opening /net/ctl and a connection, then exiting
  tcp: SYN -> 10.0.2.2:9998
  tcp: connection established
  [hello] ctl says: ok 1
  [hello] exiting without closing anything

rvos$ cat /net/status
  net: reclaiming a control file from dead task 263
  net: reclaiming a connection from dead task 263
  tcp: closing (active)
  tcp: closed
```

and a listener held by a program that is still running is left alone, which is
the property that actually matters:

```
rvos$ /NETD.ELF
rvos$ /HELLO.ELF leak
  net: reclaiming a control file from dead task 519
rvos$ cat /net/status
tcp 0 listen       :7                     <- netd's, untouched
```

### The limitation as stated was not the one that existed

The previous section claimed a *parked* task could die and wedge the server on
the reply. It cannot: a task parked in a read is blocked in `sys_recv`, and a
task that cannot run cannot exit or fault. The reachable version of the
problem was the duller one — a task dying with a descriptor open — and it was
worth finding out which, because the fix is different. The parked-request case
is now guarded anyway, since guarding it costs one comparison.

### Still not fixed

Reclamation is lazy: a leaked connection lingers until the next request or the
next timer tick. Nothing wakes the server to do it sooner, and on a system
where that mattered the kernel would have to say something after all. The
sweep also costs a handful of `SYS_ALIVE` traps per request, which is fine at
four connections and would not be at four thousand. And the console still
takes one waiter.

## A shell over TCP

`sh.c` reads from `/dev/console` and writes to it. `srv_rsh.c` reads from a
TCP connection and writes to it, and is otherwise the same program: a line,
split into words, either a builtin or a path to run. Nothing about the network
is visible in it beyond four lines of setup — it asks `/net/ctl` for port 23,
accepts, opens `/net/tcp/N`, and from there the connection is a file.

```
$ nc localhost 5556

rvos — you are on the guest, over its own TCP stack.
type `help`. connection 0

rvos# net
mac      52:54:00:12:34:56
address  10.0.2.15/24
gateway  10.0.2.2
arp      10.0.2.2 -> 52:55:0a:00:02:02
tcp 0 established  :23 <-> 10.0.2.2:46936
        rx 0  unacked 0  srtt 1ms  rto 200ms  cwnd 7168
tcp 1 listen       :23

rvos# cat /README.TXT
rvos readme
===========
Educational RISC-V microkernel with FAT16.

rvos# run /GET.ELF example.com
started as task 518; its output goes to the serial console
```

`cat` is the command worth looking at, and it is worth looking at because it
is dull. The path may be a file on the FAT16 volume, a report the proc server
renders on demand, or the state of a TCP connection — and neither `cat`, nor
the shell, nor the connection carrying the answer can tell which. The second
line of that transcript is the stack describing, over one of its own
connections, the connection it is describing it over.

Every other command is the same trick with the path filled in: `ls` is `cat`
of a directory, `ps` is `/proc/tasks`, `net` is `/net/status`, and `mounts` is
`/proc/mounts` — which reports *this shell's* namespace, and would answer
differently for a task that had cloned and rebound its own. That is why the
proc server has to be told whose to report rather than having "the" mount
table to look at:

```
rvos# mounts
/ -> task 0
/dev/ -> task 1
/proc/ -> task 2
/net/ -> task 11

rvos# ls
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
DOCS/
HELLO.ELF  (6416 bytes)
```

`ls /DOCS` fails, and honestly: the filesystem server does not walk into
subdirectories yet, which is a limit of the server and not of the shell.

It is a task in `kernel.elf` rather than a program on the disk for one reason:
`spawn()` lives in the shared user text that this image links, and a program
loaded from FAT16 cannot reach it. Everything else it does, a disk program
could.

### Two bugs this turned up

**A linker glob that was nearly right.** The remote shell needs its own
writable region — sharing the local shell's would mean two tasks sharing a
line buffer and an ELF scratch area. It got one, and it stayed empty, because
the existing pattern for the local shell was `*sh.o`, and ld matches these
against the whole path it is given: `build/srv_rsh.o` ends in `sh.o` too. So
the new file's globals were quietly filed under the old shell's region, the
region that should have held them had nothing in it, and the first write
faulted:

```
[trap] store page fault in task 'rsh'
       scause=15  stval=0x80013000
```

Anchoring the pattern with a directory separator fixes it. The lesson is not
about ld: a glob that is *nearly* right is worse than one that is wrong,
because the build succeeds and the symptom arrives somewhere else.

**A file that changed under its reader.** `cat /net/status` over TCP printed a
stray digit at the end. The status file was rendered afresh on every read and
served from the caller's offset — and between the first read and the second,
`cwnd` had grown a digit, so the offset now pointed into a longer string. It
is rendered once, at `open`, and the reader gets the system as it was when it
asked. A report that changes under its reader is not a file.

### Where a program's output goes

*(This section describes stage 26. What it built was later replaced by one
line — see [Bind, as Plan 9 means it](#bind-as-plan-9-means-it).)*

A program started with `run` has its output arrive on the connection:

```
rvos# run /GET.ELF example.com
starting /GET.ELF
  [get] resolving example.com
  [get] example.com is 104.20.23.154
HTTP/1.1 200 OK
Server: cloudflare
...
  [get] 828 bytes
```

and `/GET.ELF` contains no line about connections, shells or redirection. Four
mechanisms cooperate, none of them added for this, and no part of the chain
knows about any other part:

1. **Output is a path, not a syscall.** Every program used to write with
   `SYS_PUTC` — one character per trap, straight to the UART. That is right
   for a startup line printed before anything is listening, and wrong for
   everything after, because a syscall cannot be bound to anything. A path
   can. Programs write to `/dev/console` now, falling back to `SYS_PUTC` only
   if nothing is bound there or the far end has gone.
2. **The shell bends its own view.** `sys_nsclone()` gives it a private mount
   table; `sys_bind("/dev/console", NET_TASK_ID)` makes that name mean the
   network server in it, and in nobody else's.
3. **A child inherits its parent's namespace.** It used to get the root one,
   so no arrangement a parent made could reach it. This is Plan 9's rule, and
   the reason `vfs_ns_clone` exists is for a child that wants to diverge.
4. **The loader closes the race.** `spawn()` is the only code that holds a
   child between "address space built" and "allowed to run", so that is where
   the connection is attached to the task id. Doing it after `spawn` returns
   is a race the child can win — and does, on a round-robin scheduler.

You can see the join from inside:

```
rvos# mounts
/ -> task 0
/dev/ -> task 1
/proc/ -> task 2
/net/ -> task 11
/dev/console -> task 11        <- only in this shell's view
```

The same program run from the serial console still prints on the serial
console, because there `/dev/console` still means the console server. Nothing
in the program changed; the name did.

### What it does not do

There is no `wait()` and no job control, so `run` returns at once and the
prompt comes back while the program is still talking — which is why the
announcement is printed *before* the program starts rather than after, where
it would land in the middle of the program's first line. `ps` says what is
running.

One session at a time, for the same reason `netd` takes one caller at a time:
there is no `poll`.

## Bind, as Plan 9 means it

What this system called `bind` was `mount`: it put a *server* behind a name.
That is a real operation and Plan 9 has it, but it is not bind, and calling it
bind hid the fact that the other one was missing. A name could be pointed at a
server; it could not be pointed at another name.

The cost of that gap was the previous section. To make a program's output
arrive on a connection, the network server had to special-case `/dev/console`,
keep a table of which task belonged to which connection, and reclaim entries
from it when tasks died — and the loader had to attach the association in the
one window where a child exists but has not run, because doing it later is a
race. Four moving parts, and a paragraph in this README explaining why the
race mattered.

With the real operation, all of it is one line:

```c
sys_bind(path, "/dev/console");     /* path is "/net/tcp/2" */
```

and the four parts are gone: 57 lines out of the network server, 36 out of the
loader, and the race with them — there is nothing to arrange for the child,
because the arrangement is in the namespace it inherits by construction.

```
rvos# mounts
/ -> task 0
/dev/ -> task 1
/proc/ -> task 2
/net/ -> task 11
/dev/console -> /net/tcp/0     <- a name, not a server
```

`bind(old, new)` takes Plan 9's argument order, in which the *second* name is
the one that changes — the thing everybody gets backwards. A second bind on
the same point replaces the first rather than stacking, which is Plan 9's
`MREPL` default and is what lets the shell rebind on every connection without
filling the table.

Resolution rewrites the head of the path and goes round again, up to four
times, because `bind /a /b; bind /b /a` is a thing a person can type:

```
rvos# bind / /mnt
rvos# cat /mnt/README.TXT
rvos readme
===========
Educational RISC-V microkernel with FAT16.

rvos# bind /a /b
rvos# bind /b /a
rvos# cat /a
rsh: cannot open /a          <- four hops and it gives up, rather than not
```

Two things had to be right in the rewrite, and neither was on the first try.
Joining `/` to `/README.TXT` gives `//README.TXT`, which is a name the
filesystem has never heard of. And a prefix has to end where a component ends,
or `/mnt` claims `/mnt2/x` and rewrites it to `2/x`.

### Taking it back

`unmount(name)` is the other half, and the demonstration is the one worth
having: undo the binding and the *same program*, run the *same way*, sends its
output somewhere else.

```
rvos# unmount /dev/console
rvos# run /HELLO.ELF after-unmount
starting /HELLO.ELF
rvos#                          <- nothing here
```

and on the serial console:

```
  [hello] argv: /HELLO.ELF after-unmount
```

Plan 9's `unmount(nil, old)` removes what is mounted on a name; with no unions
there is nothing to remove it *from*, so the entry either exists or does not.
The hole is filled with the last entry rather than shifted over, because
resolution is longest-prefix-wins and the order of the table has never meant
anything — which is easy to say and worth testing, so `unmount /proc/` takes
out a middle entry and `ps` stops working until `mount /proc/ 2` puts it back.

### Namespaces come back now

There are eight private namespaces in the system and `vfs_ns_clone` used to
take the next one for ever. Three are spoken for at boot — root, the sandbox
demo, the shell over TCP — so the fourth program to want one would have failed,
and nothing would have said why.

They are reclaimed when the last task pointing at one is retired. A *sweep*,
not a reference count, and the choice is the point: a count has to be right in
every place a namespace pointer is copied — `task_create`, the inheritance in
`task_new_empty`, `ns_clone` itself — and one missed increment is either a slot
that never returns or one freed while in use. The sweep has to be right once,
over a table four entries wide.

It has one trap, and it is the same one `alloc_slot` has: a task under
construction has state `T_UNUSED` *and* a page table, and its namespace is very
much still spoken for. "Live" has to mean what the allocator means by it.

`/proc/mounts` ends with a line about the pool, because running out is a thing
that happens:

```
rvos# mounts
/ -> task 0
/dev/ -> task 1
/dev/console -> /net/tcp/0
-- namespaces 3 of 8 in use
```

Ten runs of a program that clones a namespace and exits leave it at 3.

### Two names, one directory

`bind -a` and `bind -b` join a name instead of replacing it, and the name then
has more than one answer. Plan 9's flags, spelled the same way:

```
rvos# ls
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
HELLO.ELF  (6888 bytes)

rvos# bind -a /proc/ /
rvos# ls
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
HELLO.ELF  (6888 bytes)
tasks                      <- the same directory, second member
mounts
pagetable

rvos# cat /tasks           <- the filesystem does not have this file
0  blocked  fs
1  blocked  console
2  running  proc  (me)
```

`cat /README.TXT` still comes off the disk and `cat /tasks` comes from the
proc server, and neither the shell nor either server knows the other is there.
`bind -b` puts the new member first, which is visible immediately: the listing
starts with the proc entries instead of ending with them.

**The search is in the client, not the kernel**, and that is the design
decision worth stating. "Which member has this name" cannot be answered by
looking at the mount table: the kernel holds the table but not the files, and
only an *open* can tell. So `vfs_resolve` gained a member index — give it 0, 1,
2 and it hands back successive candidates — and `vfs_open` walks them until
one succeeds. Plan 9 does this search in the kernel because the kernel holds
channels; here the equivalent place is the library where `open` already lives.

Opening a file takes the first member that has it; listing a directory takes
all of them, which is the one place a program has to know a union exists. In
the shell that is `ls` versus `cat`, and `/proc/` had to learn to list itself,
because a directory that will not say what is in it is no use in a union.

**A property was repealed.** The previous stage filled an unmounted hole with
the last entry in the table, on the grounds that resolution is
longest-prefix-wins and the order of the table has never meant anything. That
was true when it was written and stopped being true here: order is now the
order a union is searched in, and swapping one entry past another would
silently reorder it. `unmount` shifts, and takes the whole union at that name
at once — Plan 9's `unmount(nil, old)`:

```
rvos# unmount /
rvos# ls
rsh: cannot open /
```

### What it still is not

The choice among a union's members is made once, at the name the caller asked
for; anything reached by following a bind from there takes that name's first
answer. Unions inside unions are a generality this does not need and could not
explain.

Plan 9 evaluates `old` at bind time and holds the resulting channel, so
rebinding what `old` refers to afterwards does not disturb the bind. This
stores the string and re-resolves it on every open, which is simpler and
observably different: rebind `/net/tcp/0` and everything bound onto it moves.
Eight mounts per namespace is a hard ceiling with no error a program is likely
to check.

## A namespace from another machine

This is what Plan 9's namespace is *for*, and the surprise is how little it
took. A mount takes a task; a task is a thing that answers `vfs_req`; nothing
says where the answers have to come from. So a remote mount is a program that
holds a TCP connection and is mounted like any other server:

```
rvos# import 10.0.2.2 9564 /r/
mounted, task 518
rvos# ls /r/
hello.txt
motd
host.txt
rvos# cat /r/motd
rvos reached across a TCP connection for these bytes.
rvos# mounts
/ -> task 0
/net/ -> task 11
/r/ -> task 518            <- indistinguishable from a local server
```

Not one line of `vfs.c` knows about this.

### The protocol is not the struct

Inside one machine a request is a `struct vfs_req` and the kernel copies 576
bytes between two address spaces. Between machines that will not do, and the
reason is worth stating: 576 is what the compiler chose, padding included. A
protocol whose meaning depends on a compiler's padding is not a protocol. So
`src/fsproto.h` writes the fields out one at a time, little-endian, with an
explicit length for everything that has one — 32 bytes of header, then the
path and the data. A one-byte read is 33 bytes on the wire instead of 576.

The header carries a tag. This implementation keeps one request outstanding at
a time and does not need it; a format is the wrong place to economise, and
pipelining is the obvious next thing.

### The deadlock that had been waiting

`import` is the first task in this system with **two things outstanding with
one server at once**: a read parked on the network, and a request in flight to
it. Every task before this sent, then received, and so could never be the
target of a message while it was itself sending.

A rendezvous deadlocks exactly there. Import sends; the network server is not
in a `recv`, so import parks on its queue and blocks. The network server
answers the parked read; import is not in a `recv`, so it parks on import's
queue and blocks. Neither will ever reach a `recv` to collect the other.

```c
SYS_TRYSEND   /* send, but -1 rather than block if nobody is waiting */
```

A server answering a request it parked earlier must be able to *fail*: it is
the one that knows the client might be busy. A refused reply leaves the
request parked with nothing consumed on its behalf — which is why the bytes of
a read are now copied out, offered, and only removed from the queue once the
answer has been taken. And a refusal arms a deadline of its own, because the
next packet might be a long way off and an answer must not sit in a queue with
nobody to nudge it.

It follows that `import` cannot use `vfs_read` and `vfs_write`: those send and
then receive, assuming the next message is the answer. Here the next message
may be a client arriving, the parked read completing, or the reply being
waited for. Its loop is raw `send`/`recv`, dispatching on who sent it.

### Two bugs it turned up

`exportfs.elf` is 8776 bytes and the shell said only "cannot run". The
filesystem server reads a whole file into an 8 KiB buffer, and `fat16_read`
**silently returned the first 8192 bytes** — a truncated file the caller had no
way to recognise as truncated. Only a bounds check in the ELF loader stopped a
mangled program from being run. A file that does not fit is an error now, and
the buffer matches the loaders' scratch size — two numbers that are coupled
with nothing to enforce it.

And a proxy cannot know its own mount point: every server here is handed the
whole path and knows its own prefix, but somebody *else* mounts a proxy. So it
is told, and strips it, and the far end is asked about a name in its own tree.

### Checked against something that was not built from this source

Both halves were tested against an independent implementation on the host —
`/tmp/rvos/fsclient.py` and `fsserver.py`, written from the format above. If
the two agree, the format is a format and not an accident of one compiler.

The demonstration worth keeping is the host writing to the guest's *network
stack* through nothing but open/write/read/close:

```
$ python3 fsclient.py
open /net/ctl -> 0
write "listen 99" -> 9
read -> b'ok 3\n'
--- the guest status, read remotely ---
tcp 3 listen       :99
```

### Two machines, one wire

The host-side implementations prove the format. Two guests prove the thing.
`-netdev socket` is a plain layer-2 cable between two QEMUs — no slirp, no
gateway, nothing else on the segment — and both images are identical, so the
address has to become a property of the running system rather than of the
build. That is one more line in `/net/ctl`, and the shell grew `write`, which
is how one speaks to a control file from a prompt.

```
A$ write /net/ctl address 10.0.2.20
  net: this machine is now 10.0.2.20
A$ /EXPORTFS.ELF
  [exportfs] exporting this namespace on port 564
```
```
B$ write /net/ctl address 10.0.2.21
B$ import 10.0.2.20 564 /a/
  arp: who has 10.0.2.20?
  arp: 10.0.2.20 is at 52:54:00:12:34:56
  tcp: SYN -> 10.0.2.20:564
  tcp: [test] dropping this segment before it reaches the card
  tcp: timeout, retransmitting (attempt 2)
  tcp: connection established
mounted

B$ cat /a/README.TXT
rvos readme
===========
Educational RISC-V microkernel with FAT16.

B$ cat /a/proc/tasks
0  blocked  fs
2  running  proc  (me)
518  blocked  /EXPORTFS.ELF        <- A's exportfs, seen from B
```

The deliberate loss is still armed, so B's SYN was dropped and the
retransmission timer recovered it — across a real link to another machine
this time, rather than a loopback.

And then the point of all of it. B writes one line to a file and *A's* TCP
stack opens a port:

```
B$ write /a/net/ctl listen 99
ok 3
B$ cat /a/net/status
address  10.0.2.20/24
tcp 1 listen       :564
tcp 2 established  :564 <-> 10.0.2.21:40001
tcp 3 listen       :99
```

A's own console, independently:

```
A$   tcp: listening on port 99
```

Nothing on B knows it is talking to another machine; nothing on A knows its
caller is one. `write` opens a name and puts bytes in it.

### No authentication whatsoever

Anyone who reaches the port gets the namespace, and can write to it. Plan 9
has `factotum` and an auth file descriptor in the mount call; this has
nothing. That is a sentence in this README rather than a `TODO` in the source
because it is not a detail — it is the difference between a demonstration and
a system.

## A disk that is a disk

Everything above ran off a FAT16 image that QEMU copied into guest RAM with
`-device loader`, which the filesystem server read with a memcpy from a window
mapped into it alone. It is a virtio-blk device now, driven by `srv_blk.c`
inside that same task — the arrangement the network has had since stage 15,
where `srv_net.c` drives the card and `net_ip.c` speaks the protocols. Neither
pair can reach into the other, and neither driver has the kernel on its data
path.

The shape of `fat16.c` barely changed, which is the whole argument for its
having had a `blk_read()` in the first place.

A request is three descriptors chained: a header the device reads, a sector it
writes (for a read) or reads (for a write), and one status byte. The chain is
the point — they are one request because of the NEXT flag, not because they
are adjacent. And the `WRITE` flag means *the device writes here*, so it is
set for a **read**; backwards, it is a request the device accepts and a buffer
it never touches.

**Polled, not interrupt-driven, and on purpose.** The obvious thing is to wait
for the interrupt in `sys_recv`, as the network driver does. But the
filesystem server is already inside a client's request when it reads a sector,
and a `sys_recv` there is as likely to hand it a second client's request as
the interrupt it wanted — the multiplexing problem `import` had to solve, and
which the filesystem has no reason to take on. It yields between checks, so
nothing is starved, and the request completes in the time QEMU takes to copy
512 bytes.

**What it cost.** Both driver tasks now have the whole virtio-mmio range
mapped, so they can reach each other's registers — a real loss of the
isolation the earlier stages were careful about. Handing each one only its own
slot means the kernel deciding which slot that is, which means the kernel
knowing what a virtio-net is. The device tree is the way out of that and it is
not here.

**Checked by changing the image and looking.** The decisive test is not that
the guest can read a file; it is that the guest reads *the file the host just
changed*:

```
$ mcopy -o -i build/fat16.img new.txt ::/README.TXT
$ make run
rvos$ cat /README.TXT
CHANGED ON THE HOST at 03:42:10
```

and the other direction, since a driver that only reads is half a driver: the
guest wrote a sector, and the bytes were in the host's image file afterwards.

```
  [fs] wrote sector 16383
$ dd if=build/fat16.img bs=512 skip=16383 count=1 | head -c 45
rvos wrote this sector through virtio-blk
```

That was a temporary probe rather than something the system does — `blk_write`
is there and correct, and nothing calls it yet, because FAT16 writes are their
own piece of work.

## Writing to the disk

The filesystem has been read-only since stage 4. What makes writing different
from reading is worth stating: a read that goes wrong returns the wrong bytes,
and a write that goes wrong leaves a volume that no longer describes itself.
Three structures have to agree — the directory entry, the allocation chain and
the data — and the FAT is duplicated on the volume precisely because it is the
one thing nobody can reconstruct. Lose a chain and the data is still there and
unreachable. So every change to it is made to both copies.

**Whole files, not edits.** A file is read into a buffer and written back from
one, never modified in place, which matches the read side and removes an
entire class of bug: there is no partial state to be interrupted in. The cost
is that a file must fit in the buffer, which is the same 16 KiB limit that has
always been there.

**Order matters.** A rewrite allocates a fresh chain, writes the bytes, points
the directory entry at it, and only then frees the old chain. Interrupted
anywhere, the volume has a stale entry rather than one pointing at clusters
already handed to something else.

`create` is its own operation rather than a flag on `open`, because `open` has
no flags and giving it some would mean every server growing an opinion about
them. A server that cannot create anything answers -1 and nothing else
changes. Deleting goes through `ioctl`, which is what that call is for.

```
rvos$ create /A.TXT first file
rvos$ create /A.TXT overwritten, and shorter
rvos$ cat /A.TXT
overwritten, and shorter
rvos$ rm /B.TXT
```

### Checked by things that did not write it

The guest's own `cat` proves nothing about a filesystem. Three readers on the
host do:

```
$ mtype -i build/fat16.img ::/A.TXT
overwritten, and shorter
$ fsck.fat -n build/fat16.img
build/fat16.img: 13 files, 88/16223 clusters       <- no complaints
```

`fsck.fat` is the interesting one: it compares the two FAT copies against each
other and against the directory, and would say so if rvos had updated one and
not the other, or left a cluster allocated to nothing.

And the raw table, read by hand, for a file large enough to need a chain:

```
entry: first cluster 83, size 2652
chain in FAT #1: [83, 85, 86, 87, 88, 89] -> terminator 0xffff
FAT #2 agrees: True
```

It skips 84 because 84 was taken: the allocator looks for a free entry, not
the next one.

### The test that had not been run

Every file written up to that point was under 512 bytes — one cluster — so the
*links* in a chain had never been written at all. Each file's FAT entry was a
lone end-of-chain marker. `fsck` approved of them, `mtools` read them back,
and none of it said anything about the line that joins one cluster to the
next. The multi-cluster file above exists because that gap was noticed while
writing this paragraph, not while writing the code.

## Directories

The filesystem has seen only the root since stage 4. FAT16 has one structural
oddity that makes adding the rest more interesting than it sounds: **the root
directory is not a directory**. Every other one is an ordinary file — a
cluster chain whose contents happen to be 32-byte entries — but the root is a
fixed run of sectors outside the data area, with no chain and no entry
anywhere describing it.

So the driver is written against one function, "the n-th sector of a
directory", where a first cluster of 0 means the root. That is the only place
the difference is known, and every walk, listing, lookup and allocation is
written once rather than twice:

```c
static int dir_sector(uint16 dir, uint32 n, uint32 *lba)
{
    if (dir == 0) { ... fs.root_start + n ... }      /* not a chain */
    ... follow the chain n / sec_per_clus links ...
}
```

Walking a path is then what it should be: each interior component must exist
and be a directory, and a file cannot be walked through. `/DOCS` and `/DOCS/`
name the same thing.

A directory answers `read()` with its listing and a file with its bytes, and
the only way to tell which a name is, is to ask. The server tries the listing
first — a name that is a directory is never also a file.

```
rvos$ cat /DOCS
./
../
NOTE.TXT  (23 bytes)
rvos$ cat /DOCS/NOTE.TXT
a nested note in /DOCS
```

`.` and `..` are shown rather than hidden. They are what is on the disk.

**Making one.** A directory is a file whose contents are entries and which
begins by describing itself and its parent. The parent of a directory in the
root is written as cluster **0** — the root has no entry anywhere for `..` to
point at, and this is the same 0-means-root that `dir_sector` uses. Read out
of the image afterwards:

```
SUB at cluster 82
  entry 0: b'.          ' attr 0x10 first 82
  entry 1: b'..         ' attr 0x10 first 0
```

Removing one requires it to be empty, and empty means nothing besides those
two entries.

### Checked in both directions

The test worth having is not that rvos can read its own work. It is that rvos
wrote into a directory **mtools** created, and mtools read a directory **rvos**
created:

```
rvos$ mkdir /SUB
rvos$ create /SUB/DEEP.TXT written into a directory rvos made
rvos$ create /DOCS/EXTRA.TXT added to a directory mkdisk made
```
```
$ mdir -i build/fat16.img ::/SUB
.            <DIR>     1980-01-01
..           <DIR>     1980-01-01
DEEP     TXT        35 1980-01-01
$ mtype -i build/fat16.img ::/DOCS/EXTRA.TXT
added to a directory mkdisk made
$ fsck.fat -n build/fat16.img
build/fat16.img: 13 files, 83/16223 clusters      <- no complaints
```

and after deleting them again, `fsck` counts the clusters back down.

## The shell stops knowing the commands

`cat` was a builtin, and so were `ls`, `mkdir`, `rm` and the rest. They are
programs on the disk now, in `/BIN` — a directory that can exist because the
previous stage made directories possible — and the shell finds them by name:

```
rvos$ ls
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
DOCS/
BIN/
rvos$ wc /README.TXT
4 13 105  /README.TXT
rvos$ free
free 12017 of 12203 pages (48068 KiB free)
```

`ls`, `cat`, `echo`, `mkdir`, `rm`, `cp`, `mv`, `wc`, `ps`, `free` — ten ELF
files, none of which the shell has heard of. A bare word becomes
`/BIN/<word>.ELF`; a word with a slash in it is taken as written.

**Nothing uppercases anything.** FAT16 stores an 8.3 name upper-cased and
looks one up the same way, so `ls` finds `/BIN/LS.ELF` without a line of shell
code knowing that this filesystem shouts.

### `wait` had to come first

Without it this change makes the system worse, not better: the child and the
shell write the same place, so the prompt comes back in the middle of the
program's first line. That was a tolerable wart when `cat` was a builtin and
is not one when it is not.

```c
SYS_WAIT    /* block until that task is gone */
```

Polling `sys_alive` would do, and did for a moment, but it costs a scheduling
slice per check and a shell running a command spends all its time there. The
kernel version is a field and four lines in `task_retire`: anyone blocked on
an id is woken when that id is retired. `&` at the end of a line skips the
wait, which is how a program that never exits gets started:

```
rvos$ /BIN/NETD.ELF &
[4358]
```

### What a program on the disk has instead of a library

It cannot link against `ulib`: that lives in the shared user text of
`kernel.elf`, which is not mapped into anything loaded at run time. So each
program carries its own copy of the half-dozen functions it needs, and
`prog/lib.h` is how they stopped being written out ten times — included, not
linked.

Everything in it writes through `vfs_say`, which is a path, which means the
output follows whatever `/dev/console` is bound to where the program was
started. The same `/BIN/GET.ELF` prints on the serial console from the local
shell and down the connection from the one over TCP:

```
rvos# get example.com
  [get] example.com is 104.20.23.154
HTTP/1.1 200 OK
...
  [get] 828 bytes
```

### What is still a builtin, and why

Four things, and they have something in common: `bind`, `mount`, `unmount`
and `import` change *this shell's own namespace*. A separate program could do
it — a child shares its parent's namespace rather than copying it, so the
change would stick — but it would be a surprising way to write it. And `echo`,
which has nothing to start.

### What it does not have

No pipes and no redirection, so a command's output goes where its namespace
says and nowhere else; `>` would be a bind, which is the interesting way to
build it and is not built. No exit status, because nothing returns one — the
shell learns only that a task is gone. `mv` is a copy and a delete, since the
filesystem has no rename; within one directory that is eleven bytes of work
being done as a whole-file copy.

## Speaking enough telnet

`nc` sends the bytes you type and nothing else. `telnet` sends the bytes you
type *and a conversation about how to send them*, mixed into the same stream
and marked by IAC — 255, "interpret as command". Until this stage the shell
took that conversation for a command line, and said so:

```
rsh: no such command: \xff\xfd\x03\xff\xfb\x18ls  (try `help`)
```

Two rules keep the fix small. A literal 255 in the data is sent as two of
them, so `IAC IAC` is one byte of data. And a negotiation is answered **only
when it is an offer** — `WILL` and `DO` — because answering a refusal with a
refusal is how two implementations negotiate for ever.

Everything is refused, which leaves the connection in RFC 854's default mode:
the client keeps local echo and sends whole lines. That is the same
arrangement `nc` gives by letting a terminal do it, so one code path serves
both and the shell needs no idea which is on the other end.

Line endings are then whatever the client believes in — `nc` sends LF, telnet
sends CR LF, and a telnet sending a bare carriage return sends CR NUL. Leading
remnants of any of them are dropped rather than reported as empty lines.

```
$ telnet 100.95.222.7
Trying 100.95.222.7...
Connected to 100.95.222.7.

rvos — you are on the guest, over its own TCP stack.
type `help`. connection 0

rvos# ls
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
DOCS/
BIN/
rvos# wc /README.TXT
4 13 105  /README.TXT
```

Checked against three clients: GNU `telnet`, `busybox telnet`, and one written
by hand that sends an offer, a refusal and a subnegotiation interleaved with
data, to see that the offer is answered, the refusal is not, and the
subnegotiation is skipped whole.

### Where it listens, which is not a detail

The forwarded ports bind to `127.0.0.1` unless `HOSTIP` says otherwise.
Writing `hostfwd=tcp::5556-:23` — with the host address left empty — binds to
every interface, and on a machine with a public address and no firewall that
puts an unauthenticated shell on the internet, along with a filesystem anyone
may write to and a `/net/ctl` anyone may use to open outbound connections from
that address. It did exactly that here for about a day, and it was noticed
only because somebody asked how to reach the guest from outside.

```
make run HOSTIP=100.95.222.7      # a private overlay address
make run HOSTIP=0.0.0.0           # everybody, and you mean it
```

The right answer for wanting it reachable is authentication, which this still
does not have.

## Long names

An 8.3 name is eleven bytes of an alphabet chosen in 1981. `ПРИВЕТ.TXT` came
out of it as `ПРИВ.TXT` — truncated at eight *bytes*, which is four Cyrillic
letters — and the host, decoding those bytes as a code page, showed
`ðƒðáðÿðÆ`.

A long name is stored beside the short one, in the entries *immediately
before* it, marked with attribute `0x0F`: read-only, hidden, system and volume
label at once, a combination no real file has and that every driver written
before 1996 already skipped. That is the whole trick. The extension is
invisible to code that does not know about it, which is why the format could
grow one without becoming a different format.

Three details are easy to get wrong and all three are load-bearing:

- **the entries are stored in reverse.** The one flagged `0x40` comes first on
  disk and carries the *last* thirteen characters of the name;
- **a checksum of the eleven-byte short name is repeated in every long
  entry.** An old driver that renames a file the old way leaves a chain that
  no longer matches, and it is then correctly ignored rather than believed;
- **unused character slots after the terminator are `0xFFFF`**, not zero.

Anything above the basic plane is a surrogate pair, which is how a format
built around UCS-2 carries a code point invented after it.

```
rvos$ cp /README.TXT "/копия с кириллицей и emoji 🚀.txt"
rvos$ ls
a rather long file name.txt  (35 bytes)
копия с кириллицей и emoji 🚀.txt  (35 bytes)
```

Short names are still generated, because every reader expects one, but they
are *derived*: the printable ASCII of the long name, upper-cased, cut to six,
then `~1`, `~2` … until unique. A name with nothing ASCII in it becomes
underscores, so two Cyrillic names are told apart only by the tail —
`______~1` and `______~2`, which is honest about what the old alphabet can
hold. A name that fits 8.3 exactly gets no long entries at all, so a volume of
ordinary names looks exactly as it did before this stage.

Directories are walked a *slot* at a time now — entry `i` is at sector
`i/16`, offset `(i%16)*32` — which makes every scan, lookup and allocation
linear instead of a nest of sector loops. One cached sector is what keeps that
from costing sixteen reads per sector.

Two things had to move out of the way. `VFS_PATH_MAX` was 32, and
`/DOCS/длинное имя.txt` is 40 bytes before it starts; it is 128 now. And both
shells learned quotes, which stopped being optional the moment a file could be
called `a rather long file name.txt`.

### Checked against an implementation that is not this one

`mtools` has its own LFN code, and the test is that each side reads what the
other wrote:

```
$ mcopy -i build/fat16.img f "::/a rather long file name.txt"
rvos$ cat "/a rather long file name.txt"
written by mtools with a long name

rvos$ cp /README.TXT "/только кириллица без эмодзи.txt"
$ mtype -i build/fat16.img "::/только кириллица без эмодзи.txt"
rvos readme
$ fsck.fat -n build/fat16.img
build/fat16.img: 23 files, 197/16223 clusters
```

and the chain itself, read out of the image by hand:

```
LFN seq=3 LAST sum=48 " 🚀.txt"
LFN seq=2      sum=48 "лицей и emoji"
LFN seq=1      sum=48 "копия с кирил"
SFN b'______~1TXT'  sum=48 size=35
```

Reverse order, one checksum through all four entries, and the surrogate pair
intact. `mtools` shows that name as `копия с кириллицей и emoji __.txt` and
cannot open it by name — it converts to the local code page, where U+1F680
does not exist. That is mtools' display, not the disk: the raw dump above
decodes the pair correctly.

### What it does not do

A directory does not grow. Long names take several entries each, so a
directory fills sooner than it used to, and when it is full a create fails
rather than allocating another cluster for it. Names are held to 96 bytes of
UTF-8, and one longer is refused rather than quietly shortened. Case folding
is ASCII-only, which is exactly what a FAT volume promises: `Файл.txt` and
`файл.txt` are two names.

## A reply is not an event

`sys_recv` blocks until *anybody* sends and reports who it was, and in the
kernel that is literally "take the head of the queue". It is open, and for the
two things it was written for that is not a choice but a definition: a server
does not know who will ask next, and the design went further and made the same
call report interrupts and alarms as well — three kinds of event, one blocking
primitive. That is why this system has no `poll`. `sys_recv` *is* the poll,
and the network driver has waited on a card, a clock and its clients in one
place since stage 16.

The mistake was somewhere else, in the client library every program includes:

```c
static inline int vfs_call(int dst, struct vfs_req *r)
{
    sys_send(dst, r, sizeof *r);
    sys_recv(r, sizeof *r);        /* the next message, from anybody */
    return r->result;
}
```

That is not waiting for an event. It is waiting for a **reply**, and a reply
has a sender that is known in advance. Taking something else is not a race; it
is an answer to a different question.

The assumption underneath — *the next message to arrive is mine* — holds only
while a task has at most one thing outstanding and is not itself a server.
Every task satisfied that until `import`, which holds a read parked on the
network while sending to it; and a shell that serves its children's console
breaks it thoroughly, which is how an attempt at that turned into four
separate failures with one cause.

```c
SYS_RECVFROM    /* a0 = sender, a1 = buffer, a2 = cap */
```

Three changes make it work, and each of them is about *not consuming* things:

- `dequeue_sender` takes the head for an open receive and searches for a named
  one otherwise, **unlinking from the middle** — everybody else stays where
  they were. A message nobody asked for is kept, not thrown away;
- `ipc_send` grows a filter. A task parked in a closed receive is blocked, but
  not available to anyone it did not name, so a sender parks instead of being
  delivered. That also makes `sys_trysend` mean the right thing for a server
  answering a request it parked earlier;
- the interrupt and timer flags are sticky, and a closed receive leaves them
  alone. The next open receive finds them.

The buffering that all of this needed turned out to be in the kernel already.
`wait_sender` has been there since stage 3; it was simply always drained from
the head.

And the hazard the primitive introduces is closed in the same breath: a closed
receive can name a task that then dies, so `task_retire` wakes anyone waiting
to hear from it with -1. Without that, a client of a server that crashed would
wait for ever with somebody else's message queued behind it.

## What a message costs

After two stages of surgery on the IPC there was not a single number anywhere
saying what any of it cost, and when the shell wedged there was no way to ask
which module had stopped answering. Both gaps close with one thing: an
operation every server answers immediately, with 0 and nothing else, so that
the only information in the reply is its own timing.

```
rvos$ ping /
  ioctl: 92.88 us per round trip  (2000 of them in 185 ms)
  open+close: 173.10 us per round trip
rvos$ ping /r3/                     <- a namespace on another machine
  ioctl: 25339.40 us per round trip
```

Same command, same call, the same five-operation interface — and the number is
the only thing that says one of them crosses a TCP connection.

### It found something before it measured anything

The first run said 127 µs to the filesystem and 182 µs to `/proc`, which made
no sense: the same request, the same size, two servers doing the same nothing.
The difference was **where they sat in the round-robin**. Three tasks left over
from the boot demonstration were finishing with

```c
for (;;)
    yield();
```

which is not idling — it is *busy*. Every message in the system paid for them
in context switches on the way past. They block now, and `/proc` went from
182 µs to 116 µs without touching a line of the proc server. The cost had been
there since stage 9 and nothing had ever looked.

### The floor underneath

`ping` measures the whole interface. `bench` measures two tasks and one
message with nothing on top, so the difference is what the interface costs
over the primitive it is built on:

```
rvos$ bench
  8 bytes, open recv  : 85.68 us
  8 bytes, closed recv: 85.79 us
  (672 bytes is one vfs_req)
  vfs_req, closed recv: 89.92 us
```

Three things fall out of those four lines:

- **the closed receive is free.** 85.68 against 85.79 µs — searching the
  sender queue instead of taking its head does not show up at all;
- **the copy is nearly free.** 672 bytes across two address spaces, twice,
  costs about 4 µs — which is worth knowing, because "IPC copies" has been
  the stated price of isolation since stage 9 and it is not where the money
  goes;
- **the interface is nearly free.** 92.9 µs for a full resolve-send-dispatch-
  reply against 89.9 for the bare message: about 3 µs, three per cent. Every
  open, read and write in this system is a message and a rounding error.

So a message costs 86 µs and everything built on it costs 3 — and then the
obvious conclusion from that is wrong, which is the most useful thing in this
section.

### Is that real time, or the emulator's?

Real. `rdtime` reads the CLINT, and QEMU without `icount` drives it from the
*host* clock rather than from instructions executed, so the guest's stopwatch
is a stopwatch. Checked from outside: the guest reported 430 ms for 4000 round
trips and the host measured 457 ms for the whole command, the difference being
the shell, an ELF load and a 50 ms polling interval.

But real time spent on emulated execution is not the same as the time that
work would take, and the emulation is **not uniformly slow**. So `bench`
measures one more thing — a two-instruction loop, written in assembly so the
compiler cannot have an opinion about it:

```
  plain code          : 861 million instructions per second
```

Nearly native: a tight loop is translated once and then runs as host code.
Which means a round trip at 86 µs costs about **74,000 instruction-times** —
and the actual work in it is two `ecall`s, two saves and restores of
thirty-two registers, a walk of the scheduler, and a copy. Call it a couple of
thousand instructions. The other seventy thousand are not in the guest at all.

They are in the two writes to `satp`. Changing an address space makes QEMU
throw away its software TLB, and every memory access afterwards takes the slow
path until it refills. Real silicon has a real TLB and an ASID; there, a
context switch is on the order of a microsecond, not eighty-six.

### Which inverts the conclusion

"The switch dominates, the interface is free" is an artifact of the emulator.
The interface's 3 µs is plain code running at nearly native speed, so it would
cost about 3 µs on a real processor too — while the 86 µs switch would not.
On hardware the two would be the same order, and quite possibly the other way
round.

The measurements are honest and the ratio between them is not transferable.
That is worth more than the numbers: a benchmark that cannot say what it is
measuring is exactly as useful as the first version of this one, which ran
during the boot demonstration and reported the primitive as slower than the
interface built on top of it.

They also move with load — `/dev/console` measured 128 µs on a quiet system
and 187 µs while somebody was typing at it over telnet.

The first version of `bench` ran at boot, and reported the bare primitive as
*slower* than the whole interface built on top of it — because it was
competing with a name being resolved, a program being loaded and a
retransmission timer. The number was not wrong about anything except what it
was measuring, which is the ordinary way for a benchmark to lie. It runs on
demand now.

And one number invites a question rather than answering it: 25 ms across a
mounted remote namespace is suspiciously close to the 20 ms retry deadline the
network server arms when a reply is refused. That suggests every remote round
trip is waiting for one, which would be worth chasing.

## Two panels, in colour

```
  Left  File  Command  Options  Right
┌─ / ──────────────────────────────────┐┌─ /BIN ───────────────────────────────┐
│ Name                         Size    ││ Name                         Size    │
│ HELLO.TXT                          54││/..                           DIR     │
│ README.TXT                        105││ CAT.ELF                          5608│
│/DOCS                         DIR     ││ CP.ELF                           5792│
│/BIN                          DIR     ││ ECHO.ELF                         5256│
│                                      ││ EXPORTFS.ELF                     8776│
│                                      ││ FREE.ELF                         5400│
│                                      ││ GET.ELF                          6528│
└─ HELLO.TXT ──────────────────────────┘└─ .. ─────────────────────────────────┘
Hint: everything here is a file, including this.
/$
1Help 2Menu 3View 4Edit 5Copy 6RenMov 7Mkdir 8Delete 9PullDn 10Quit
```

Blue panels, cyan frames, white directories, green programs, the cursor black
on cyan. It is a copy of a program from 1994, and the copying is not
decoration: a frame, a header and a footer are three more things the eye can
find without reading, and the key strip along the bottom is the only
documentation a full-screen program ever gets to show. Arrows and Tab move,
Enter opens, and F3 F5 F7 F8 F10 do what the strip says they do.

`/BIN/MC.ELF` is an ordinary program on the disk. It opens `/dev/console` and
reads and writes it; it opens directories and reads them. Those are the same
five calls `cat` uses, and nothing in it is privileged.

What makes it full-screen rather than scrolling is that **a terminal is also
just bytes**. Cursor positions and colours are escape sequences, and they
travel unchanged through every layer here — the console server, the TCP stack,
telnet's framing — because none of those layers has an opinion about what the
bytes mean. That was checked before a line of it was written: `0x1b[1;31m`
goes in one end and comes out the other, and `0x1b` cannot collide with
telnet's `0xff` any more than it can with UTF-8.

### The listing had to become regular first

A directory used to read as `README.TXT  (105 bytes)`, which is pleasant to
look at and unpleasant to parse — and names can contain spaces and brackets
now. It reads as this instead:

```
- 105 README.TXT
d 0 DOCS
```

Two fields and then the name to the end of the line, which needs no rules at
all. Making it pretty moved into `ls`, which is where presentation belongs.
The screen above is `mc` reading exactly those lines.

### Two things it had to arrange

**Character mode.** A terminal reached over telnet is in *line* mode: the
client keeps local echo and sends nothing until Enter, so an arrow key would
arrive only after one, if at all. Two telnet options fix that, and the program
sends them **itself** — it can, because `/dev/console` *is* the connection, so
a negotiation is only more bytes. It withdraws them on the way out, and the
shell finds things as it left them.

**Knowing whether to.** On the serial line there is no telnet and those bytes
would be printed as rubbish. So the program asks:

```c
sys_resolve("/dev/console", real, sizeof real, 0);
/* if `real` begins with /net/tcp/, the console is a connection */
```

Resolution reports not only which server answers for a name but *what name to
ask it about*, and only a connection is called `/net/tcp/N`. That is the
namespace answering a question about itself, and it is the alternative to
guessing. Verified from both ends: over TCP the negotiation goes out; on the
serial console the stream contains no `0xff` at all.

### A message is ninety microseconds, and it shows

One character written is one message. A screen is about two thousand of them,
which at the measured cost would be a fifth of a second per keystroke. So the
screen is built in a buffer and sent in a handful of calls — and that is the
first place in this project where the number from `bench` changed how
something was written rather than merely describing it. (The next section
takes the same number further: the buffer moved into a library, which sends
not the whole screen but the part of it that changed.)

### Asking the terminal how big it is

A framed screen has to know where its edges are, and there is nobody in this
system to ask: no window manager, no `TIOCGWINSZ`, and the console server
counts bytes rather than columns. So the program asks the *terminal*, in the
terminal's own language, and it asks twice:

```c
/* telnet's answer, if this is telnet: IAC DO NAWS, and the client sends the
   size now and again whenever the window changes */
"\xff\xfd\x1f"
/* and the answer any terminal can give: go as far right and down as you can,
   then report where that turned out to be */
"\x1b[999;999H\x1b[6n\x1b[H"
```

The first arrives as a subnegotiation, the second as `ESC [ h ; w R` through
the same keyboard the arrow keys come through — so both are handled in
`getkey`, next to the arrows, and a resize is just another key (`K_RESIZE`,
which redraws). Neither can hang: if nothing answers, 80×24 stands, which is
what the serial line does. Checked at 80×24 and at 58×22, and the frames close
in both.

The mistake that cost the most time here was not in the program. Its output
looked one column short in my own test harness, and the harness was a
byte-per-cell renderer — every box character is three bytes, and it was
charging three columns for each. The guest was right and the tool was wrong,
which is worth remembering the *next* time something only looks broken.

### What it does not do

The menu bar along the top is painted, not wired; the keys under it are the
ones on the strip. And one session at a time, which by now is a familiar
sentence.

It could not run anything either, and that sentence stood here for several
stages: "a program loaded from the disk cannot start another, because `spawn`
lives in the shared user text of the kernel image". True, and the conclusion
was wrong — see below.

## The screen becomes a data structure

Everything full-screen written here had done the same three things by hand: a
buffer of escape sequences, a column counter that knew UTF-8, and a parser for
the bytes an arrow key is made of. None of that is about files, and all of it
is the job of a library that has existed since 1980. So `prog/curses.h` is a
subset of X/Open curses — the real names, the real constants, the real octal
key codes — and `mc` is now written against it:

```c
initscr();
cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
start_color();
init_pair(P_PANEL, COLOR_WHITE, COLOR_BLUE);

WINDOW *w = newwin(LINES - 4, COLS / 2, 1, 0);
wattrset(w, COLOR_PAIR(P_FRAME) | A_BOLD);
box(w, 0, 0);
mvwaddstr(w, 1, 1, " Name");
wrefresh(w);
```

The program lost two hundred and fifty lines and gained page-up, page-down,
Home and End, which cost four lines each once there was something that could
tell a key from a byte.

### The idea worth copying is the second screen

A curses program does not write to the terminal. It writes into an array of
cells; `doupdate` compares that array against a second one holding what the
terminal is believed to be showing, and sends the difference. Here that is not
a nicety. Moving the cursor down one row in a file panel changes two rows out
of twenty-four, and the same key on the same screen costs:

|                        | by hand   | curses   |
|------------------------|-----------|----------|
| first full screen      | 8732 B    | 7554 B   |
| one arrow key          | ~4344 B   | ~138 B   |
| keystroke to redrawn    | 5.8 ms    | 2.6 ms   |

Thirty-one times fewer bytes for a keystroke, and the round trip that carries
them — measured from the other end of a real connection, key sent to last byte
back — is less than half as long. **The library is faster than the code it
replaced because it knows more, not less.** The hand-written version could not
have done this: it had no idea what was already on the screen.

The other saving is smaller and comes from the same place. A cell holds a
character *and* its colour in one 32-bit `chtype`, so the update loop emits an
SGR sequence only when the attribute actually changes from one cell to the
next. Colour stops being something you remember to turn off.

```c
typedef unsigned int chtype;    /* 21 bits of code point, 11 of attribute */
#define A_BOLD      0x00200000u
#define COLOR_PAIR(n) (((chtype)(n) << 26) & A_COLOR)
```

The original spends eight bits on the character because it predates Unicode by
a decade. This one spends twenty-one, because a box corner is U+250C and
drawing a frame is the first thing anybody asks a screen library for. The
`ACS_ULCORNER` names are kept anyway — a program written against them ports
both ways.

### What initscr knows, and how

Real curses learns the terminal from terminfo and the line discipline from
termios. Neither exists here, so `initscr` asks the namespace what
`/dev/console` resolves to; if the answer is called `/net/tcp/N` then the
console is a connection, and the library negotiates character mode itself and
asks the far end how big it is. `cbreak()` and `noecho()` are one telnet
negotiation, because a telnet client is the line discipline this system does
not have.

That is a better answer than terminfo gives, and worth saying why: terminfo is
a guess about what the far end probably is, keyed on a string the far end sent
about itself. Resolution is not a guess. It is the system reporting which of
its own servers will answer for a name.

### A window is a view, not a page

`newwin` returns a pen with a margin — an origin and a clipping rectangle —
and every window draws into the one virtual screen. Real curses gives each
window its own array of cells, which is what lets an overlapping window
remember what was underneath it. That costs a screen of memory per window,
there is no `malloc` here, and nothing in this system overlaps anything. So
`wrefresh(win)` does not mean "paint this over that"; it means "I am done,
update the screen". `panel(3)` is the library that stacks windows properly,
and this is not it.

Also absent, and for reasons rather than by omission: `printw`, because there
is no vsnprintf; and `nodelay`, because the console server answers a read when
a key arrives and there is no way to ask it whether one is waiting. Blocking
is a server declining to answer yet, and it does not do partial refusals.

### The corner that cannot be written

The first bug report was "pressing Enter makes the screen double, downwards",
and the cause is a corner of terminal behaviour old enough that the original
curses has a workaround for it.

Terminals disagree about what happens when a character lands in the *last
column*. Some leave the cursor sitting there with the wrap pending until the
next character arrives, which is harmless. Some wrap immediately — and on the
bottom line, wrapping means the screen scrolls. The key strip is padded to the
full width, so every full redraw wrote that cell, and on a terminal of the
second kind the whole picture moved up one row.

The hand-written version did this too, five times in a five-keystroke session.
It got away with it because it redrew everything on every key: the damage was
painted over before anyone could read it, and all that was lost was the top
line, briefly. The differential update does not paint over anything. It sends
the difference between two screens and never looks at the terminal again, so
one silent scroll leaves the library's model one row out of register for the
rest of the session — and the next big repaint lands rows in the wrong places
and the result looks doubled. **The library did not introduce this bug; it
made a bug that was already there impossible to ignore.**

The fix is the one ncurses uses: the bottom-right cell is never written, and
the library records it as though it had been so that no later update tries
again. One blank cell in the corner, and the screen holds still.

Finding it needed a better tool than the last one. `screen.py`, the renderer
these captures go through, models cursor movement and nothing else — no
wrapping, no scrolling — so it showed a perfect screen while a real terminal
was sliding. A second one that does model both now reads the same byte stream
twice, once with each wrapping rule, and asserts that the two pictures come
out identical. That is the actual property being claimed, and it was worth
sixteen keystrokes to check: Enter, the viewer, PgUp, PgDn, Home, End, Tab.

### Two things the port found

The panel used to compute its list height from the screen height, arrive at
one row too many, draw the last entry and then paint over it with the bottom
frame. A window that knows its own size cannot make that mistake, and the
mistake was invisible until something else did the arithmetic.

And `mc` stopped fitting. A program with ten kilobytes of code is a
seventeen-kilobyte ELF file here: a page of padding before the text, so that
file offset and load address agree modulo the page size, and another between
text and data, so the two segments land on pages that can carry different
permissions. The filesystem server holds a whole file in one buffer and the
loaders read a whole ELF into another; both were 16 KiB, which is the largest
file this system could open. Both are 32 KiB now. It is the same coupled pair
of numbers with nothing enforcing it, only twice as large.


## A viewer and an editor, because there is nowhere to put them

MC runs its viewer and its editor as separate programs. This one cannot: a
program loaded from the disk has no way to start another. So F3 and F4 open
something that lives inside `mc` — and they are one piece of code, of which
the viewer is the half that does not change anything. F4 hands the other half
over, even from inside the viewer.

```
 Edit /README.TXT *   4/6
rvos readme
===========
EduHELLO from the editor.
  приветcational RISC-V microkernel with FAT16.
Stages: boot, timer, IPC, filesystem.

2Save 7Search 10Quit
```

Arrows, PgUp, PgDn, Home and End; F7 searches and wraps round the end once;
F2 writes; F10 leaves, and asks first if there is anything to lose. Typing
inserts, Backspace and Delete remove, Enter splits a line. The `*` in the
title bar is the only thing standing between you and the disk.

The model is the filesystem's own: **the whole file in one buffer**. That is
what the server does with it anyway — there is no seek here and no partial
write-back — so an editor built on anything cleverer would be pretending to an
interface this system does not have. An edit is therefore a `memmove` and a
rebuild of the line index, which for sixteen kilobytes at the measured 861
MIPS is about twenty microseconds. A gap buffer would be a data structure
carried around for a saving nobody could perceive. Saving is `create`, which
truncates, then the bytes, then `close`, which is what puts them on the disk —
the same three facts `cp` is built on.

A file too big for the buffer opened read-only and said so in the title bar,
because the alternative is an editor that silently cuts a file off at sixteen
kilobytes and calls it saving. There is no such buffer any more: the next
stage gave the system an allocator, and the editor asks for the size of the
file it is opening.

### The byte that is not a character

Point a viewer at an executable and the difficulty appears at once. In an
ELF file a byte over 0x7f is a byte, not the beginning of anything, and
decoding it as UTF-8 swallows the two or three after it — so the display stops
lining up with the file, which is worse than merely ugly. A sequence counts as
a character here only if its continuation bytes really are continuation bytes;
anything else is one byte, shown as a dot. Then `/BIN/CP.ELF` opens with
`.ELF` in the first four columns and `usage: cp <src> <dst>` findable with F7,
which is the test that it is reading the file rather than a theory about it.

Long lines are not wrapped — a 5 KiB line of machine code is one row, and the
arrow keys scroll the window sideways eight columns at a time. In the viewer
they move the window; in the editor they move the cursor. Moving something
invisible looks like nothing happening, and the viewer hides its cursor.

Verified from outside, which is the only verification worth the name: text
typed into `/README.TXT` through a telnet session came back through `mtype` on
the host reading the raw FAT16 image, Cyrillic and all, and `HELLO.TXT` — the
one edited and then abandoned at the "save? y / n" question — was byte for
byte what it had been.


## malloc, and the limits that were never a policy

Every buffer in this system was written into it at compile time. The loaders'
scratch space for an ELF file. The filesystem server's copy of an open file.
The editor's copy of the text. Those numbers are why `mc` stopped fitting and
had to be given a larger cage rather than let out of it — and a limit that is
a static array is not a policy, it is an absence.

**The kernel's whole contribution is one call.**

```c
SYS_SBRK = 25,      /* a0 = bytes, positive or negative -> the old break */
```

It moves the end of a window in the calling task's address space and answers
with where that end used to be. Growing allocates page frames and maps them;
shrinking unmaps them and gives the frames back, with an `sfence.vma` after,
because the task is about to go on running with the same `satp` and a mapping
that has gone away may still be in the TLB. If it runs out of memory partway
it unmaps what it just took, so a failed `sbrk` leaves the address space
exactly as it found it. That is thirty lines, and it is the entire privileged
part of having an allocator.

### The state has to be at a fixed address

Everything else is arithmetic, unprivileged, in `src/malloc.h` — and it has
one constraint that shaped it. User programs here share their text and not
their data: `spawn` lives in the shared user text and is *called by the
shell*, so a variable of its own would sit at an address mapped into the
loader's task and not into the caller's. An allocator with

```c
static struct mheap *heap;      /* faults the first time the shell allocates */
```

cannot work. So the heap describes itself: its header is the first thing in
the region it manages, at a fixed `UHEAP_BASE`, and the kernel maps that one
page when the task is created, the way it maps a stack. A page arrives zeroed,
and zero is what the allocator reads as "nothing here yet". One copy of the
code then works for whoever is running, in whatever address space they have.

The rest is the plain allocator out of the textbook: one free list in address
order, first fit, split on the way out, coalesced with both neighbours on the
way back in, whole pages asked for when nothing fits and whole pages returned
when the last block reaches the break. Not fast, nothing to lock — a task is
single-threaded and its heap is its own — and readable, which is the point.

One bug is worth recording because it is the kind this design invites. Growing
inserted the new pages into the free list, and inserting ran the "the tail is
free, give it back" rule, which handed the kernel the very pages the call had
just gone to get. `malloc` of anything larger than a page returned zero,
always. The fix is that growing inserts and does not trim, and trimming
happens only on `free` — with sixty-four kilobytes of hysteresis, so that a
program alternating one large allocation with one large free does not make a
syscall each time.

### What it took away

| was | is |
|-----|-----|
| `ELFMAX`, three static 32 KiB buffers | `spawn` reads the file into a buffer its own size |
| `FS_BUFSZ`, three static 32 KiB buffers | the fs server allocates per open file and grows on write |
| `ED_MAX`, 16 KiB in the editor | the editor asks for the size of the file |
| `ED_LINES`, 1024 | the line index grows with the file |

The kernel's static footprint fell from 313592 bytes of `.bss` to 117032 —
192 KiB, which is exactly the six buffers — while its text grew by 3.7 KiB for
the allocator and the syscall. Then the numbers stopped being interesting,
which is the real result: a 60000-byte file was opened in the editor, paged to
line 661 of 1353, typed into, saved, and read back on the host out of the raw
FAT16 image with the insertion at line 661 and a length of 60022. Nothing in
that sentence would have been possible two stages ago, and nothing in it
mentions a limit.

`free` reports the same 11971 pages before and after — the allocator gives
back what it borrowed, and a task that exits hands back the rest, because
`vm_free_task` already frees every page a task owns privately and a heap page
is exactly that.


### Where else it belonged, and where it did not

The allocator went in to remove limits, so the next question is which of the
remaining fixed arrays are limits and which are decisions. The IP stack held
the answer to both.

**A TCP control block was 7.9 kilobytes**, almost all of it three buffers: two
kilobytes to send, two to receive, and three slots of twelve hundred bytes for
segments that arrive out of order. Four of those is thirty-one kilobytes of
memory that a machine with no connections was carrying anyway. The buffers are
allocated when a block is claimed and freed when it is released, and the
out-of-order slots are allocated only when a segment actually arrives out of
order — which on a working link is never, so the rarest path in the stack had
been half its memory.

**The count of control blocks stayed fixed, and that is the decision.** A
stack that allocates a block for every arriving SYN can be pushed out of
memory by a stranger; it is the oldest denial of service there is and the
reason SYN cookies exist. A fixed four bounds what a peer can make this
machine spend. What is inside them is another matter — nobody can make this
machine hold more than four connections' worth of buffers, and an idle stack
now costs a few hundred bytes.

There is a pleasing consequence in the reassembly path. If the allocation for
a held-aside segment fails, the segment is dropped — which is exactly what the
code already did when its queue was full, and exactly what the protocol was
designed around. **An allocation that fails in TCP is not an error; it is a
lost packet.**

The filesystem server's directory listing was the one real bug found by
looking. It read into a static array of thirty-two entries, so the
thirty-third file in a directory did not exist as far as anything above that
line was concerned — silently, with nothing reporting a truncation. It grows
now until the driver stops filling it, which is the only way to know that it
did not truncate. A directory of sixty files was the test, and sixty came
back.

What stayed static, on purpose:

| stayed | why |
|--------|-----|
| `ns_pool` in the namespace code, 9.8 KiB | it is kernel memory, and the kernel has a page allocator and no heap — `malloc.h` runs in user tasks by construction |
| the task table, 7.8 KiB | a task id is a slot number and a generation; the table is the mechanism, not a buffer |
| the trap stack, 4 KiB | it has to exist before anything can allocate anything |
| the IP stack's packet scratch, 3.6 KiB | one buffer used on every frame in and out — allocating it per packet would be pure cost |
| the console's key ring, 256 B | it catches keystrokes at interrupt time |

The kernel's `.bss` finished at **83160 bytes, from 313592 four commits ago** —
a quarter of what it was. And it costs nothing measurable: five runs of `ping /`
on each build put the message round trip at 94–117 µs before and 94–111 µs
after, and `open+close`, which now allocates and frees a whole-file buffer on
every open, at 177–217 µs before and 182–218 µs after. The allocation is lost
in the two messages around it.


### There is no defragmenter, and there cannot be one

Not an oversight. `malloc` hands the caller a bare pointer, so a block that is
in use cannot be moved: nothing knows where the pointers to it are. The
systems that did compact their heaps — Mac OS before X, 16-bit Windows — did
not hand out pointers at all. They handed out *handles*, and you locked one to
get an address and unlocked it again so the manager could shuffle the block
underneath you. The other way is a garbage collector, which can move things
precisely because it knows every reference. C's contract forbids both.

What there is instead is coalescing: when a block comes back, it is joined to
its free neighbours. `/BIN/FRAG.ELF` is what that is worth, and what it is
not. This is what it printed when the heap was one free list and every object
carried a header:

```
at rest                    taken 4K    free 3K    in 1 pieces    largest 4064
2000 x 64 bytes            taken 160K  free 3K    in 1 pieces    largest 3216
all freed                  taken 4K    free 3K    in 1 pieces    largest 4064
every second one freed     taken 160K  free 81K   in 1001 pieces largest 3216
after asking for 4096      taken 168K  free 85K   in 1001 pieces largest 7296
200 x (1K then 48 bytes)   taken 216K  free 0K    in 1 pieces    largest 336
only the 1K ones freed     taken 216K  free 203K  in 201 pieces  largest 1040
after asking for 64K       taken 284K  free 207K  in 201 pieces  largest 4416
everything freed           taken 4K    free 3K    in 1 pieces    largest 4064
```

Read the third line first: two thousand blocks taken and given back leave the
heap in **one** piece and 4 KiB taken from the kernel. Coalescing is not a
detail; without it that line would read "in 2000 pieces" and the next
allocation of anything larger than 64 bytes would grow the heap for ever.

Then read the second group. Two hundred kilobyte blocks, each with a
forty-eight byte block wedged behind it, and then the kilobyte ones freed:
**203 KiB free, in 201 pieces, and the largest is 1040 bytes.** Asking for
64 KiB had to go to the kernel for 68 KiB more, while holding three times that
much idle. That is external fragmentation, and no allocation policy fixes it —
best fit rather than first fit changes which hole is chosen, and every hole
here is 1040 bytes.

The last line is the consolation and it is a real one: when the small blocks
go, everything joins up again and the heap falls back to a single page. The
memory was never lost, only unusable while something small was standing in
the middle of it.

### Size classes: what the modern ones do instead of moving

jemalloc, tcmalloc, mimalloc and the glibc allocator do not defragment. They
arrange for the fragmentation not to happen, by never letting two different
sizes share a page. Small objects come from a **run**: one page carved into
slots of a single size, with a free list threaded through the empty ones. A
forty-eight byte object and a kilobyte object are then not neighbours and
cannot wedge each other, because they are not in the same page.

```c
struct mrun {
    struct mrun *next;          /* the next run of this class with room */
    void        *slots;         /* free slots, threaded through themselves */
    unsigned short cls, used;
};
```

Everything at or under 512 bytes is rounded up to one of sixteen sizes — 16,
32, 48 … 384, 448, 512 — so nothing is rounded up by more than about an eighth
of itself. Anything larger goes to the free list as before, which is right:
the large allocations this system makes are a file, an ELF image, a TCP
buffer, and they are few, long-lived, and freed in something close to the
order they were taken.

Two things follow that are not obvious until you build it.

**A slot needs no header.** Where an object came from is a property of its
page, not of the object, so `free` finds the run by masking off the low twelve
bits of the pointer and asking a page map — one byte per page of heap, saying
which class lives there, which is the same structure tcmalloc keeps under the
same name. Sixty-four byte objects that cost eighty bytes each now cost
sixty-four.

**A run page is an ordinary block of the same pool**, not a separate arena. An
empty run is given straight back to the free list, where it coalesces with its
neighbours and is trimmed to the kernel like anything else. The alternative —
a private pool of run pages — is simpler and gives up the thing that makes the
last line of `frag` read the way it does.

The same program, on the same three patterns:

|                                   | one free list | size classes |
|-----------------------------------|---------------|--------------|
| 2000 × 64 bytes, in use           | 160K taken    | **132K** taken |
| every second one freed            | 81K free in **1001** pieces | 1K free in **1** piece, 32 runs holding 63K spare |
| 200 × (1K + 48B), the 1K ones freed | 203K free in **201** pieces, largest **1040** | 205K free in **4** pieces, largest **90112** |
| …then asking for 64 KiB           | heap grew 216K → **284K** | heap stayed at **220K** |

The row that matters is the last one. Two hundred kilobytes were free in both
cases; only in the second was any of it usable. And the row above it is the
mechanism: two hundred and one pieces became four, because the forty-eight
byte objects that used to be standing between the holes are all in three pages
of their own.

Two mistakes, both worth keeping:

*The page map was allocated through a size class.* A class needs a run, a run
needs a page, a page needs a map entry, and the map needed a run — the
allocator recursed until the filesystem server's stack ran eight bytes off the
bottom of its last page. The fault was a store to `0x2fffbff8` in a task that
had allocated nothing, which is as far from the mistake as a report can get.
The map is taken from the large path now, and its smallest size is chosen to
be past the last class so that it stays there.

*The run page was given a block header in front of it.* That is the ordinary
way to do aligned allocation, and here it meant a page could never be carved
from the front of a freshly grown region — the first page always went to
holding the header. Every run cost two pages, and the first line of the table
read 260K instead of 132K. Run pages carry no header at all: the run knows its
own address and its own size, which is everything `free` needs.

### The one thing an MMU could do here, and why it is not done

Compaction is impossible, but *this* machine has an option a flat address
space does not. The scarce resource is physical frames, not addresses, and the
two are not the same thing here — so the whole pages inside a large free hole
could have their frames handed back to the kernel while the addresses stay
reserved, and be filled again before the block is next used. Unix spells that
`madvise(MADV_DONTNEED)`, and it is perhaps sixty lines: a syscall that unmaps
a range, a flag bit in the block header (blocks are 16-aligned, so the low
bits are free), and a rule that a dropped block is filled again before it is
merged or handed out.

It is not here because the measurement says it would recover nothing. The
holes this system produces are of two kinds and neither qualifies. The small
ones do not contain a whole page. The large ones are at the end of the heap,
where the tail trim already returns them: opening a 60000-byte file twice and
closing it twice leaves the free-page count unchanged, so the filesystem
server's high-water mark is not permanent and there is nothing to reclaim. A
feature that recovers nothing on any workload the system has is a
demonstration, not a feature.


## Why `ls /` did not show /proc

Because nothing was wrong, and that is the interesting part.

`ls /` asks whichever server answers for `/` — the filesystem — and gets back
what is in the FAT16 root directory. `/proc` is not there. It is not anywhere
on the volume, and the filesystem server is right not to mention it: it has
never heard of it. The name exists in the **mount table**, which belongs to
the task doing the asking, and a listing had no way of saying so.

That is a real gap and not a cosmetic one. A namespace whose names cannot be
enumerated is a namespace you have to already know about, which is the
opposite of what "everything is a file" is for.

Three things were wrong underneath it, and they are worth taking in order.

**`/proc` was not a name.** The servers are mounted at `/dev/`, `/proc/`,
`/net/` — with the slash — and `prefix_matches` required the path to *start*
with the prefix, so `/proc/` resolved and `/proc` did not. `cat /proc/` worked
and `cat /proc` said "cannot open". A prefix ending in a slash now also
matches the name without it, because that is what a person types and what
joining a directory to a name produces.

**A server did not list its own directory.** `/proc` had a listing already —
"a directory that will not say what is in it is no use in a union", as the
comment there says. `/net` had none, and `/dev` had something worse: opening
it opened the console, so `ls /dev` blocked the shell until somebody pressed a
key and then printed the keystroke. The console server now answers a read of
`/dev` with `- 0 console` and keeps the keyboard for the console itself. The
network server answers `/net` with `ctl`, `status` and `tcp`, and `/net/tcp`
with one entry per live connection — which makes `ls /net/tcp` the shortest
way to ask what this machine is talking to.

**The mount points had to be merged in by the client.** This is the one with a
design in it. The kernel holds the mount table but not the files; the servers
hold the files but not the table. Plan 9 joins them in its kernel, where its
mount table lives. Here the table is in the kernel and the *walking* of it has
always been in the library — the union search in `vfs_open` is a client-side
loop over `sys_resolve(path, …, nth)` for exactly that reason. So the merge
belongs in the same place:

```c
/* the names in a directory that no server put there */
static inline int vfs_mounts_in(const char *dir, char *out, int cap);
```

It asks the kernel for the caller's own mount table (`sys_mounts(-1, …)` — a
task can now ask about itself without knowing its number), keeps the prefixes
whose parent is the directory being listed, and returns them in the ordinary
`d 0 NAME` shape so that a caller can feed them to the parser it already has.
`ls` and `mc` each gained six lines and a check against printing a name twice,
which `/dev/console` in the sandbox namespace would otherwise do.

```
rvos$ ls
HELLO.TXT  (54 bytes)      rvos$ ls /net        rvos$ ls /net/tcp
README.TXT  (105 bytes)    ctl  (0 bytes)       1  (0 bytes)
DOCS/                      status  (0 bytes)
BIN/                       tcp/
dev/
proc/
net/
```

The panels show them too, and browsing into `/net` from `mc` works the way
browsing into `/DOCS` does, which is the whole claim of the thing being made
good on: the network is a directory, and now it is one you can find without
being told.

`ping /` is unchanged at 111–114 µs a message — resolution grew one string
comparison and it does not show.


### One directory per task

`/proc` had three files and no directories. Two of them — `mounts` and
`pagetable` — answer about *whoever is asking*, which works because a message
carries its sender: the server is told who wants to know without the path
having to say. That is why there is no `/proc/self` here and why Linux needs
one.

What was missing is the other question, the one a task cannot ask about
itself. `/proc/<id>/` is that:

```
rvos$ ls /proc              rvos$ ls /proc/5       rvos$ cat /proc/5/mounts
tasks  (0 bytes)            mounts  (0 bytes)      / -> task 0
mounts  (0 bytes)           pagetable  (0 bytes)   /dev/ -> task 1
pagetable  (0 bytes)                               /proc/ -> task 2
0/                                                 /net/ -> task 11
1/                                                 /dev/console -> task 3
...                                                -- namespaces 3 of 8 in use
```

Task 5 is the sandbox, and the line that is only in *its* table is the whole
point of being able to look: `/dev/console -> task 3` is the null server, which
is what makes it a sandbox. From inside the shell you can now read the reason.
The same holds for the address space — `/proc/0/pagetable` and
`/proc/4/pagetable` give the identical virtual address `0x2ffff000` and
different physical pages, which is the isolation claim made visible from a
command line rather than from a boot-time demonstration.

Three details worth having:

**A directory appears when the task does.** The listing is rendered from the
task table at the moment it is asked for, not remembered, so a program that
has exited has no directory and one that starts between two `ls` commands has
one.

**A stale id is refused rather than answered.** `sys_alive` is checked before
anything is rendered, and because an id carries a generation, a number that
named a task which has since exited does not quietly answer with the state of
whoever moved into the slot. `cat /proc/999/mounts` says it cannot open it.

**The page-table dump grew a line.** It now walks `UHEAP_BASE` as well as the
stack and the UART, so `cat /proc/4/pagetable` shows the heap page the kernel
maps at task creation — the one that makes an allocator with no variables of
its own possible.


### The standard port

`telnet 100.95.222.7` with no number after it now reaches the guest, which is
worth a paragraph because of *where* the difficulty was.

It was never in the guest. The shell has listened on port 23 since the day it
existed — inside, the numbers are all the standard ones: 7 for echo, 23 for
telnet, 564 for 9P. The renumbering is entirely on the host, and its cause is
that a port below 1024 needs a privilege, and giving that privilege to a whole
emulator so that one of its guest's ports can have a nicer number is a poor
trade.

Three ways out, and the one taken is the cheapest:

```
tailscale serve --bg --tcp 23 tcp://100.95.222.7:5556
```

`tailscaled` already runs as root and already owns the overlay interface, so
it binds 23 *there* and forwards to the unprivileged port QEMU has. Nothing on
the host's port policy changes, nothing else on the machine gains a right it
did not have, and `tailscale serve --tcp 23 off` undoes it. The alternatives —
`setcap cap_net_bind_service` on the QEMU binary, or lowering
`net.ipv4.ip_unprivileged_port_start` — both grant something permanent and
machine-wide to solve something temporary and local.

The Makefile takes the host-side numbers as variables now, so if you do have
the privilege, `make run TELNETPORT=23` is the whole of it.

None of this changes what is on the other end. **The shell still has no
authentication of any kind**, and a standard port number is a better-known
door rather than a stronger one. It is reachable on a private overlay because
`HOSTIP` says so, and that is the only thing keeping it private.


## The conclusion that was wrong

Two things a person notices in the first minute of using the panels, and both
had the same shape: something was missing because nobody had thought about
where it should come from.

### `..` belongs to the path, not to the directory

FAT16 keeps `..` as a real entry on the disk, so it showed up in those panels
and in no others. `/proc`, `/net` and `/proc/<id>` are rendered by servers
with no reason to invent one, so a panel showing them had no way up except the
left arrow — and nothing on the screen said so. (`.` is filtered on purpose:
a directory's entry for itself is noise in a panel.)

The fix is one line, and it is the same idea as the mount points: **every path
except the root has a parent, and that is arithmetic on the name rather than a
fact about the directory.** The entry is put into the panel before the
directory is read, and the duplicate that a real FAT16 directory offers is
dropped by the same check that stops a mount point being named twice.

### A program on the disk can start another after all

Pressing Enter on `/BIN/FREE.ELF` opened it in the viewer, because of a
sentence that had been in this file since the panels existed: a program loaded
from the disk cannot start another, since `spawn` lives in the shared user
text of the kernel image and a disk program is not linked against it.

Every word of that is true and the conclusion does not follow. What is out of
reach is the *function*. The three calls underneath it were never restricted
at all:

```c
SYS_NEWTASK   an empty address space          -> task id
SYS_VMLOAD    one PT_LOAD segment into pages
SYS_START     an entry point, and let it run
```

and the comment beside them in `syscall.h` has said so all along — "they are
deliberately unguarded — any task may build another. Real systems put a
capability in front of exactly these." A disk program can therefore carry its
own loader, which is the same answer `lib.h` gives to not having a libc:
`prog/spawn.h` is forty lines, reads the file through the ordinary filesystem
interface, allocates with the ordinary allocator, and makes three syscalls.

The interesting part is the screen, not the loading. The child inherits its
parent's namespace, so `/dev/console` is the same connection for both, and a
program that prints a line knows nothing about panels. So the panels get out
of the way — `curses_suspend()`, which surrenders the picture without giving
up the terminal or the telnet negotiation — the program runs, `sys_wait` waits
for it, and a keystroke brings the screen back with everything marked as
unknown so it is drawn again from nothing.

```
free 11934 of 12235 pages (47736 KiB free)

-- any key --
```

Enter on an `.ELF` runs it; F3 still reads it as bytes, which is how you look
at a program rather than run it. The one thing this does not do is give the
child a different console — a program that wants to draw its own screen would
paint over the panels' idea of theirs, and getting that right is what a job
control and a saved screen are for.


## Next steps
- the menu bar in `mc` is a picture of a menu; pulling it down needs windows
  that remember what was under them, which is `panel(3)` and a cell array per
  window
- each driver mapped only its own virtio slot, which needs a device tree
- authentication on `exportfs`, without which none of the above should be
  pointed at a network anybody else is on
- more than one request in flight per connection: the tag is already there
- `old` held as a channel rather than re-resolved as a string, so rebinding
  what a bind points at does not move everything bound onto it
- a number from real hardware, or from `qemu -icount`, to put the 86 µs in
  proportion — everything above is an emulator describing itself
- why a round trip through a remote mount takes 25 ms when the link is
  loopback: the 20 ms retry deadline is the obvious suspect
- the terminal's real size, which telnet already sends and this ignores
- a pseudo-terminal: the shell serving its children's `/dev/console`, so that
  output is ordered and line endings are added in one place. It was attempted
  before the closed receive existed and failed four different ways
- redirection, which in this system wants to be a bind rather than a `>`
- `poll`/`select`, or a second task per connection — either would let `netd`
  serve more than one caller at a time
- more than one waiter on the console, so two programs can read keystrokes
  without one of them being told 0
- delayed acknowledgements and Nagle's algorithm: this stack answers every
  segment at once and sends every write as its own segment
- selective acknowledgement, so a loss costs one segment rather than
  everything after it
- capabilities on the task-building syscalls, which are currently open to all
- freeing a retired task's pages and page table (nothing is reclaimed yet)
- run under OpenSBI in supervisor mode

## License

BSD 3-Clause. See [LICENSE](LICENSE).
