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
# Every .c in prog/ is a program. They land in /BIN on the volume, which is
# where the shell looks for a bare command name.
PROGS   := $(patsubst prog/%.c,$(BUILD)/%.elf,$(wildcard prog/*.c))
PCFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -nostdlib -fno-common -fno-builtin \
           -Wall -Wextra -Os -I$(SRCDIR) -MMD -MP

# The FAT16 image, attached as a drive. It used to be copied into guest RAM
# with -device loader and read with a memcpy; it is a disk now, and the
# filesystem server drives it.
DISK    := $(BUILD)/fat16.img
DRIVE   := -drive file=$(DISK),if=none,format=raw,id=hd0 \
           -device virtio-blk-device,drive=hd0
# sstc is requested explicitly because the S-mode timer depends on it: CLINT's
# mtimecmp is machine-mode only and the machine timer interrupt is not
# delegable, so without Sstc an S-mode kernel has no clock of its own.
# force-legacy=false matters: QEMU presents virtio-mmio as a *legacy* (version
# 1) transport by default, whose queue registers are laid out differently. The
# driver speaks virtio 1.x, so the transport has to be the modern one.
# hostfwd is what makes the guest reachable at all: QEMU's user-mode network
# is a NAT, so nothing on the host can open a connection inward unless a port
# is forwarded. Inside the guest the numbers are the standard ones already:
# 7 is echo, where /BIN/NETD.ELF listens once you start it; 23 is telnet, where
# the shell listens from boot; 564 is 9P, where /BIN/EXPORTFS.ELF hands out the
# namespace. Only the host side is renumbered, and only because a port below
# 1024 needs privilege that QEMU should not be given.
#
#   make run TELNETPORT=23            # if you have the privilege; see below
#
# Three ways to have the standard number on the host:
#
#   sudo setcap cap_net_bind_service=+ep $$(which qemu-system-riscv64)
#       one binary, one capability, nobody in the path. Persistent: it stays
#       on the file until the package is upgraded. This is what runs here.
#   tailscale serve --bg --tcp 23 tcp://$(HOSTIP):5556
#       tailscaled already runs as root and binds 23 on the overlay interface
#       only. Nothing on the host changes and one command undoes it, at the
#       price of a second process in the path.
#   sudo sysctl net.ipv4.ip_unprivileged_port_start=23
#       every program on the machine gains the right; the only one of the
#       three that does not survive a reboot.
#
# HOSTIP is the address they listen on, and the default is not decoration.
# Writing `hostfwd=tcp::5556-:23`, with the host address left empty, binds to
# every interface — which on a machine with a public address and no firewall
# puts an unauthenticated shell, and a filesystem anyone may write to, on the
# internet. It did, here, for about a day. Override it deliberately:
#
#   make run HOSTIP=100.95.222.7      # a private overlay address
#   make run HOSTIP=0.0.0.0           # everybody, and you mean it
HOSTIP      ?= 127.0.0.1
ECHOPORT    ?= 5555
TELNETPORT  ?= 5556
EXPORTPORT  ?= 5564
QFLAGS  := -machine virt -cpu rv64,sstc=true -bios none -nographic \
           -global virtio-mmio.force-legacy=false -kernel $(ELF) \
           -netdev user,id=n0,hostfwd=tcp:$(HOSTIP):$(ECHOPORT)-:7,\
hostfwd=tcp:$(HOSTIP):$(TELNETPORT)-:23,\
hostfwd=tcp:$(HOSTIP):$(EXPORTPORT)-:564 \
           -device virtio-net-device,netdev=n0 $(DRIVE)

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

run rundisk: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS)

# Same, but every frame is written to build/net.pcap for inspection on the
# host — the way to check a network driver without trusting its own output.
runpcap: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS) \
	  -object filter-dump,id=f0,netdev=n0,file=$(BUILD)/net.pcap

clean:
	rm -rf $(BUILD)
