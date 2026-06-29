#!/bin/bash
#
# herald-index.sh — generate a signed repository index.
#
# A herald repository is just a static file tree:
#     <repo>/index            signed catalog (this tool writes it)
#     <repo>/index.sig        detached ECDSA-P256/SHA-256 signature
#     <repo>/pool/<pkg>.hpkg  the packages (+ their .hpkg.sig)
#
# This scans <repo>/pool/*.hpkg, reads each package's manifest (id, version),
# and emits one TAB-separated index line per package:
#     <name>\t<version>\t<relpath>\t<sha256>
# then signs the whole index with the dev key. No server software — any static
# HTTP file server (or object store) can host the tree.
#
# Usage: herald-index.sh <repo-dir>
#   HERALD_KEY=<path>   signing key (default build/herald-keys/herald-dev.key)
set -e

REPO="$1"
PRIV="${HERALD_KEY:-build/herald-keys/herald-dev.key}"

if [ -z "$REPO" ] || [ ! -d "$REPO/pool" ]; then
    echo "usage: herald-index.sh <repo-dir>   (with <repo-dir>/pool/*.hpkg)" >&2
    exit 2
fi
if [ ! -f "$PRIV" ]; then
    echo "herald-index: signing key $PRIV not found (run tools/herald-keygen.sh)" >&2
    exit 1
fi

IDX="$REPO/index"
: > "$IDX"

for hpkg in "$REPO"/pool/*.hpkg; do
    [ -f "$hpkg" ] || continue
    rel="pool/$(basename "$hpkg")"
    # The manifest is the first member; tar may store it as "manifest" or "./manifest".
    man="$(tar xfO "$hpkg" manifest 2>/dev/null || tar xfO "$hpkg" ./manifest 2>/dev/null || true)"
    name="$(printf '%s\n' "$man" | sed -n 's/^id=//p' | head -n1)"
    ver="$(printf '%s\n' "$man"  | sed -n 's/^version=//p' | head -n1)"
    sha="$(sha256sum "$hpkg" | cut -d' ' -f1)"
    if [ -z "$name" ] || [ -z "$ver" ]; then
        echo "herald-index: skipping $hpkg (no id/version in manifest)" >&2
        continue
    fi
    printf '%s\t%s\t%s\t%s\n' "$name" "$ver" "$rel" "$sha" >> "$IDX"
done

openssl dgst -sha256 -sign "$PRIV" -out "$IDX.sig" "$IDX"
echo "[herald-index] wrote $IDX ($(wc -l < "$IDX") packages) + index.sig"
