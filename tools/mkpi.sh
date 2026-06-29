#!/usr/bin/env bash
#
# mkpi.sh — prepare a Raspberry Pi boot SD card for Aegis ARM64.
#
# Usage:
#   sudo tools/mkpi.sh <pi-version> <sd-device> <path-to-aegis.img>
#
# Examples:
#   sudo tools/mkpi.sh 4 /dev/sdX kernel/arch/arm64/build/aegis.img
#   sudo tools/mkpi.sh 5 /dev/disk4 kernel/arch/arm64/build/aegis.img
#
# What it does:
#   1. Sanity-checks the device (refuses anything that looks like a
#      system disk).
#   2. Builds a single FAT32 boot partition and formats it.
#   3. Downloads (or reuses cached) Raspberry Pi firmware files.
#   4. Writes a minimal config.txt that loads aegis.img as kernel8.img.
#   5. Copies aegis.img and syncs.
#
# STATUS: UNTESTED on real hardware as of 2026-04-12. Aegis ARM64 boots
# in QEMU virt via `make -C kernel/arch/arm64 image && make run-image`,
# but the Pi serial path hasn't yet been verified on a physical device.
# The linker layout expects the Image to load at physical 0x40200000
# (kernel_address=0x200000 in config.txt, written below). Real Pi 4/5
# firmware defaults to 0x80000 for arm_64bit=1; we override that here.

set -euo pipefail

# ── Arg parsing ──────────────────────────────────────────────────────
if [ "$#" -lt 3 ]; then
    cat >&2 <<EOF
Usage: $0 <pi-version> <sd-device> <path-to-aegis.img>

  pi-version      4 | 5
  sd-device       /dev/sdX or /dev/diskN (NOT a partition)
  path-to-aegis.img   usually kernel/arch/arm64/build/aegis.img
EOF
    exit 1
fi

PI_VERSION="$1"
SD_DEVICE="$2"
IMG_PATH="$3"

case "$PI_VERSION" in
    4|5) ;;
    *) echo "error: unsupported pi version '$PI_VERSION' (use 4 or 5)" >&2; exit 1 ;;
esac

if [ ! -b "$SD_DEVICE" ] && [ ! -c "$SD_DEVICE" ]; then
    echo "error: $SD_DEVICE is not a block/char device" >&2
    exit 1
fi

if [ ! -f "$IMG_PATH" ]; then
    echo "error: $IMG_PATH not found. Run 'make -C kernel/arch/arm64 image' first." >&2
    exit 1
fi

# Refuse to write to the root disk. Catches the common foot-gun of
# passing /dev/sda (usually the system disk) instead of /dev/sdb.
ROOT_SRC=$(df / | awk 'NR==2 {print $1}')
if [[ "$ROOT_SRC" == "$SD_DEVICE"* ]]; then
    echo "error: refusing to write to $SD_DEVICE — appears to host the root filesystem" >&2
    exit 1
fi

echo ">>> Target : $SD_DEVICE"
echo ">>> Image  : $IMG_PATH"
echo ">>> Model  : Raspberry Pi $PI_VERSION"
echo
read -rp "Wipe $SD_DEVICE and install Aegis? [y/N] " confirm
[[ "$confirm" == "y" || "$confirm" == "Y" ]] || { echo "aborted"; exit 0; }

# ── Firmware cache ───────────────────────────────────────────────────
FW_CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/aegis/pi-firmware"
mkdir -p "$FW_CACHE"

# Pi 4 and Pi 5 share the same basic firmware set. On Pi 5, bootcode.bin
# is not used (the Pi 5 has a different ROM boot flow), but leaving it
# on the SD card is harmless.
FW_BASE="https://github.com/raspberrypi/firmware/raw/master/boot"
FW_FILES=(bootcode.bin start4.elf fixup4.dat bcm2711-rpi-4-b.dtb)
if [ "$PI_VERSION" = "5" ]; then
    FW_FILES+=(bcm2712-rpi-5-b.dtb)
fi

for f in "${FW_FILES[@]}"; do
    if [ ! -f "$FW_CACHE/$f" ]; then
        echo ">>> Fetching $f ..."
        curl -fL "$FW_BASE/$f" -o "$FW_CACHE/$f.tmp"
        mv "$FW_CACHE/$f.tmp" "$FW_CACHE/$f"
    fi
done

# ── Partition + format ───────────────────────────────────────────────
echo ">>> Partitioning $SD_DEVICE ..."
sudo wipefs -a "$SD_DEVICE"
# One primary FAT32 boot partition covering the whole card.
echo -e "o\nn\np\n1\n\n\nt\nc\nw" | sudo fdisk "$SD_DEVICE" >/dev/null

# Figure out the partition node (sdX1 on Linux, diskNs1 on macOS).
if [[ "$SD_DEVICE" == *"disk"* ]]; then
    PART_NODE="${SD_DEVICE}s1"
else
    PART_NODE="${SD_DEVICE}1"
fi
sleep 1     # give the kernel a moment to notice the new partition

echo ">>> Formatting $PART_NODE as FAT32 ..."
if command -v mkfs.vfat >/dev/null; then
    sudo mkfs.vfat -F 32 -n AEGISBOOT "$PART_NODE"
else
    # macOS fallback
    sudo newfs_msdos -F 32 -v AEGISBOOT "$PART_NODE"
fi

# ── Mount + populate ─────────────────────────────────────────────────
MOUNT_DIR=$(mktemp -d)
trap 'sudo umount "$MOUNT_DIR" 2>/dev/null; rmdir "$MOUNT_DIR"' EXIT

sudo mount "$PART_NODE" "$MOUNT_DIR"

echo ">>> Copying firmware ..."
for f in "${FW_FILES[@]}"; do
    sudo cp "$FW_CACHE/$f" "$MOUNT_DIR/"
done

echo ">>> Writing config.txt ..."
DTB_FILE="bcm2711-rpi-4-b.dtb"
[ "$PI_VERSION" = "5" ] && DTB_FILE="bcm2712-rpi-5-b.dtb"
sudo tee "$MOUNT_DIR/config.txt" >/dev/null <<EOF
# Aegis ARM64 — config.txt (written by tools/mkpi.sh)
arm_64bit=1
kernel=kernel8.img
device_tree=$DTB_FILE
kernel_address=0x200000
enable_uart=1
uart_2ndstage=1
disable_commandline_tags=1
EOF

echo ">>> Copying aegis.img as kernel8.img ..."
sudo cp "$IMG_PATH" "$MOUNT_DIR/kernel8.img"

sync
sudo umount "$MOUNT_DIR"
trap - EXIT
rmdir "$MOUNT_DIR"

echo ">>> Done. Insert into the Pi and connect a UART cable on GPIO 14/15."
echo ">>> You should see the Pi firmware banner followed by Aegis boot output."
