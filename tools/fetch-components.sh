#!/usr/bin/env bash
# fetch-components.sh — download the desktop component packages from their
# LoricaOS releases, unpack them into a single overlay tree, and emit the herald
# installed-package db the desktop image pre-seeds.
#
# This is what makes the loricaos build CONSUME the per-component repos instead of
# building the graphical stack from in-tree source. Mirrors fetch-kernel.sh: each
# package is a pinned, cached, versioned artifact (vendor/components/).
#
# Outputs:
#   build/desktop-overlay/      merged payload of every component (bin/, apps/,
#                               etc/aegis/caps.d/, etc/vigil/, usr/share/, ...)
#   build/desktop-overlay.db    id<TAB>version<TAB>exec<TAB>sha256 per component
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

LIST=tools/components.list
VENDOR=vendor/components
OVERLAY=build/desktop-overlay
DB=build/desktop-overlay.db
BASE=https://github.com/LoricaOS

mkdir -p "$VENDOR" build
rm -rf "$OVERLAY"; mkdir -p "$OVERLAY"
: > "$DB"

while read -r id ver _; do
    [ -z "${id:-}" ] && continue
    case "$id" in \#*) continue ;; esac
    cache="$VENDOR/${id}-${ver}.hpkg"
    if [ ! -f "$cache" ]; then
        url="$BASE/$id/releases/download/v$ver/$id.hpkg"
        echo "[fetch-components] $id v$ver"
        echo "[fetch-components]   $url"
        if ! curl -fsSL "$url" -o "$cache.tmp"; then
            echo "[fetch-components] ERROR: could not fetch $id v$ver" >&2
            echo "[fetch-components] (place it at $cache to build offline)" >&2
            rm -f "$cache.tmp"; exit 1
        fi
        mv "$cache.tmp" "$cache"
    else
        echo "[fetch-components] using cached $cache"
    fi
    # Unpack the package payload (everything except the manifest) into the
    # shared overlay; later packages merge alongside earlier ones.
    tar xf "$cache" -C "$OVERLAY" --exclude=manifest
    sha="$(sha256sum "$cache" | cut -d' ' -f1)"
    printf '%s\t%s\t\t%s\n' "$id" "$ver" "$sha" >> "$DB"
done < "$LIST"

echo "[fetch-components] overlay -> $OVERLAY ($(find "$OVERLAY" -type f | wc -l | tr -d ' ') files)"
echo "[fetch-components] db -> $DB ($(grep -c . "$DB") components)"
