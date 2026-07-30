# rvos — RISC-V educational microkernel. Build + run on QEMU 'virt'.
CROSS   := riscv64-unknown-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
QEMU    := qemu-system-riscv64

SRCDIR  := src
BUILD   := build

CFLAGS  := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany \
           -ffreestanding -nostdlib -fno-common -fno-builtin \
           -Wall -Wextra -O2 -g -I$(SRCDIR)
LDFLAGS := -nostdlib -T $(SRCDIR)/kernel.ld -Wl,--build-id=none

CSRC    := $(wildcard $(SRCDIR)/*.c)
ASRC    := $(wildcard $(SRCDIR)/*.S)
OBJ     := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(CSRC)) \
           $(patsubst $(SRCDIR)/%.S,$(BUILD)/%.o,$(ASRC))

ELF     := $(BUILD)/kernel.elf

# FAT16 RAM-disk image, loaded into guest memory (see fs stage).
DISK    := $(BUILD)/fat16.img
DISK_ADDR := 0x88000000
QFLAGS  := -machine virt -bios none -nographic -kernel $(ELF)

.PHONY: all run rundisk disk clean
all: $(ELF)

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
disk: $(DISK)
$(DISK): scripts/mkdisk.sh | $(BUILD)
	scripts/mkdisk.sh $(DISK)

run: $(ELF)
	$(QEMU) $(QFLAGS)

# Run with the FAT16 image mapped into RAM at $(DISK_ADDR).
rundisk: $(ELF) $(DISK)
	$(QEMU) $(QFLAGS) -device loader,file=$(DISK),addr=$(DISK_ADDR),force-raw=on

clean:
	rm -rf $(BUILD)
