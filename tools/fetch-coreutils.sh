#!/bin/sh
# fetch-coreutils.sh — download the pinned coreutils.hpkg artifact (built by the
# LoricaOS/coreutils repo's CI) and unpack its /bin payload into
# vendor/coreutils/bin/ for the base rootfs build. Mirrors fetch-kernel.sh /
# fetch-components.sh: a versioned, signature-trusted fetched dependency.
set -eu
cd "$(dirname "$0")/.."

VER="$(cat COREUTILS_VERSION)"
DEST=vendor/coreutils
CACHE="$DEST/coreutils-$VER.hpkg"
URL="https://github.com/LoricaOS/coreutils/releases/download/v$VER/coreutils.hpkg"

mkdir -p "$DEST"
if [ ! -f "$CACHE" ]; then
    echo "[fetch-coreutils] coreutils v$VER"
    echo "[fetch-coreutils]   $URL"
    if ! curl -fsSL "$URL" -o "$CACHE.tmp"; then
        echo "[fetch-coreutils] ERROR: could not fetch coreutils v$VER" >&2
        echo "[fetch-coreutils] (place it at $CACHE to build offline)" >&2
        rm -f "$CACHE.tmp"; exit 1
    fi
    mv "$CACHE.tmp" "$CACHE"
else
    echo "[fetch-coreutils] using cached $CACHE"
fi

# Unpack the /bin payload (everything except the manifest).
rm -rf "$DEST/bin"
tar xf "$CACHE" -C "$DEST" --exclude=manifest
touch "$DEST/.fetched"
echo "[fetch-coreutils] $(ls "$DEST/bin" | wc -l | tr -d ' ') binaries -> $DEST/bin"
