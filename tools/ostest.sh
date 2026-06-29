#!/usr/bin/env bash
# AspisOS boot test.
#
# Boots the live ISO headless in QEMU. Success = the kernel comes up, mounts the
# root filesystem, execs init (/bin/vigil), and the userland brings the display
# stack all the way to the login greeter ("[BASTION] greeter ready"). That single
# line exercises kernel + init-from-rootfs + services + Lumen + Bastion.
set -u
ISO="${1:?usage: ostest.sh <aegis.iso>}"
LOG="$(mktemp)"
MARKER="[BASTION] greeter ready"

timeout 150 qemu-system-x86_64 -machine pc -cdrom "$ISO" -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot -m 2048M \
    > "$LOG" 2>&1 || true

if grep -qF "$MARKER" "$LOG"; then
    echo "[ostest] PASS: AspisOS booted to the greeter"
    rm -f "$LOG"
    exit 0
fi

echo "[ostest] FAIL: did not reach the greeter ('$MARKER')"
echo "----- last 40 serial lines -----"
tail -40 "$LOG"
rm -f "$LOG"
exit 1
