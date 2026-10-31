#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir "$TMP/root"
${CC:-cc} -std=c99 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror \
    -I"$ROOT/user/bin/herald" "$ROOT/tools/herald-state-test.c" \
    "$ROOT/user/bin/herald/transaction.c" "$ROOT/user/bin/herald/db.c" \
    -o "$TMP/check"
"$TMP/check" "$TMP/root"
