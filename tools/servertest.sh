#!/usr/bin/env bash
# AspisOS server-ISO boot test.
#
# Boots the server ISO headless. Success = the system comes up text-only to the
# console login prompt ("login:") — kernel + init-from-rootfs + vigil + getty +
# login, with NO graphical stack. We also assert the compositor/display manager
# never appear on serial (they aren't in the server image at all), so the build
# really is graphical-free.
set -u
ISO="${1:?usage: servertest.sh <aegis-server.iso>}"
LOG="$(mktemp)"

timeout 120 qemu-system-x86_64 -machine pc -cdrom "$ISO" -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot -m 2048M \
    > "$LOG" 2>&1 || true

reached_login=0; grep -qE "login:" "$LOG" && reached_login=1
graphical=$(grep -cE "\[LUMEN\]|\[BASTION\]|greeter ready" "$LOG" || true)

if [ "$reached_login" = 1 ] && [ "$graphical" = 0 ]; then
    echo "[servertest] PASS: server booted to console login, no graphical stack"
    rm -f "$LOG"
    exit 0
fi

echo "[servertest] FAIL: reached_login=$reached_login graphical_lines=$graphical"
echo "----- last 40 serial lines -----"
tail -40 "$LOG"
rm -f "$LOG"
exit 1
