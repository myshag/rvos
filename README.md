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

**TCP is still not complete, and it is worth being exact about what is
missing.** What works is a client that opens a connection, sends, receives,
closes in order, and *retransmits what goes unacknowledged*:

```
tcp: SYN -> 10.0.2.2:9998
tcp: SYN-ACK received, connection established
tcp: sent 16 bytes
tcp: received 16 bytes: "hello-from-host"
tcp: closing
tcp: closed
```

What is still absent: out-of-order reassembly, window management beyond a
fixed advertised window, congestion control, and a fixed initial sequence
number instead of a random one.

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

## Next steps

- a blocking `read()` on the console, so the shell stops polling the driver
  (the driver is interrupt-driven now; its client is not)
- a network *server*: bind the stack at `/net/` so programs reach it through
  the same open/read/write interface as everything else
- answering ARP and ICMP rather than only initiating them
- capabilities on the task-building syscalls, which are currently open to all
- freeing a retired task's pages and page table (nothing is reclaimed yet)
- a `virtio-blk` driver, replacing the RAM image with a real disk
- `argv`/`envp`, and a shell that execs what you type
- a `virtio-blk` driver, replacing the RAM image with a real disk
- FAT16 writes; subdirectory traversal
- run under OpenSBI in supervisor mode

## License

BSD 3-Clause. See [LICENSE](LICENSE).
