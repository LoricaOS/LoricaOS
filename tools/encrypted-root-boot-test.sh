#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TMP="$(mktemp -d)"; QPID=""; trap 'test -z "$QPID" || kill "$QPID" 2>/dev/null || true; rm -rf "$TMP"' EXIT
cc -std=c99 -O2 -Wall -Wextra -Werror -Ibuild/bearssl-install/include -Iuser/lib/libinstall \
  tools/cryptroot-image.c user/lib/libinstall/cryptroot_crypto.c \
  -Lbuild/bearssl-install/lib -lbearssl -o "$TMP/cryptroot-image"
"$TMP/cryptroot-image" build/rootfs-server.img "$TMP/root.enc" testpass
truncate -s 256M "$TMP/disk.img"
sgdisk -og "$TMP/disk.img" >/dev/null
sgdisk -n 1:2048:+8M -t 1:ef00 "$TMP/disk.img" >/dev/null
sgdisk -n 2:0:0 -t 2:A3618F24-0C76-4B3D-0001-000000000000 "$TMP/disk.img" >/dev/null
dd if=build/esp-server.img of="$TMP/disk.img" bs=512 seek=2048 conv=notrunc status=none
root_start="$(sgdisk -i 2 "$TMP/disk.img" | awk '/First sector:/{print $3}')"
dd if="$TMP/root.enc" of="$TMP/disk.img" bs=512 seek="$root_start" conv=notrunc status=none
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars.fd"
qemu-system-x86_64 -machine q35 -m 2048M -display none -no-reboot -nodefaults \
  -drive if=pflash,unit=0,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,unit=1,format=raw,file="$TMP/vars.fd" \
  -drive if=none,id=root,format=raw,file="$TMP/disk.img" -device virtio-blk-pci,drive=root \
  -object rng-random,id=rng0,filename=/dev/urandom -device virtio-rng-pci,rng=rng0 \
  -monitor unix:"$TMP/monitor",server,nowait -serial file:"$TMP/serial.log" & QPID=$!
for _ in $(seq 1 120); do grep -q 'Unlock LoricaOS root:' "$TMP/serial.log" 2>/dev/null && break; sleep 1; done
grep -q 'Unlock LoricaOS root:' "$TMP/serial.log"
for key in w r o n g ret; do printf 'sendkey %s\n' "$key" | socat - UNIX-CONNECT:"$TMP/monitor" >/dev/null; done
for _ in $(seq 1 30); do grep -q 'Incorrect passphrase' "$TMP/serial.log" 2>/dev/null && break; sleep 1; done
grep -q 'Incorrect passphrase' "$TMP/serial.log"
for key in t e s t p a s s ret; do printf 'sendkey %s\n' "$key" | socat - UNIX-CONNECT:"$TMP/monitor" >/dev/null; done
for _ in $(seq 1 120); do grep -q 'login:' "$TMP/serial.log" 2>/dev/null && break; sleep 1; done
grep -q '\[CRYPT\] OK: encrypted root unlocked' "$TMP/serial.log"
grep -q 'login:' "$TMP/serial.log"
kill "$QPID" 2>/dev/null || true;wait "$QPID" 2>/dev/null || true;QPID=""

# The offline recovery image must unlock the same root before checking ext2.
cp build/esp-server.img "$TMP/recovery-esp.img"
sed -e 's/^timeout: 3$/timeout: 0\ndefault_entry: 3/' \
    -e 's/cmdline: recovery$/cmdline: recovery recovery_auto/' \
    build/esp-server.img.conf > "$TMP/recovery.conf"
mcopy -o -i "$TMP/recovery-esp.img" "$TMP/recovery.conf" ::limine.conf
tools/prepare-limine-efi.sh tools/limine/BOOTX64.EFI "$TMP/recovery.conf" "$TMP/BOOTX64.EFI"
tools/prepare-limine-efi.sh tools/limine/BOOTIA32.EFI "$TMP/recovery.conf" "$TMP/BOOTIA32.EFI"
mcopy -o -i "$TMP/recovery-esp.img" "$TMP/BOOTX64.EFI" ::EFI/BOOT/BOOTX64.EFI
mcopy -o -i "$TMP/recovery-esp.img" "$TMP/BOOTIA32.EFI" ::EFI/BOOT/BOOTIA32.EFI
dd if="$TMP/recovery-esp.img" of="$TMP/disk.img" bs=512 seek=2048 conv=notrunc status=none
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$TMP/vars2.fd"
qemu-system-x86_64 -machine q35 -m 2048M -display none -no-reboot -nodefaults \
  -drive if=pflash,unit=0,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,unit=1,format=raw,file="$TMP/vars2.fd" \
  -drive if=none,id=root,format=raw,file="$TMP/disk.img" -device virtio-blk-pci,drive=root \
  -object rng-random,id=rng1,filename=/dev/urandom -device virtio-rng-pci,rng=rng1 \
  -monitor unix:"$TMP/monitor2",server,nowait -serial file:"$TMP/serial2.log" & QPID=$!
for _ in $(seq 1 120); do grep -q 'Root passphrase:' "$TMP/serial2.log" 2>/dev/null && break; sleep 1; done
grep -q 'Root passphrase:' "$TMP/serial2.log"
for key in t e s t p a s s ret; do printf 'sendkey %s\n' "$key" | socat - UNIX-CONNECT:"$TMP/monitor2" >/dev/null; done
for _ in $(seq 1 120); do grep -Eq ' is clean |\[RECOVERY\] PASS' "$TMP/serial2.log" 2>/dev/null && break; sleep 1; done
grep -q 'Encrypted root unlocked.' "$TMP/serial2.log"
grep -Eq ' is clean |\[RECOVERY\] PASS' "$TMP/serial2.log"
echo '[encrypted-root] PASS: boot/unlock/login and encrypted offline recovery'
