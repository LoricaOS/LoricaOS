#!/bin/bash
#
# build-desktop-pkg.sh — build the signed `desktop` herald system package.
#
# Produces a class=system .hpkg whose payload is exactly the graphical delta a
# desktop ISO adds on top of a server: the compositor/display-manager/dock/
# launcher binaries, the GUI app bundles, their cap policies, the graphical
# vigil services, and the fonts/logo assets. Installing it on a server with
# `herald install desktop` makes that box desktop-identical on the next boot
# (vigil infers graphical mode from /bin/bastion — see gen-limine-conf.sh).
#
# The payload is DERIVED from the same sources the desktop rootfs is built from
# (rootfs.desktop.manifest + rootfs-desktop/ skeleton + assets), so it can't
# drift from the desktop ISO. Binaries are stripped exactly as build-rootfs.sh
# strips them.
#
# Deliberately EXCLUDES gui-installer: it is the live-ISO installer, auto-removed
# by vigil on installed systems, and carries the spiciest caps (DISK_ADMIN AUTH
# SETUID) — an already-installed box has no use for it, and an installed desktop
# doesn't ship it either, so excluding it keeps parity and drops attack surface.
#
# Usage: build-desktop-pkg.sh [out.hpkg]
#   defaults to build/desktop_<VERSION>_x86_64.hpkg
#   HERALD_KEY=<path>  signing key (forwarded to herald-pack.sh)
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

VERSION="$(cat VERSION 2>/dev/null || echo 0.0.0)"
OUT="${1:-build/desktop_${VERSION}_x86_64.hpkg}"
MANIFEST="rootfs.desktop.manifest"
SKEL="rootfs-desktop"
EXCLUDE_ID="gui-installer"   # installer-only; see header

STRIP_TOOL="$(command -v x86_64-linux-gnu-strip || command -v strip || true)"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# ── 1. Binaries + app ELFs from the desktop manifest (SOURCE DEST lines) ─────
while IFS= read -r line; do
    line="${line%%#*}"
    line="$(echo "$line" | xargs)"
    [[ -z "$line" ]] && continue
    src="$(echo "$line" | awk '{print $1}')"
    dest="$(echo "$line" | awk '{print $2}')"          # e.g. /bin/lumen, /apps/x/x
    case "$dest" in */$EXCLUDE_ID/*|*/$EXCLUDE_ID) continue ;; esac
    if [[ ! -f "$src" ]]; then
        echo "build-desktop-pkg: ERROR: $src not found (build the desktop profile first)" >&2
        exit 1
    fi
    rel="${dest#/}"                                     # strip leading '/'
    mkdir -p "$STAGE/$(dirname "$rel")"
    if [[ -n "$STRIP_TOOL" ]] && "$STRIP_TOOL" -o "$STAGE/$rel" "$src" 2>/dev/null; then
        :   # stripped copy written
    else
        cp "$src" "$STAGE/$rel"
    fi
    chmod 0755 "$STAGE/$rel"
done < "$MANIFEST"

# ── 2. Skeleton: app.ini, caps.d, vigil services (skip the excluded id) ──────
while IFS= read -r -d '' f; do
    rel="${f#$SKEL/}"
    case "$rel" in
        apps/$EXCLUDE_ID/*|etc/aegis/caps.d/$EXCLUDE_ID) continue ;;
    esac
    mkdir -p "$STAGE/$(dirname "$rel")"
    cp "$f" "$STAGE/$rel"
done < <(find "$SKEL" -type f -print0 | sort -z)

# ── 3. Graphical assets (fonts + logo/claude raws), as build-rootfs.sh ships ─
if [[ -d assets ]]; then
    mkdir -p "$STAGE/usr/share/fonts"
    for ttf in assets/*.ttf; do
        [[ -f "$ttf" ]] && cp "$ttf" "$STAGE/usr/share/fonts/$(basename "$ttf")"
    done
fi
mkdir -p "$STAGE/usr/share"
for raw in build/logo.raw build/claude.raw; do
    [[ -f "$raw" && -s "$raw" ]] && cp "$raw" "$STAGE/usr/share/$(basename "$raw")"
done

# ── 4. The system manifest ───────────────────────────────────────────────────
# class=system → herald installs the whole tree verbatim, trusting the package
# signature (no caps allow-list / exec==id). No exec (it's not one launcher).
cat > "$STAGE/manifest" <<EOF
id=desktop
name=AspisOS Desktop
version=$VERSION
class=system
EOF

# ── 5. Pack + sign (herald-pack.sh requires manifest + apps/, both present) ──
mkdir -p "$(dirname "$OUT")"
bash tools/herald-pack.sh "$STAGE" "$OUT"
echo "[build-desktop-pkg] desktop $VERSION -> $OUT"
echo "[build-desktop-pkg] payload:"
( cd "$STAGE" && find . -type f | sed 's|^\./|  |' | sort )
