#!/bin/bash
# tools/build-tcc.sh — build tcc static-musl and install it to build/tcc-install.
#
# Produces a self-contained tcc toolchain (compiler + assembler + linker + its
# runtime libtcc1.a + headers) that runs on Aegis and emits Aegis binaries. The
# resulting tree (build/tcc-install/usr/local/{bin/tcc,lib/tcc/*}) is what
# tools/tcc-iso.sh stages onto a rootfs for the on-Aegis proof.
#
# --config-musl makes tcc target musl (crt names, gnu-hash, TLS model). tcc's
# own compiler is built with musl-gcc so the tcc binary itself is a static musl
# ELF — directly runnable on Aegis.
set -e
REPO="${REPO:-$(git rev-parse --show-toplevel)}"
CC="${CC:-musl-gcc}"
SRC="$REPO/references/tinycc"
OUT="$REPO/build/tcc-install"

[ -f "$SRC/configure" ] || bash "$REPO/tools/fetch-tcc.sh"

cd "$SRC"
./configure \
    --cc="$CC" \
    --extra-cflags="-O2 -static" \
    --extra-ldflags="-static" \
    --config-musl

make
rm -rf "$OUT"
make install DESTDIR="$OUT" prefix=/usr/local

echo "[tcc] installed -> $OUT/usr/local (bin/tcc + lib/tcc runtime)"
"$OUT/usr/local/bin/tcc" -v 2>&1 | head -1 || true
