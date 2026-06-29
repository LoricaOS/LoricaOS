#!/usr/bin/env bash
# Fetch the Aegis kernel image AspisOS is built against.
#
# Resolution order:
#   1. local cache  vendor/aegis-<version>.elf   (committed or pre-populated)
#   2. download     <release URL>/v<version>/aegis.elf
#
# The kernel is a versioned ARTIFACT — AspisOS does not build it. OS and kernel
# versions move independently; this is the single point that couples them.
set -eu

VER="${1:?usage: fetch-kernel.sh <version> <dest-path>}"
DEST="${2:?usage: fetch-kernel.sh <version> <dest-path>}"

CACHE="vendor/aegis-${VER}.elf"
URL="https://github.com/AspisOS/Aegis/releases/download/v${VER}/aegis.elf"

mkdir -p vendor "$(dirname "$DEST")"

if [ -f "$CACHE" ]; then
    echo "[fetch-kernel] using cached kernel $CACHE (v$VER)"
else
    echo "[fetch-kernel] downloading Aegis kernel v$VER"
    echo "[fetch-kernel]   $URL"
    if ! curl -fsSL "$URL" -o "$CACHE.tmp"; then
        echo "[fetch-kernel] ERROR: could not fetch kernel v$VER" >&2
        echo "[fetch-kernel] (place it at $CACHE manually to build offline)" >&2
        rm -f "$CACHE.tmp"
        exit 1
    fi
    mv "$CACHE.tmp" "$CACHE"
fi

cp "$CACHE" "$DEST"
echo "$VER" > "$(dirname "$DEST")/.kernel-version"
echo "[fetch-kernel] kernel v$VER -> $DEST"
