#!/usr/bin/env bash
# LoricaOS userland security self-test.
#
# Boots the selftest ISO (kernel cmdline carries `selftest`). Vigil runs
# /bin/selftest, which execs /bin/captest — the baseline-cap probe that attempts
# a set of privileged operations, all of which the kernel must DENY to an
# ordinary process. Pass = "[CAPTEST] ALL PASS" appears (and no "[CAPTEST] FAIL").
set -u
ISO="${1:?usage: selftest.sh <loricaos-test.iso>}"
LOG="$(mktemp)"

timeout 150 qemu-system-x86_64 -machine pc -cdrom "$ISO" -boot order=d \
    -display none -vga std -nodefaults -serial stdio -no-reboot -m 2048M \
    > "$LOG" 2>&1 || true

fails=$(grep -c "\[CAPTEST\] .*FAIL" "$LOG" || true)
if grep -q "\[CAPTEST\] ALL PASS" "$LOG" && [ "$fails" = 0 ]; then
    echo "[selftest] PASS: userland capability model enforced"
    grep -E "\[SELFTEST\]|\[CAPTEST\]" "$LOG" | sed 's/^/  /'
    rm -f "$LOG"
    exit 0
fi

echo "[selftest] FAIL: captest did not ALL PASS (FAIL lines: $fails)"
grep -E "\[SELFTEST\]|\[CAPTEST\]" "$LOG" | sed 's/^/  /'
echo "----- last 30 serial lines -----"; tail -30 "$LOG"
rm -f "$LOG"
exit 1
