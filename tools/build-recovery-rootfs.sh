#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
OUT="${1:?usage: build-recovery-rootfs.sh <image>}"
BIN=user/bin/lorica-recover/lorica-recover.elf
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
printf 'firstboot DISK_ADMIN\nservice POWER\n' > "$tmp/policy"
truncate -s 1M "$OUT"
mke2fs -q -t ext2 -F -b 1024 -N 64 -L lorica-recovery "$OUT"
debugfs -w -R 'mkdir /bin' "$OUT" >/dev/null 2>&1
debugfs -w -R 'mkdir /etc' "$OUT" >/dev/null 2>&1
debugfs -w -R 'mkdir /etc/aegis' "$OUT" >/dev/null 2>&1
debugfs -w -R 'mkdir /etc/aegis/caps.d' "$OUT" >/dev/null 2>&1
debugfs -w -R "write $BIN /bin/vigil" "$OUT" >/dev/null 2>&1
debugfs -w -R 'set_inode_field /bin/vigil mode 0100755' "$OUT" >/dev/null 2>&1
debugfs -w -R "write $tmp/policy /etc/aegis/caps.d/vigil" "$OUT" >/dev/null 2>&1
e2fsck -fy "$OUT" >/dev/null 2>&1
echo "[recovery] $OUT ($(du -h "$OUT" | awk '{print $1}'))"
