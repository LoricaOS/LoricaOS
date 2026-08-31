#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
printf kernel > "$TMP/kernel"
printf rootfs > "$TMP/rootfs"
kh="$(tools/blake2b.sh "$TMP/kernel")"
rh="$(tools/blake2b.sh "$TMP/rootfs")"
KERNEL_HASH="$kh" ROOTFS_HASH="$rh" ESP_HASH="$rh" \
    tools/gen-limine-conf.sh test > "$TMP/limine.conf"
grep -q "aegis.elf#$kh" "$TMP/limine.conf"
[ "$(grep -c "rootfs.img#$rh" "$TMP/limine.conf")" = 1 ]
grep -q '^editor_enabled: no$' "$TMP/limine.conf"
grep -q '^hash_mismatch_panic: yes$' "$TMP/limine.conf"
echo "boot-integrity: PASS (kernel/module hashes and fail-closed policy)"
