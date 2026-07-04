#!/bin/bash
# tools/build-nasm.sh — build NASM static-musl for on-device use.
set -e
REPO="${REPO:-$(git rev-parse --show-toplevel)}"
CC="${CC:-musl-gcc}"
SRC="$REPO/references/nasm"
OUT="$REPO/build/nasm-install"

[ -f "$SRC/configure" ] || bash "$REPO/tools/fetch-nasm.sh"

cd "$SRC"
CC="$CC" CFLAGS="-O2 -static" LDFLAGS="-static" ./configure
make nasm
rm -rf "$OUT"; mkdir -p "$OUT"
cp nasm "$OUT/nasm"
echo "[nasm] built -> $OUT/nasm"
"$OUT/nasm" -v || true
