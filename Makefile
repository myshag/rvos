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

# A standalone program: its own ELF, linked at a fixed user address, put on
# the FAT16 volume and loaded at run time. Not part of kernel.elf.
PROG    := $(BUILD)/hello.elf
PCFLAGS := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -nostdlib -fno-common -fno-builtin \
           -Wall -Wextra -Os -I$(SRCDIR)

# FAT16 RAM-disk image, loaded into guest memory (see fs stage).
DISK    := $(BUILD)/fat16.img
DISK_ADDR := 0x84000000
# sstc is requested explicitly because the S-mode timer depends on it: CLINT's
# mtimecmp is machine-mode only and the machine timer interrupt is not
# delegable, so without Sstc an S-mode kernel has no clock of its own.
QFLAGS  := -machine virt -cpu rv64,sstc=true -bios none -nographic -kernel $(ELF)

.PHONY: all run rundisk disk prog clean
all: $(ELF)

# -MMD -MP writes a .d file per object listing the headers it used, so editing
# a header rebuilds everything that included it. Without this a stale object
# can disagree with the rest of the tree about a struct layout — which is
# exactly the kind of bug that hides until one field happens to be non-zero.
-include $(OBJ:.o=.d)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SRCDIR)/%.S | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(ELF): $(OBJ) $(SRCDIR)/kernel.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@
	$(CROSS)size $@

$(BUILD):
	mkdir -p $(BUILD)

# Build a 4 MiB FAT16 image and populate it with sample files.
$(PROG): prog/hello.c prog/hello.ld | $(BUILD)
	$(CC) $(PCFLAGS) -nostdlib -T prog/hello.ld -Wl,--build-id=none -o $@ $<
	$(CROSS)strip $@
	$(CROSS)size $@

prog: $(PROG)

disk: $(DISK)
$(DISK): scripts/mkdisk.sh $(PROG) | $(BUILD)
	scripts/mkdisk.sh $(DISK) $(PROG)

run: $(ELF)
	$(QEMU) $(QFLAGS)

# Run with the FAT16 image mapped into RAM at $(DISK_ADDR).
rundisk: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS) -device loader,file=$(DISK),addr=$(DISK_ADDR),force-raw=on

clean:
	rm -rf $(BUILD)
