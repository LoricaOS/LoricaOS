#!/usr/bin/env bash
# fetch-limine.sh — fetch the pinned Limine binary release into tools/limine/
# instead of vendoring ~4 MB of bootloader blobs in git.
#
# Limine ships prebuilt binaries on its `vX.x-binary` branch; we take the
# x86_64 BIOS+UEFI subset Aegis needs for its ISO + installer ESP, plus the
# host deploy-tool source (limine.c). The version is pinned in tools/limine/VERSION
# (first token, e.g. v11.4.1-binary). Cached under vendor/ like fetch-kernel.sh.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

DEST=tools/limine
VER="$(awk 'NR==1{print $1; exit}' "$DEST/VERSION")"
URL=https://github.com/limine-bootloader/limine.git
CACHE="vendor/limine-$VER"
FILES="BOOTX64.EFI BOOTIA32.EFI limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin limine-bios-hdd.h limine.c LICENSE"

if [ ! -d "$CACHE" ]; then
    echo "[fetch-limine] cloning Limine $VER"
    echo "[fetch-limine]   $URL ($VER)"
    rm -rf "$CACHE.tmp"
    if ! git clone --depth 1 --branch "$VER" "$URL" "$CACHE.tmp" >/dev/null 2>&1; then
        echo "[fetch-limine] ERROR: could not clone Limine $VER from $URL" >&2
        echo "[fetch-limine] (populate $CACHE manually to build offline)" >&2
        rm -rf "$CACHE.tmp"; exit 1
    fi
    rm -rf "$CACHE.tmp/.git"
    mv "$CACHE.tmp" "$CACHE"
else
    echo "[fetch-limine] using cached $CACHE"
fi

mkdir -p "$DEST"
for f in $FILES; do
    [ -f "$CACHE/$f" ] || { echo "[fetch-limine] ERROR: $f missing in Limine $VER" >&2; exit 1; }
    cp "$CACHE/$f" "$DEST/$f"
done
echo "[fetch-limine] Limine $VER -> $DEST"
