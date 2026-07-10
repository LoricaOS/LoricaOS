#!/bin/bash
# tools/fetch-kernel-toolchain.sh — fetch a STATIC musl-native GCC + binutils.
#
# The Aegis kernel builds fine with an ordinary musl-targeting gcc + GNU ld +
# nasm (no bare-metal x86_64-elf- cross compiler needed — verified: it boots +
# passes captest). To build the kernel ON AEGIS the toolchain binaries must run
# on Aegis, i.e. be static (no interpreter/lib deps). musl.cc's native toolchain
# ships statically-linked gcc/cc1/as/ld/nm/objcopy — they run unmodified on
# Aegis (which speaks the Linux syscall ABI). See the self-hosting roadmap.
#
# ~85MB download, ~180MB extracted (GCC 11.2.1). Installs under build/.
set -e
REPO="$(git rev-parse --show-toplevel)"
URL="${KTC_URL:-https://musl.cc/x86_64-linux-musl-native.tgz}"
TMP="/tmp/x86_64-linux-musl-native.tgz"
OUT="$REPO/build/kernel-toolchain"

if [ -x "$OUT/bin/gcc" ]; then
    echo "[ktc] toolchain present at build/kernel-toolchain — skip"
    exit 0
fi
[ -f "$TMP" ] || wget -O "$TMP" "$URL"
mkdir -p "$REPO/build"
rm -rf "$OUT" "$REPO/build/x86_64-linux-musl-native"
tar -C "$REPO/build" -xzf "$TMP"
mv "$REPO/build/x86_64-linux-musl-native" "$OUT"
echo "[ktc] static musl gcc/binutils -> build/kernel-toolchain"
"$OUT/bin/gcc" --version | head -1
