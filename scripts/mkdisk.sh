#!/usr/bin/env bash
# Build a small FAT16 image and populate it with sample files using mtools
# (no mount / no root needed). The image is loaded into guest RAM at boot.
set -euo pipefail
IMG="${1:-build/fat16.img}"
shift || true
PROGS=("$@")
mkdir -p "$(dirname "$IMG")"

# 8 MiB raw image, FAT16 with 512-byte clusters (16k clusters -> valid FAT16).
dd if=/dev/zero of="$IMG" bs=1M count=8 status=none
mkfs.fat -F 16 -s 1 -S 512 -n RVOS "$IMG" >/dev/null

tmp="$(mktemp)"
printf 'Hello from rvos!\nThis file lives on a FAT16 RAM disk.\n' > "$tmp"
mcopy -i "$IMG" "$tmp" ::/HELLO.TXT

printf 'rvos readme\n===========\nEducational RISC-V microkernel with FAT16.\nStages: boot, timer, IPC, filesystem.\n' > "$tmp"
mcopy -i "$IMG" "$tmp" ::/README.TXT

mmd -i "$IMG" ::/DOCS
printf 'a nested note in /DOCS\n' > "$tmp"
mcopy -i "$IMG" "$tmp" ::/DOCS/NOTE.TXT
rm -f "$tmp"

# Real executables on the volume, for the loader to find at run time. The
# 8.3 name FAT16 stores is the basename upper-cased: build/netd.elf -> NETD.ELF.
for p in "${PROGS[@]:-}"; do
    [ -n "$p" ] && [ -f "$p" ] || continue
    name="$(basename "$p" | tr '[:lower:]' '[:upper:]')"
    mcopy -i "$IMG" "$p" "::/$name"
done

echo "built $IMG"
mdir -i "$IMG" ::/ | sed 's/^/  /'
