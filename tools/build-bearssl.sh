#!/bin/bash
# tools/build-bearssl.sh — compile BearSSL 0.6 (no autoconf; just cc the src).
# Arch-parametrizable: set CC/AR/SUFFIX to cross-build (arm64: CC=<aarch64 musl
# gcc> AR=aarch64-linux-gnu-ar SUFFIX=-arm64 → build/bearssl-install-arm64).
set -e

REPO="${REPO:-$(git rev-parse --show-toplevel)}"
CC="${CC:-musl-gcc}"
AR="${AR:-ar}"
SUFFIX="${SUFFIX:-}"
BEARSSL_VER="0.6"
BEARSSL_URL="https://bearssl.org/bearssl-${BEARSSL_VER}.tar.gz"
BEARSSL_TAR="$REPO/references/bearssl-${BEARSSL_VER}.tar.gz"
BEARSSL_SRC="$REPO/references/bearssl-${BEARSSL_VER}"
STAGING="$REPO/build/bearssl-install${SUFFIX}"
OBJDIR="$REPO/build/bearssl-objs${SUFFIX}"

# Download if absent — CI builds start with an empty references/ dir.
if [ ! -d "$BEARSSL_SRC" ]; then
    mkdir -p "$REPO/references"
    if [ ! -f "$BEARSSL_TAR" ]; then
        echo "[bearssl] Downloading BearSSL ${BEARSSL_VER}..."
        curl -L -o "$BEARSSL_TAR" "$BEARSSL_URL"
    fi
    echo "[bearssl] Extracting..."
    tar -xzf "$BEARSSL_TAR" -C "$REPO/references/"
fi

mkdir -p "$STAGING/include" "$STAGING/lib" "$STAGING/lib64" "$OBJDIR"
rm -f "$OBJDIR"/*.o

while IFS= read -r -d '' src; do
    rel="${src#$BEARSSL_SRC/src/}"
    obj="$OBJDIR/$(echo "$rel" | tr '/' '__' | sed 's/\.c$/.o/')"
    "$CC" -O2 -I"$BEARSSL_SRC/inc" -I"$BEARSSL_SRC/src" -c "$src" -o "$obj"
done < <(find "$BEARSSL_SRC/src" -name '*.c' -print0)

OBJS=( "$OBJDIR"/*.o )
[ -f "${OBJS[0]}" ] || { echo "[bearssl] ERROR: no objects compiled"; exit 1; }

"$AR" rcs "$STAGING/lib/libbearssl.a" "${OBJS[@]}"
cp "$STAGING/lib/libbearssl.a" "$STAGING/lib64/libbearssl.a"
cp -r "$BEARSSL_SRC/inc/." "$STAGING/include/"

echo "[bearssl] built: $STAGING/lib/libbearssl.a"
