# rvos — educational RISC-V microkernel

A small teaching OS for `qemu-system-riscv64` (`virt` board), written in C and
a little assembly. It boots in machine mode, preempts tasks on a timer, and
passes messages between them. On top of that raw message passing sits a single
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
| `src/boot.S`       | reset entry: park secondary harts, clear bss, call `kmain` |
| `src/entry.S`      | machine-mode trap save/restore (full integer context) |
| `src/kernel.ld`    | link at `0x80000000`, boot stack |
| `src/trap.c`       | timer setup, trap/`ecall` dispatch |
| `src/task.c`       | task table, preemptive round-robin scheduler, syscalls |
| `src/ipc.c`        | synchronous rendezvous `send`/`recv` |
| `src/vfs.h`        | **the one interface** + client wrappers |
| `src/vfs.c`        | the namespace: mount table, `bind`, longest-prefix routing |
| `src/srv_fs.c`     | filesystem module (owns FAT16 entirely) |
| `src/srv_console.c`| console module (owns the UART) |
| `src/srv_proc.c`   | kernel state as files (`/proc/tasks`, `/proc/mounts`) |
| `src/srv_null.c`   | the bit bucket, for binding over other paths |
| `src/fat16.c`      | read-only FAT16 over a RAM-backed block device |
| `src/uart.c`       | NS16550 driver + tiny `kprintf` |
| `src/kmain.c`      | boot, initial namespace, the demo shell |

## Design notes

- **Machine mode only, no MMU.** Tasks share one address space and cooperate
  by message passing, so this is *microkernel-style* in structure rather than
  in isolation. Per-task Sv39 page tables are the natural next step.
- **Scheduling** is preemptive round-robin driven by the CLINT timer; `ecall`
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

## Development stages (git history)

1. boot + UART console
2. traps, CLINT timer, preemptive round-robin + `ecall` syscalls
3. synchronous rendezvous IPC
4. FAT16 filesystem server + shell
5. one interface for every module: `open/read/write/ioctl/close`
6. dynamic namespace (`bind`) + `/proc` module
7. per-task namespaces (`vfs_ns_clone`) + `/dev/null` module

## Next steps

- Sv39 paging for real isolation (and message payloads that aren't raw pointers)
- a `virtio-blk` driver, replacing the RAM image with a real disk
- FAT16 writes; subdirectory traversal
- run under OpenSBI in supervisor mode
