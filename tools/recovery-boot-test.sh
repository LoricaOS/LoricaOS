#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TMP="$(mktemp -d)"; qemu_pid=""
cleanup() { [[ -z "$qemu_pid" ]] || kill "$qemu_pid" 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT
OVMF="${OVMF:-/usr/share/ovmf/OVMF.fd}"
[[ -f "$OVMF" ]] || { echo "recovery-boot: OVMF not found" >&2; exit 1; }
cc -std=c99 -O2 -Wall -Wextra -Werror tools/ext2-repair-file.c \
    user/lib/libinstall/ext2_repair.c -o "$TMP/ext2-repair"

cp build/esp-server.img "$TMP/esp.img"
cp build/rootfs-server.img "$TMP/root.ext2"
truncate -s 44M "$TMP/root.ext2"
debugfs -w -R 'set_super_value state 0' "$TMP/root.ext2" >/dev/null 2>&1
mkdir -p "$TMP/boot"
mcopy -i "$TMP/esp.img" ::boot/aegis.old "$TMP/boot/aegis.old"
mcopy -i "$TMP/esp.img" ::boot/recovery.img "$TMP/boot/recovery.img"
kernel_hash="$(tools/blake2b.sh "$TMP/boot/aegis.old")"
recovery_hash="$(tools/blake2b.sh "$TMP/boot/recovery.img")"
cat > "$TMP/limine.conf" <<EOF
timeout: 0
editor_enabled: no
hash_mismatch_panic: yes

/LoricaOS Recovery Test
    protocol: limine
    path: boot():/boot/aegis.old#$kernel_hash
    module_path: boot():/boot/recovery.img#$recovery_hash
    cmdline: recovery_auto
EOF
mcopy -o -i "$TMP/esp.img" "$TMP/limine.conf" ::limine.conf
tools/prepare-limine-efi.sh tools/limine/BOOTX64.EFI "$TMP/limine.conf" "$TMP/BOOTX64.EFI"
tools/prepare-limine-efi.sh tools/limine/BOOTIA32.EFI "$TMP/limine.conf" "$TMP/BOOTIA32.EFI"
mcopy -o -i "$TMP/esp.img" "$TMP/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -o -i "$TMP/esp.img" "$TMP/BOOTIA32.EFI" ::EFI/BOOT/BOOTIA32.EFI

truncate -s 128M "$TMP/disk.img"
sgdisk --clear \
    --new=1:2048:18431 --typecode=1:ef00 \
    --new=2:18432:0 --typecode=2:A3618F24-0C76-4B3D-0001-000000000000 \
    "$TMP/disk.img" >/dev/null
dd if="$TMP/esp.img" of="$TMP/disk.img" bs=512 seek=2048 conv=notrunc status=none
dd if="$TMP/root.ext2" of="$TMP/disk.img" bs=512 seek=18432 conv=notrunc status=none

qemu-system-x86_64 -machine q35 -bios "$OVMF" -m 2048M \
    -drive file="$TMP/disk.img",format=raw,if=none,id=root \
    -device nvme,drive=root,serial=lorica-recovery \
    -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,rng=rng0 \
    -display none -serial stdio -no-reboot >"$TMP/serial.log" 2>&1 & qemu_pid=$!
for _ in $(seq 1 60); do
    grep -q '\[RECOVERY\] PASS' "$TMP/serial.log" && break
    kill -0 "$qemu_pid" 2>/dev/null || break
    sleep 1
done
kill "$qemu_pid" 2>/dev/null || true; wait "$qemu_pid" 2>/dev/null || true; qemu_pid=""
grep -q '\[RECOVERY\] PASS' "$TMP/serial.log" || {
    echo 'recovery-boot: recovery environment did not complete' >&2
    tail -40 "$TMP/serial.log" >&2
    exit 1
}
dd if="$TMP/disk.img" of="$TMP/repaired.ext2" bs=512 skip=18432 count=90112 status=none
"$TMP/ext2-repair" "$TMP/repaired.ext2" | grep -q CLEAN
echo 'recovery-boot: PASS (retained kernel, offline root, raw-disk repair)'
