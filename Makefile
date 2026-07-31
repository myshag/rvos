# rvos — RISC-V educational microkernel. Build + run on QEMU 'virt'.
CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
QEMU    := qemu-system-riscv64

SRCDIR  := src
BUILD   := build

CFLAGS  := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -nostdlib -fno-common -fno-builtin \
           -Wall -Wextra -O2 -g -I$(SRCDIR) -MMD -MP
LDFLAGS := -nostdlib -T $(SRCDIR)/kernel.ld -Wl,--build-id=none

CSRC    := $(wildcard $(SRCDIR)/*.c)
ASRC    := $(wildcard $(SRCDIR)/*.S)
OBJ     := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CSRC)) \
           $(patsubst $(SRCDIR)/%.S,$(BUILD)/%.o,$(ASRC))

ELF     := $(BUILD)/kernel.elf

# Standalone programs: their own ELFs, linked at a fixed user address, put on
# the FAT16 volume and loaded at run time. Not part of kernel.elf.
PROG    := $(BUILD)/hello.elf
NETD    := $(BUILD)/netd.elf
GET     := $(BUILD)/get.elf
PROGS   := $(PROG) $(NETD) $(GET)
# -MMD -MP for the same reason the kernel has it: these programs are built
# from headers in src/, and without dependency tracking a change to vfs.h
# leaves a stale .elf on the disk image that disagrees with the kernel it is
# loaded by. That is a bug that hides until one field happens to be non-zero.
PCFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -nostdlib -fno-common -fno-builtin \
           -Wall -Wextra -Os -I$(SRCDIR) -MMD -MP

# FAT16 RAM-disk image, loaded into guest memory (see fs stage).
DISK    := $(BUILD)/fat16.img
DISK_ADDR := 0x84000000
# sstc is requested explicitly because the S-mode timer depends on it: CLINT's
# mtimecmp is machine-mode only and the machine timer interrupt is not
# delegable, so without Sstc an S-mode kernel has no clock of its own.
# force-legacy=false matters: QEMU presents virtio-mmio as a *legacy* (version
# 1) transport by default, whose queue registers are laid out differently. The
# driver speaks virtio 1.x, so the transport has to be the modern one.
# hostfwd is what makes the guest reachable at all: QEMU's user-mode network
# is a NAT, so nothing on the host can open a connection inward unless a port
# is forwarded. localhost:5555 becomes 10.0.2.15:7 — the port /NETD.ELF takes
# once you start it — and localhost:5556 becomes port 23, where the shell is
# listening from boot. `nc localhost 5556` is a login.
QFLAGS  := -machine virt -cpu rv64,sstc=true -bios none -nographic \
           -global virtio-mmio.force-legacy=false -kernel $(ELF) \
           -netdev user,id=n0,hostfwd=tcp::5555-:7,hostfwd=tcp::5556-:23 \
           -device virtio-net-device,netdev=n0

.PHONY: all run rundisk runpcap disk prog clean
all: $(ELF)

# -MMD -MP writes a .d file per object listing the headers it used, so editing
# a header rebuilds everything that included it. Without this a stale object
# can disagree with the rest of the tree about a struct layout — which is
# exactly the kind of bug that hides until one field happens to be non-zero.
-include $(OBJ:.o=.d) $(PROGS:.elf=.d)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRCDIR)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJ) $(SRCDIR)/kernel.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@
	$(CROSS)size $@

$(BUILD):
	mkdir -p $(BUILD)

# Every program is linked the same way: one C file, the shared program link
# script, no libc.
$(BUILD)/%.elf: prog/%.c prog/hello.ld | $(BUILD)
	$(CC) $(PCFLAGS) -nostdlib -T prog/hello.ld -Wl,--build-id=none -o $@ $<
	$(CROSS)strip $@
	$(CROSS)size $@

prog: $(PROGS)

disk: $(DISK)
$(DISK): scripts/mkdisk.sh $(PROGS) | $(BUILD)
	scripts/mkdisk.sh $(DISK) $(PROGS)

run: $(ELF)
	$(QEMU) $(QFLAGS)

# Run with the FAT16 image mapped into RAM at $(DISK_ADDR).
rundisk: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS) -device loader,file=$(DISK),addr=$(DISK_ADDR),force-raw=on

# Same, but every frame is written to build/net.pcap for inspection on the
# host — the way to check a network driver without trusting its own output.
runpcap: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS) -device loader,file=$(DISK),addr=$(DISK_ADDR),force-raw=on \
	  -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

clean:
	rm -rf $(BUILD)
