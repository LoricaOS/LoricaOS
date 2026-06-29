#!/bin/bash
#
# herald-pack.sh — build and sign a herald package (.hpkg).
#
# A .hpkg is an uncompressed POSIX ustar archive whose first entry is the
# `manifest` and which carries the /apps bundle (apps/<id>/<exec> + app.ini).
# This tool packs a pre-staged source directory and produces a detached
# ECDSA-P256/SHA-256 signature (<out>.sig) over the whole archive, using the
# dev private key from tools/herald-keygen.sh.
#
# The staging dir must contain:
#     manifest
#     apps/<id>/<exec>
#     apps/<id>/app.ini
#
# Usage: herald-pack.sh <staging-dir> <out.hpkg>
#   HERALD_KEY=<path>   override the signing key (default build/herald-keys/herald-dev.key)
set -e

SRC="$1"
OUT="$2"
PRIV="${HERALD_KEY:-build/herald-keys/herald-dev.key}"

if [ -z "$SRC" ] || [ -z "$OUT" ]; then
    echo "usage: herald-pack.sh <staging-dir> <out.hpkg>" >&2
    exit 2
fi
if [ ! -f "$SRC/manifest" ]; then
    echo "herald-pack: $SRC/manifest not found" >&2
    exit 1
fi
if [ ! -d "$SRC/apps" ]; then
    echo "herald-pack: $SRC/apps not found" >&2
    exit 1
fi
if [ ! -f "$PRIV" ]; then
    echo "herald-pack: signing key $PRIV not found (run tools/herald-keygen.sh)" >&2
    exit 1
fi

# manifest first so the extractor can read it without scanning. Uncompressed
# ustar so the in-tree extractor (no inflate) can read it. After manifest+apps,
# include any extra top-level install trees (e.g. lib/ for a package that ships
# a shared-library closure under a manifest-declared paths= prefix, like the
# Lantern browser bundling the Ladybird engine under /lib/ladybird).
extra=$(cd "$SRC" && ls -A | grep -vE '^(manifest|apps)$' | tr '\n' ' ')
tar --format=ustar -C "$SRC" -cf "$OUT" manifest apps $extra

openssl dgst -sha256 -sign "$PRIV" -out "$OUT.sig" "$OUT"

echo "[herald-pack] wrote $OUT ($(wc -c < "$OUT") bytes) + $OUT.sig"
