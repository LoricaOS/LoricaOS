#!/usr/bin/env bash
# build-component-pkgs.sh — build the per-component desktop herald packages.
#
# Peels the desktop graphical stack into ONE signed class=system .hpkg per
# component (lumen, bastion, citadel-dock, applications, lumen-probe, and each
# GUI app bundle), plus a `desktop` META-package that depends on them all.
# Each component's payload is DERIVED from rootfs.desktop.manifest (its binary)
# + rootfs-desktop/ (its caps.d / app.ini / vigil service) + (for lumen) the
# fonts/logo assets — so the package set can't drift from the desktop image.
# Excludes gui-installer (installer-only; see build-desktop-pkg.sh).
#
# All components are class=system: they are first-party, signature-trusted, and
# install across /bin //apps //etc — the uniform path. (Third-party apps still
# use the constrained class=app path; that is unchanged.)
#
# Also emits build/pkgs/herald-db — the herald installed-package database
# (id<TAB>version<TAB>exec<TAB>sha256, one line per package) that the desktop
# image pre-seeds so `herald list` shows the whole graphical stack as installed.
#
# Usage: build-component-pkgs.sh   (HERALD_KEY=<key> to sign; binaries must be built)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

VERSION="$(cat VERSION 2>/dev/null || echo 0.0.0)"
HERALD_KEY="${HERALD_KEY:-tools/herald-keys/herald.key}"
STRIP_TOOL="$(command -v x86_64-linux-gnu-strip || command -v strip || true)"
MANIFEST=rootfs.desktop.manifest
SKEL=rootfs-desktop
OUT=build/pkgs
EXCLUDE=gui-installer

[ -f "$HERALD_KEY" ] || { echo "build-component-pkgs: signing key $HERALD_KEY not found" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
DB="$OUT/herald-db"; : > "$DB"
NAMES=""

pack_one() {  # <name> <binary-src> <binary-dest>
    local name="$1" binsrc="$2" bindest="$3" stage rel hpkg sha roots
    stage="$(mktemp -d)"
    rel="${bindest#/}"
    mkdir -p "$stage/$(dirname "$rel")"
    if [ -n "$STRIP_TOOL" ] && "$STRIP_TOOL" -o "$stage/$rel" "$binsrc" 2>/dev/null; then :; else cp "$binsrc" "$stage/$rel"; fi
    chmod 0755 "$stage/$rel"
    [ -f "$SKEL/etc/aegis/caps.d/$name" ] && { mkdir -p "$stage/etc/aegis/caps.d"; cp "$SKEL/etc/aegis/caps.d/$name" "$stage/etc/aegis/caps.d/$name"; }
    [ -f "$SKEL/apps/$name/app.ini" ]     && { mkdir -p "$stage/apps/$name"; cp "$SKEL/apps/$name/app.ini" "$stage/apps/$name/app.ini"; }
    [ -d "$SKEL/etc/vigil/services/$name" ] && { mkdir -p "$stage/etc/vigil/services/$name"; cp -R "$SKEL/etc/vigil/services/$name/." "$stage/etc/vigil/services/$name/"; }
    if [ "$name" = lumen ]; then
        mkdir -p "$stage/usr/share/fonts"
        cp assets/*.ttf "$stage/usr/share/fonts/" 2>/dev/null || true
        for raw in build/logo.raw build/claude.raw; do [ -f "$raw" ] && cp "$raw" "$stage/usr/share/$(basename "$raw")"; done
    fi
    printf 'id=%s\nname=%s\nversion=%s\nclass=system\n' "$name" "$name" "$VERSION" > "$stage/manifest"
    hpkg="$OUT/${name}_${VERSION}_x86_64.hpkg"
    roots="$(cd "$stage" && ls -A | grep -v '^manifest$' | tr '\n' ' ')"
    ( cd "$stage" && tar --format=ustar -cf "$ROOT/$hpkg" manifest $roots )
    openssl dgst -sha256 -sign "$HERALD_KEY" -out "$hpkg.sig" "$hpkg"
    sha="$(sha256sum "$hpkg" | cut -d' ' -f1)"
    printf '%s\t%s\t\t%s\n' "$name" "$VERSION" "$sha" >> "$DB"
    rm -rf "$stage"
    echo "[component] $name -> $(basename "$hpkg")"
}

while IFS= read -r line; do
    line="${line%%#*}"; line="$(echo "$line" | xargs)"; [ -z "$line" ] && continue
    src="$(echo "$line" | awk '{print $1}')"; dest="$(echo "$line" | awk '{print $2}')"
    name="$(basename "$dest")"
    [ "$name" = "$EXCLUDE" ] && continue
    [ -f "$src" ] || { echo "build-component-pkgs: ERROR: $src not built (build the desktop profile first)" >&2; exit 1; }
    pack_one "$name" "$src" "$dest"
    NAMES="$NAMES $name"
done < "$MANIFEST"

# desktop meta-package: depends on every component, no payload of its own.
stage="$(mktemp -d)"
printf 'id=desktop\nname=AspisOS Desktop\nversion=%s\nclass=system\ndepends=%s\n' \
    "$VERSION" "$(echo $NAMES | xargs)" > "$stage/manifest"
( cd "$stage" && tar --format=ustar -cf "$ROOT/$OUT/desktop_${VERSION}_x86_64.hpkg" manifest )
openssl dgst -sha256 -sign "$HERALD_KEY" -out "$OUT/desktop_${VERSION}_x86_64.hpkg.sig" "$OUT/desktop_${VERSION}_x86_64.hpkg"
sha="$(sha256sum "$OUT/desktop_${VERSION}_x86_64.hpkg" | cut -d' ' -f1)"
printf 'desktop\t%s\t\t%s\n' "$VERSION" "$sha" >> "$DB"
rm -rf "$stage"

echo "[component] desktop meta depends:$NAMES"
echo "[component] wrote $(grep -c . "$DB") db entries -> $DB"
