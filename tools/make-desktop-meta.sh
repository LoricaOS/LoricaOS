#!/usr/bin/env bash
# make-desktop-meta.sh — generate the signed `desktop` meta-package and append
# its herald db entry. The meta has no payload of its own; it depends on every
# component in tools/components.list, so `herald install desktop` from a repo
# resolves and pulls the whole graphical stack.
#
# Output: build/pkgs/desktop_<VER>_x86_64.hpkg (+ .sig), and the `desktop` line
# appended to build/desktop-overlay.db so the desktop image lists it too.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VERSION="$(cat VERSION 2>/dev/null || echo 0.0.0)"
HERALD_KEY="${HERALD_KEY:-tools/herald-keys/herald.key}"
LIST=tools/components.list
DB=build/desktop-overlay.db
OUT=build/pkgs
[ -f "$HERALD_KEY" ] || { echo "make-desktop-meta: signing key $HERALD_KEY not found" >&2; exit 1; }
mkdir -p "$OUT"

deps="$(grep -vE '^\s*#|^\s*$' "$LIST" | awk '{print $1}' | tr '\n' ' ' | sed 's/ *$//')"

stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
printf 'id=desktop\nname=LoricaOS Desktop\nversion=%s\nclass=system\ndepends=%s\n' \
    "$VERSION" "$deps" > "$stage/manifest"
hpkg="$OUT/desktop_${VERSION}_x86_64.hpkg"
( cd "$stage" && tar --format=ustar -cf "$ROOT/$hpkg" manifest )
openssl dgst -sha256 -sign "$HERALD_KEY" -out "$hpkg.sig" "$hpkg"

# Append the desktop entry to the db (only if not already present this build).
if [ -f "$DB" ] && ! grep -q '^desktop	' "$DB"; then
    sha="$(sha256sum "$hpkg" | cut -d' ' -f1)"
    printf 'desktop\t%s\t\t%s\n' "$VERSION" "$sha" >> "$DB"
fi
echo "[make-desktop-meta] desktop $VERSION depends=$deps"
echo "[make-desktop-meta] -> $hpkg"
