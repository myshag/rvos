# rvos — educational RISC-V microkernel

A small teaching OS for `qemu-system-riscv64` (`virt` board), written in C and
a little assembly. It boots in machine mode, hands the machine to supervisor
mode with Sv39 paging enabled, preempts tasks on a timer, and passes messages
between them. On top of that raw message passing sits a single
Plan-9-style interface — `open/read/write/ioctl/close` — through which every
module is reached: a FAT16 filesystem, the console, and a view of kernel state.

Everything runs **headless** — serial console only, no graphics.

## The idea

The kernel provides exactly three primitives: `send`, `recv`, `yield`.

Everything else is a *task* answering the same request struct, and paths are
routed to tasks by a mount table. So a new module — a pipe, a socket, a
network stack — is added without touching the kernel at all: write a task,
bind it into the tree. The kernel never learns it exists.

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
| `src/vm.c`         | **Sv39**: the three-level walk, mapping, superpages |
| `src/entry.S`      | supervisor-mode trap save/restore (full integer context) |
| `src/kernel.ld`    | link at `0x80000000`, boot stack |
| `src/trap.c`       | timer setup, trap/`ecall` dispatch |
| `src/task.c`       | task table, preemptive round-robin scheduler, syscalls |
| `src/ipc.c`        | synchronous rendezvous `send`/`recv` |
| `src/vfs.h`        | **the one interface** + client wrappers |
| `src/vfs.c`        | the namespace: mount table, `bind`, longest-prefix routing |
| `src/srv_fs.c`     | filesystem module (owns FAT16 entirely) |
| `src/srv_console.c`| console module (owns the UART) |
| `src/srv_proc.c`   | kernel state as files (`/proc/tasks`, `/mounts`, `/pagetable`) |
| `src/srv_null.c`   | the bit bucket, for binding over other paths |
| `src/fat16.c`      | read-only FAT16 over a RAM-backed block device |
| `src/uart.c`       | NS16550 driver + tiny `kprintf` |
| `src/kmain.c`      | boot, initial namespace, the demo shell |

## Design notes

- **Supervisor mode with Sv39 paging.** The kernel identity-maps itself, so
  tasks still share one address space and cooperate by message passing; this
  is *microkernel-style* in structure, and paging currently buys protection
  (page permissions are enforced) rather than isolation. Giving each task its
  own page table is the next step, and it forces the IPC redesign noted below.
- **Scheduling** is preemptive round-robin driven by the Sstc timer; `ecall`
  provides `yield`, `send`, `recv`. Since servers spend their lives blocked in
  `recv`, most switching actually happens at IPC boundaries — the timer is
  there so a CPU-bound task can't wedge the system.
- **IPC** carries one machine word; larger payloads pass a pointer to a shared
  request struct (safe only because there is no address-space isolation yet).
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
make disk       # build the FAT16 image (build/fat16.img)
make rundisk    # run headless with the disk mapped into RAM
```

Exit QEMU with `Ctrl-A` then `X`.

## What you'll see

Two tasks run the *same* call against the *same* path and reach different
modules, because one of them rebound that path in its own namespace:

```
--- shell (task 4) ------------------------------------
$ cat /proc/mounts          -- shell's view
/ -> task 0
/dev/ -> task 1
/proc/ -> task 2

$ write(/dev/console, "...")
  visible: routed to the console module

--- sandbox (task 5) ----------------------------------
$ vfs_ns_clone()            -- take a private namespace
$ bind /dev/console -> null (task 3)

$ cat /proc/mounts          -- sandbox's own view
/ -> task 0
/dev/ -> task 1
/proc/ -> task 2
/dev/console -> task 3

$ write(/dev/console, "...")  -- identical call to the shell's
  (nothing printed: the path now reaches null)

--- back in the shell ----------------------------------
  still visible: the shell's namespace was never touched
```

Note that `/proc/mounts` is one path returning different text to different
readers — the namespace it reports is the caller's, not a global one.
`/proc/tasks` is likewise a live snapshot of the scheduler:

```
0  blocked  fs
1  blocked  console
2  running  proc  (me)
3  blocked  null
4  blocked  shell
5  runnable sandbox
```

The proc server is running because it is serving the request; the shell is
blocked because it is waiting for the reply — rendezvous IPC, read as a file.

`/proc/pagetable` walks live translations. The UART takes all three levels;
a kernel address stops at level 1 because a 2 MiB superpage covers it:

```
va 0x10000000
  L2 idx 0   pte 0x20bff401  -> table 0x82ffd000
  L1 idx 128 pte 0x20bff001  -> table 0x82ffc000
  L0 idx 0   pte 0x40000c7   leaf rw-- pa 0x10000000 (4KiB)

va 0x80001000
  L2 idx 2   pte 0x20bff801  -> table 0x82ffe000
  L1 idx 0   pte 0x200000cf  leaf rwx- pa 0x80001000 (2MiB superpage)
```

And the permissions are real, not decorative — a task that writes to a page
mapped `r--` is stopped by the MMU and retired:

```
$ write *(char*)0x40000000  -- same page, no W bit
[trap] store page fault in task 'faulter'
       scause=15  stval=0x40000000  sepc=0x80000718
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

## Next steps

- a page table per task, for real isolation — which also means IPC can no
  longer hand a raw pointer across tasks and needs the kernel to copy payloads
- user mode (U-bit mappings), so tasks cannot touch kernel pages at all
- a `virtio-blk` driver, replacing the RAM image with a real disk
- FAT16 writes; subdirectory traversal
- run under OpenSBI in supervisor mode
