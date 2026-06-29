#!/bin/bash
# tools/build-bearssl.sh — compile BearSSL 0.6 with musl-gcc
set -e

REPO="$(git rev-parse --show-toplevel)"
BEARSSL_VER="0.6"
BEARSSL_URL="https://bearssl.org/bearssl-${BEARSSL_VER}.tar.gz"
BEARSSL_TAR="$REPO/references/bearssl-${BEARSSL_VER}.tar.gz"
BEARSSL_SRC="$REPO/references/bearssl-${BEARSSL_VER}"
STAGING="$REPO/build/bearssl-install"
OBJDIR="$REPO/build/bearssl-objs"

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
    musl-gcc -O2 -I"$BEARSSL_SRC/inc" -I"$BEARSSL_SRC/src" -c "$src" -o "$obj"
done < <(find "$BEARSSL_SRC/src" -name '*.c' -print0)

OBJS=( "$OBJDIR"/*.o )
[ -f "${OBJS[0]}" ] || { echo "[bearssl] ERROR: no objects compiled"; exit 1; }

ar rcs "$STAGING/lib/libbearssl.a" "${OBJS[@]}"
cp "$STAGING/lib/libbearssl.a" "$STAGING/lib64/libbearssl.a"
cp -r "$BEARSSL_SRC/inc/." "$STAGING/include/"

echo "[bearssl] built: $STAGING/lib/libbearssl.a"
