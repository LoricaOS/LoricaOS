#!/bin/bash
# tools/fetch-nasm.sh — fetch the NASM source into references/nasm/.
# NASM assembles the x86_64 kernel's *.asm; built static-musl (build-nasm.sh) it
# runs on Aegis, part of the on-device kernel toolchain. See self-hosting roadmap.
set -e
REPO="$(git rev-parse --show-toplevel)"
VER="${NASM_VERSION:-2.16.03}"
URL="https://www.nasm.us/pub/nasm/releasebuilds/${VER}/nasm-${VER}.tar.gz"
TMP="/tmp/nasm-${VER}.tar.gz"
SRC="$REPO/references/nasm"

if [ -f "$SRC/configure" ]; then
    echo "[nasm] source present at references/nasm/ — skip"
    exit 0
fi
wget -O "$TMP" "$URL"
mkdir -p "$REPO/references"
rm -rf "$SRC"
tar -C "$REPO/references" -xzf "$TMP"
mv "$REPO/references/nasm-${VER}" "$SRC"
echo "[nasm] source extracted to references/nasm/ (${VER})"
