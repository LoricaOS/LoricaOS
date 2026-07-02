#!/usr/bin/env bash
# fetch-components.sh — download the desktop component packages from their
# LoricaOS releases, unpack them into a single overlay tree, and emit the herald
# installed-package db the desktop image pre-seeds.
#
# This is what makes the loricaos build CONSUME the per-component repos instead of
# building the graphical stack from in-tree source. Mirrors fetch-kernel.sh: each
# package is a pinned, cached, versioned artifact (vendor/components/).
#
# STALE-BINARY GUARD (tools/components.list is a lockfile):
#   Each line is `<id> <version> [fingerprint]`. The fingerprint is a content
#   hash of the package PAYLOAD (file contents, not mtimes, not the ECDSA
#   signature — so it is stable across rebuilds of identical source and
#   independent of the .hpkg wrapper). On every build we recompute it and:
#     - pin present + MATCH    -> OK, use it.
#     - pin present + MISMATCH -> HARD FAIL. The vendor .hpkg is not what was
#       locked: either a stale/wrong artifact got into vendor/components/, or
#       you rebuilt the component on purpose. If intentional, re-pin with
#       tools/relock-components.sh. This is the check that stops a stale binary
#       from silently shipping (the whole reason this guard exists).
#     - pin absent -> WARN loudly and print the fingerprint to pin. Never
#       silently accept an unpinned component in a build.
#   compute_fingerprint() below is the single source of truth for the hash;
#   relock-components.sh sources this file to reuse it.
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

# compute_fingerprint <hpkg> — deterministic content hash of the package
# payload. Extracts to a temp dir, hashes every file's CONTENT (sorted by
# path), then hashes that digest list. Ignores mtimes, tar ordering, the
# manifest, and the detached .sig — so two builds of identical source produce
# the same fingerprint, but any change to a shipped file (e.g. a stale binary
# with old branding) changes it. Shared with relock-components.sh.
compute_fingerprint() {
    local hpkg="$1" tmp fp
    tmp="$(mktemp -d)"
    tar xf "$hpkg" -C "$tmp" --exclude=manifest 2>/dev/null
    fp="$(cd "$tmp" && find . -type f -print0 | LC_ALL=C sort -z \
          | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
    rm -rf "$tmp"
    printf '%s' "$fp"
}

# relock-components.sh sources this file (COMPONENTS_LIB=1) purely to reuse
# compute_fingerprint — skip the bundling below in that case.
[ "${COMPONENTS_LIB:-}" = 1 ] && return 0

mkdir -p "$VENDOR" build
rm -rf "$OVERLAY"; mkdir -p "$OVERLAY"
: > "$DB"

unpinned=0
while read -r id ver pin _; do
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
    fi

    # STALE-BINARY GUARD: verify the payload fingerprint against the lock.
    fp="$(compute_fingerprint "$cache")"
    if [ -n "${pin:-}" ]; then
        if [ "$fp" != "$pin" ]; then
            {
              echo ""
              echo "  ========================================================"
              echo "  [fetch-components] FINGERPRINT MISMATCH for $id v$ver"
              echo "    locked: $pin"
              echo "    found:  $fp"
              echo "    file:   $cache"
              echo "  The vendor package is NOT what components.list locked."
              echo "  Either a stale/wrong .hpkg is in vendor/components/, or"
              echo "  you rebuilt $id on purpose. If intentional, re-pin with:"
              echo "      tools/relock-components.sh"
              echo "  ========================================================"
              echo ""
            } >&2
            exit 1
        fi
        echo "[fetch-components] $id v$ver OK (pinned)"
    else
        echo "[fetch-components] WARN: $id v$ver is UNPINNED — pin it with" >&2
        echo "[fetch-components]   tools/relock-components.sh   (fp=$fp)" >&2
        unpinned=1
    fi

    # Unpack the package payload (everything except the manifest) into the
    # shared overlay; later packages merge alongside earlier ones.
    tar xf "$cache" -C "$OVERLAY" --exclude=manifest
    sha="$(sha256sum "$cache" | cut -d' ' -f1)"
    printf '%s\t%s\t\t%s\n' "$id" "$ver" "$sha" >> "$DB"
done < "$LIST"

echo "[fetch-components] overlay -> $OVERLAY ($(find "$OVERLAY" -type f | wc -l | tr -d ' ') files)"
echo "[fetch-components] db -> $DB ($(grep -c . "$DB") components)"
[ "$unpinned" = 1 ] && echo "[fetch-components] NOTE: unpinned components present (see WARN above)." >&2
exit 0
