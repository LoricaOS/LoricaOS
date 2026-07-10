#!/bin/bash
# tools/fetch-make.sh — fetch the GNU make source into references/make/.
# make is built static-musl (tools/build-make.sh) as an on-device build driver
# for self-hosting. See the self-hosting roadmap.
set -e
REPO="$(git rev-parse --show-toplevel)"
VER="${MAKE_VERSION:-4.4}"
URL="https://ftp.gnu.org/gnu/make/make-${VER}.tar.gz"
TMP="/tmp/make-${VER}.tar.gz"
SRC="$REPO/references/make"

if [ -f "$SRC/configure" ]; then
    echo "[make] source present at references/make/ — skip"
    exit 0
fi
wget -O "$TMP" "$URL"
mkdir -p "$REPO/references"
rm -rf "$SRC"
tar -C "$REPO/references" -xzf "$TMP"
mv "$REPO/references/make-${VER}" "$SRC"
echo "[make] source extracted to references/make/ (${VER})"
