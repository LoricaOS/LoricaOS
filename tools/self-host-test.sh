#!/usr/bin/env bash
# self-host-test.sh — prove LoricaOS can BUILD real software on itself. Builds
# tcc + make, assembles the self-host ISO, boots it headless, and greps the
# serial log for the marker printed by a program that make+tcc compiled, linked
# (static, against musl libc), and ran ON AEGIS. Run from the repo root.
set -u
REPO="$(git rev-parse --show-toplevel)"; cd "$REPO"

[ -x build/tcc-install/usr/local/bin/tcc ] || bash tools/build-tcc.sh
[ -x build/make-install/make ]             || bash tools/build-make.sh
[ -f build/rootfs-server.img ]             || make server-iso
bash tools/self-host-iso.sh || exit 1

LOG="$(mktemp)"
timeout 150 qemu-system-x86_64 -machine pc -cdrom build/loricaos-selfhost.iso \
    -boot order=d -display none -vga std -nodefaults -serial stdio \
    -no-reboot -m 2048M > "$LOG" 2>&1 || true

if grep -qa "SELFHOST_OK" "$LOG"; then
    echo "[self-host-test] PASS: make + tcc built + ran a real libc program ON AEGIS"
    grep -a "SELFHOST_OK" "$LOG" | sed 's/^/  /'
    rm -f "$LOG"; exit 0
fi
echo "[self-host-test] FAIL: no on-Aegis build marker"
echo "----- last 25 serial lines -----"; tail -25 "$LOG"
rm -f "$LOG"; exit 1
