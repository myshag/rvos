# rvos — educational RISC-V microkernel

A small teaching OS for `qemu-system-riscv64` (`virt` board), written in C and
a little assembly. It boots in machine mode, preempts tasks on a timer, and
passes messages between them. On top of that raw message passing sits a single
Plan-9-style interface — `open/read/write/ioctl/close` — through which every
module is reached: a FAT16 filesystem, the console, and a view of kernel state.

Everything runs **headless** — serial console only, no graphics.

## The idea

The kernel provides exactly three primitives: `send`, `recv`, `yield`.

Everything else is a *task* that answers the same request struct, and paths are
routed to tasks by a mount table that can change while the system runs. So a
new module — a pipe, a socket, a network stack — is added without touching the
kernel at all. The `/proc` module in the demo is bound into the namespace after
the system is already up, and the kernel never learns it exists.

```
        shell
          |  open/read/write/ioctl/close   (vfs.h — a library, not syscalls)
          v
      namespace (vfs.c)   "/" -> fs,  "/dev/" -> console,  "/proc/" -> proc
          |
          |  send/recv                     (the only kernel interface)
          v
   +------------+   +---------------+   +--------------+
   |  fs task   |   | console task  |   |  proc task   |
   |  FAT16     |   | UART          |   | task table   |
   +------------+   +---------------+   +--------------+
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
- **Namespace** is data, not policy: `vfs_bind(prefix, task)` at runtime,
  longest-prefix wins. Plan 9 gives each process its own mutable namespace;
  here there is one global table — per-task namespaces are the next step.
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

```
$ cat /                          # a directory read()s like anything else
HELLO.TXT  (54 bytes)
README.TXT  (105 bytes)
DOCS/

$ ioctl(/README.TXT, GETSIZE) -> 105 bytes (file not read)
$ write(/dev/console) -> these bytes came back via IPC

$ cat /proc/tasks        (before binding anything there)
  -> fails: nothing serves that subtree yet
$ bind /proc/ -> task 2      (system already running)
$ cat /proc/tasks
0  blocked  fs
1  blocked  console
2  running  proc  (me)
3  blocked  shell
```

That last listing is a real snapshot of the scheduler: the proc server is
running because it is serving the request, and the shell is blocked because
it is waiting for the reply — rendezvous IPC, visible as a file.

## Development stages (git history)

1. boot + UART console
2. traps, CLINT timer, preemptive round-robin + `ecall` syscalls
3. synchronous rendezvous IPC
4. FAT16 filesystem server + shell
5. one interface for every module: `open/read/write/ioctl/close`
6. dynamic namespace (`bind`) + `/proc` module

## Next steps

- per-task namespaces, so two tasks can see different things at one path
- Sv39 paging for real isolation (and message payloads that aren't raw pointers)
- a `virtio-blk` driver, replacing the RAM image with a real disk
- FAT16 writes; subdirectory traversal
- run under OpenSBI in supervisor mode
