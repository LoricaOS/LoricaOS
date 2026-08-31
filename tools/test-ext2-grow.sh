#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

cc -std=c99 -O2 -Wall -Wextra -Werror \
    -I"$ROOT/user/lib/libinstall" \
    "$ROOT/tools/ext2-grow-file.c" \
    "$ROOT/user/lib/libinstall/ext2_grow.c" \
    -o "$TMP/ext2-grow-file"

truncate -s 44M "$TMP/base.ext2"
mke2fs -q -t ext2 -F -b 4096 -i 32768 "$TMP/base.ext2"
printf 'growth-check\n' > "$TMP/payload"
debugfs -w -R "write $TMP/payload /growth-check" "$TMP/base.ext2" >/dev/null 2>&1

check_size() {
    size=$1 expected=$2 name=$3
    cp "$TMP/base.ext2" "$TMP/$name.ext2"
    truncate -s "$size" "$TMP/$name.ext2"
    "$TMP/ext2-grow-file" "$TMP/$name.ext2"
    e2fsck -fn "$TMP/$name.ext2" > "$TMP/e2fsck-$name.log" 2>&1
    blocks=$(dumpe2fs -h "$TMP/$name.ext2" 2>/dev/null |
        awk -F: '/Block count:/ { gsub(/ /, "", $2); print $2 }')
    [ "$blocks" = "$expected" ] || {
        echo "ext2-grow: $name has wrong block count: $blocks" >&2
        exit 1
    }
    debugfs -R 'cat /growth-check' "$TMP/$name.ext2" 2>/dev/null |
        grep -qx 'growth-check'
}

check_size 96M  24576  partial-first-group
check_size 137M 35072  partial-last-group
check_size 1G   262144 full-groups
echo "ext2-grow: PASS (partial/full groups, clean e2fsck, payload preserved)"
