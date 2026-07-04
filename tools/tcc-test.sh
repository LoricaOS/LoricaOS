#!/usr/bin/env bash
# tcc-test.sh — prove the ported tcc runs ON AEGIS and emits working Aegis
# binaries. Builds tcc + the tcc ISO, boots it headless, and greps the serial
# log for the marker printed by the tcc-COMPILED-ON-AEGIS program.
#
# The whole chain is on-device: vigil -> stsh runs a script -> tcc compiles a C
# program (loop+branch codegen) + links a static ELF -> stsh runs it -> it
# computes sum(1..10)==55 and prints TCC_ON_AEGIS_OK. Run from the repo root.
set -u
REPO="$(git rev-parse --show-toplevel)"; cd "$REPO"

[ -x build/tcc-install/usr/local/bin/tcc ] || bash tools/build-tcc.sh
[ -f build/rootfs-server.img ] || make server-iso
bash tools/tcc-iso.sh || exit 1

LOG="$(mktemp)"
timeout 120 qemu-system-x86_64 -machine pc -cdrom build/loricaos-tcc.iso \
    -boot order=d -display none -vga std -nodefaults -serial stdio \
    -no-reboot -m 2048M > "$LOG" 2>&1 || true

if grep -qa "TCC_ON_AEGIS_OK" "$LOG"; then
    echo "[tcc-test] PASS: tcc compiled + ran a C program ON AEGIS"
    grep -a "TCC_ON_AEGIS_OK" "$LOG" | sed 's/^/  /'
    rm -f "$LOG"; exit 0
fi
echo "[tcc-test] FAIL: no on-Aegis compile marker"
echo "----- last 25 serial lines -----"; tail -25 "$LOG"
rm -f "$LOG"; exit 1
