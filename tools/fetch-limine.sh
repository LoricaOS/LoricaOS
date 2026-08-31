#!/usr/bin/env bash
# fetch-limine.sh — fetch the pinned Limine binary release into tools/limine/
# instead of vendoring ~4 MB of bootloader blobs in git.
#
# Limine ships a prebuilt binary bundle with each release; we take the
# x86_64 BIOS+UEFI subset Aegis needs for its ISO + installer ESP, plus the
# host deploy-tool source (limine.c). The version is pinned in tools/limine/VERSION
# (version and SHA-256). Cached under vendor/ like fetch-kernel.sh.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

DEST=tools/limine
read -r VER EXPECTED < "$DEST/VERSION"
URL="https://github.com/limine-bootloader/limine/releases/download/$VER/limine-binary.tar.xz"
CACHE="vendor/limine-$VER"
ARCHIVE="vendor/limine-$VER.tar.xz"
FILES="BOOTX64.EFI BOOTIA32.EFI limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin limine-bios-hdd.h limine.c LICENSE"

if [ ! -f "$ARCHIVE" ]; then
    echo "[fetch-limine] downloading Limine $VER"
    echo "[fetch-limine]   $URL"
    curl -fsSL "$URL" -o "$ARCHIVE.tmp" || { rm -f "$ARCHIVE.tmp"; exit 1; }
    mv "$ARCHIVE.tmp" "$ARCHIVE"
else
    echo "[fetch-limine] using cached $ARCHIVE"
fi

FOUND="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
[ "$FOUND" = "$EXPECTED" ] || {
    echo "[fetch-limine] ERROR: checksum mismatch (expected $EXPECTED, found $FOUND)" >&2
    exit 1;
}
rm -rf "$CACHE.tmp"
mkdir -p "$CACHE.tmp"
tar -xJf "$ARCHIVE" -C "$CACHE.tmp" --strip-components=1
rm -rf "$CACHE"
mv "$CACHE.tmp" "$CACHE"

mkdir -p "$DEST"
for f in $FILES; do
    [ -f "$CACHE/$f" ] || { echo "[fetch-limine] ERROR: $f missing in Limine $VER" >&2; exit 1; }
    cp "$CACHE/$f" "$DEST/$f"
done
echo "[fetch-limine] Limine $VER -> $DEST"
