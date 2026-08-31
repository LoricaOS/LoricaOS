#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT INT TERM

cc -std=c99 -O2 -Wall -Wextra -Werror \
    "$ROOT/tools/ext2-repair-file.c" \
    "$ROOT/user/lib/libinstall/ext2_repair.c" -o "$TMP/ext2-repair"
truncate -s 44M "$TMP/fs.ext2"
mke2fs -q -t ext2 -F -b 4096 -i 32768 "$TMP/fs.ext2"
printf 'repair-me\n' > "$TMP/payload"
debugfs -w -R "write $TMP/payload /payload" "$TMP/fs.ext2" >/dev/null 2>&1
debugfs -w -R 'set_super_value state 0' "$TMP/fs.ext2" >/dev/null 2>&1

set +e
"$TMP/ext2-repair" "$TMP/fs.ext2" >/dev/null 2>&1
rc=$?
set -e
test "$rc" = 3
"$TMP/ext2-repair" -y "$TMP/fs.ext2"
block=$(debugfs -R 'blocks /payload' "$TMP/fs.ext2" 2>/dev/null | awk '{print $1}')
debugfs -w -R "freeb $block" "$TMP/fs.ext2" >/dev/null 2>&1
debugfs -w -R 'set_super_value state 1' "$TMP/fs.ext2" >/dev/null 2>&1
set +e
"$TMP/ext2-repair" "$TMP/fs.ext2" >/dev/null 2>&1
rc=$?
set -e
test "$rc" = 3
"$TMP/ext2-repair" -y "$TMP/fs.ext2"
e2fsck -fn "$TMP/fs.ext2" >"$TMP/e2fsck.log" 2>&1
debugfs -R 'cat /payload' "$TMP/fs.ext2" 2>/dev/null | grep -qx repair-me
"$TMP/ext2-repair" "$TMP/fs.ext2" | grep -q CLEAN
echo 'ext2-repair: PASS (dirty state, missing block allocation, counters, payload)'
