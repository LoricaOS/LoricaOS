#!/usr/bin/env bash
# build-pi5-sdimg.sh — assemble a single flashable Raspberry Pi 5 SD-card image.
#
# Produces one .img with an MBR partition table and a single FAT32 boot
# partition holding everything the Pi 5 needs:
#   bcm2712-rpi-5-b.dtb        Pi 5 device tree (firmware auto-selects it)
#   overlays/bcm2712d0.dtbo    optional D0-stepping overlay (rev 1.1 boards)
#   config.txt                 kernel + `initramfs rootfs.ext2`
#   kernel_2712.img            the native pi5-kernel.img (non-Limine boot stub)
#   rootfs.ext2                loaded as the firmware initramfs; Aegis reads
#                              /chosen linux,initrd-* (fdt_initrd) → ramdisk0 →
#                              ext2-mounted as root.
#
# Pi 5's second-stage bootloader lives in the on-board SPI EEPROM, so the card
# only carries DTB + kernel + initramfs + config. No loop devices / root needed
# (mkfs.vfat a file, mcopy into it, dd it into a partitioned disk image).
#
# Usage: build-pi5-sdimg.sh <pi5-kernel.img> <rootfs.ext2> <out.img>
#
# STATUS: Pi 5 hardware boot is UNVERIFIED — build + flash + boot on a real
# board before relying on this.
set -euo pipefail

KERNEL="${1:?usage: build-pi5-sdimg.sh <pi5-kernel.img> <rootfs.ext2> <out.img>}"
ROOTFS="${2:?usage: build-pi5-sdimg.sh <pi5-kernel.img> <rootfs.ext2> <out.img>}"
OUT="${3:?usage: build-pi5-sdimg.sh <pi5-kernel.img> <rootfs.ext2> <out.img>}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FW_CACHE="$REPO/references/pi-firmware"
FW_BASE="https://github.com/raspberrypi/firmware/raw/master/boot"

for f in "$KERNEL" "$ROOTFS"; do
    [ -f "$f" ] || { echo "build-pi5-sdimg: missing input $f" >&2; exit 1; }
done

# ── firmware (DTB + optional overlay), cached ────────────────────────────────
mkdir -p "$FW_CACHE/overlays"
fetch() { # <rel-path>
    local rel="$1" dest="$FW_CACHE/$1"
    [ -s "$dest" ] && return 0
    echo "[pi5-sdimg] fetching $rel"
    curl -fsSL -o "$dest.tmp" "$FW_BASE/$rel" && mv "$dest.tmp" "$dest"
}
fetch bcm2712-rpi-5-b.dtb
fetch overlays/bcm2712d0.dtbo || echo "[pi5-sdimg] note: optional overlay skipped"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/stage"; mkdir -p "$STAGE/overlays"

cp "$FW_CACHE/bcm2712-rpi-5-b.dtb" "$STAGE/"
[ -s "$FW_CACHE/overlays/bcm2712d0.dtbo" ] && cp "$FW_CACHE/overlays/bcm2712d0.dtbo" "$STAGE/overlays/"
cp "$KERNEL" "$STAGE/kernel_2712.img"
cp "$ROOTFS" "$STAGE/rootfs.ext2"

# config.txt: native kernel at 0x80000 + the rootfs.ext2 as the firmware
# initramfs. Matches Makefile.pi5native's staged config, plus the initramfs line.
cat > "$STAGE/config.txt" <<'CFG'
kernel=kernel_2712.img
kernel_address=0x80000
initramfs rootfs.ext2 followkernel
enable_uart=1
os_check=0
pciex4_reset=0
usb_max_current_enable=1
dtparam=pciex1
CFG

# ── size the FAT32 boot partition: content + 32 MiB slack, ≥ 64 MiB ──────────
# --apparent-size: rootfs.ext2 is a sparse mke2fs image; mtools writes the FULL
# byte size into the FAT, so size against apparent bytes, not allocated blocks.
CONTENT_KB=$(du -sk --apparent-size "$STAGE" | awk '{print $1}')
FAT_MB=$(( (CONTENT_KB / 1024) + 32 ))
[ "$FAT_MB" -lt 64 ] && FAT_MB=64

# ── FAT32 filesystem image (no root) ─────────────────────────────────────────
FAT="$WORK/boot.fat"
dd if=/dev/zero of="$FAT" bs=1M count="$FAT_MB" status=none
mkfs.vfat -F 32 -n LORICA "$FAT" >/dev/null
mmd   -i "$FAT" ::/overlays 2>/dev/null || true
for f in "$STAGE"/*; do
    [ -d "$f" ] && continue
    mcopy -i "$FAT" "$f" "::/$(basename "$f")"
done
[ -s "$STAGE/overlays/bcm2712d0.dtbo" ] && mcopy -i "$FAT" "$STAGE/overlays/bcm2712d0.dtbo" ::/overlays/

# ── partitioned disk image: 1 MiB gap + the FAT32 partition (type 0x0c) ──────
dd if=/dev/zero of="$OUT" bs=1M count=$(( FAT_MB + 1 )) status=none
echo "2048,,c" | sfdisk "$OUT" >/dev/null      # start LBA 2048 (1 MiB), type W95 FAT32 (LBA)
dd if="$FAT" of="$OUT" bs=1M seek=1 conv=notrunc status=none

echo "[pi5-sdimg] -> $OUT ($(( $(stat -c%s "$OUT") / 1048576 )) MiB, FAT32 ${FAT_MB} MiB)"
