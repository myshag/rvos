# rvos — educational RISC-V microkernel with FAT16

A small teaching OS for `qemu-system-riscv64` (`virt` board), written in C +
a little assembly. It boots in machine mode, preempts tasks on a timer, passes
messages between them (microkernel-style), and reads files off a FAT16 disk
through a filesystem *server* task.

Everything runs **headless** — serial console only, no graphics.

## Layout

| File | Role |
|------|------|
| `src/boot.S`     | reset entry: park secondary harts, clear bss, call `kmain` |
| `src/entry.S`    | machine-mode trap save/restore (full integer context) |
| `src/kernel.ld`  | link at `0x80000000`, boot stack |
| `src/uart.c`     | NS16550 console + tiny `kprintf` |
| `src/trap.c`     | timer setup + trap/`ecall` dispatch |
| `src/task.c`     | task table, preemptive round-robin scheduler, syscalls |
| `src/ipc.c`      | synchronous rendezvous `send`/`recv` |
| `src/fat16.c`    | read-only FAT16 over a RAM-backed block device |
| `src/kmain.c`    | wires it together: `fs` server + `shell` demo |

## Architecture

- **Machine mode only**, no MMU/paging — tasks share one address space and
  cooperate via message passing (so it's *microkernel-style*; per-task Sv39
  isolation is the natural next step).
- **Scheduler:** timer (CLINT) drives preemptive round-robin; `ecall` provides
  `yield`, `send`, `recv`.
- **IPC:** one machine word per message; larger payloads pass a pointer to a
  shared request struct.
- **Filesystem:** the FAT16 image is loaded into guest RAM at `0x84000000`
  (`-device loader`); the driver treats that region as a flat block device.

## Build & run

Needs `gcc-riscv64-unknown-elf`, `qemu-system-misc`, `mtools`, `mkfs.fat`.

```bash
make            # build kernel.elf
make disk       # build the FAT16 image (build/fat16.img)
make rundisk    # run headless with the disk mapped into RAM
```

Exit QEMU with `Ctrl-A` then `X`.

## What you'll see

```
$ ls /
  HELLO.TXT  (54 bytes)
  README.TXT (105 bytes)
  DOCS/
$ cat README.TXT
rvos readme ...
```

## Development stages (git history)

1. boot + UART console
2. traps, CLINT timer, preemptive round-robin + `ecall` syscalls
3. synchronous rendezvous IPC
4. FAT16 filesystem server + shell

## Next steps

- Sv39 paging for real per-task isolation
- a `virtio-blk` driver (real disk instead of RAM image)
- FAT16 writes; subdirectory traversal in the shell
- run under OpenSBI in supervisor mode
