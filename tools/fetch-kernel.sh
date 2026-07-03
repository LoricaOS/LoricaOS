#!/usr/bin/env bash
# Fetch the Aegis kernel image LoricaOS is built against.
#
# Resolution order:
#   1. local cache  vendor/aegis[-arm64]-<version>.elf   (committed or pre-populated)
#   2. download     <release URL>/v<version>/aegis[-arm64].elf
#
# The kernel is a versioned ARTIFACT — LoricaOS does not build it. OS and kernel
# versions move independently; this is the single point that couples them.
#
# The 3rd arg (or $ARCH) selects the arch: x86_64 (default) → aegis.elf,
# arm64/aarch64 → aegis-arm64.elf. Each arch is a separate release artifact.
set -eu

VER="${1:?usage: fetch-kernel.sh <version> <dest-path> [arch]}"
DEST="${2:?usage: fetch-kernel.sh <version> <dest-path> [arch]}"
ARCH="${3:-${ARCH:-x86_64}}"

case "$ARCH" in
    arm64|aarch64) SUFFIX="-arm64" ;;
    x86_64|amd64|"") SUFFIX="" ;;
    *) echo "[fetch-kernel] ERROR: unknown arch '$ARCH'" >&2; exit 1 ;;
esac

CACHE="vendor/aegis${SUFFIX}-${VER}.elf"
URL="https://github.com/LoricaOS/Aegis/releases/download/v${VER}/aegis${SUFFIX}.elf"

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
