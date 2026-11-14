#!/bin/sh
# fetch-coreutils.sh — download the pinned coreutils.hpkg artifact (built by the
# LoricaOS/coreutils repo's CI) and unpack its /bin payload into
# vendor/coreutils/bin/ for the base rootfs build. Mirrors fetch-kernel.sh /
# fetch-components.sh: a versioned, checksum-pinned fetched dependency.
# Release artifacts are unsigned; the committed lock authenticates build input.
set -eu
cd "$(dirname "$0")/.."

VER="$(cat COREUTILS_VERSION)"
DEST=vendor/coreutils
CACHE="$DEST/coreutils-$VER.hpkg"
URL="https://github.com/LoricaOS/coreutils/releases/download/v$VER/coreutils.hpkg"

LOCK=tools/coreutils.sha256
EXPECTED="$(awk -v ver="$VER" '$1 == ver { print $2; exit }' "$LOCK")"
[ "${#EXPECTED}" = 64 ] || { echo "[fetch-coreutils] missing checksum for $VER" >&2; exit 1; }
case "$EXPECTED" in *[!0-9a-f]*) echo "[fetch-coreutils] invalid checksum" >&2; exit 1 ;; esac
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

# Verify an immutable copy before extraction. A failed fetch or verification
# must leave the previous extracted tools and success stamp untouched.
STAGE="$(mktemp -d "$DEST/.stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM
cp "$CACHE" "$STAGE/package.hpkg"
FOUND="$(sha256sum "$STAGE/package.hpkg" | cut -d' ' -f1)"
[ "$FOUND" = "$EXPECTED" ] || { echo "[fetch-coreutils] checksum mismatch for $CACHE" >&2; exit 1; }
mkdir "$STAGE/payload"
tar xf "$STAGE/package.hpkg" -C "$STAGE/payload"
[ "$(sed -n 's/^id=//p' "$STAGE/payload/manifest")" = coreutils ] &&
[ "$(sed -n 's/^version=//p' "$STAGE/payload/manifest")" = "$VER" ] &&
[ -d "$STAGE/payload/bin" ] || { echo "[fetch-coreutils] wrong package identity or payload" >&2; exit 1; }
# Rename the verified tree into place; retain the previous tree if publication fails.
if [ -e "$DEST/bin" ]; then mv "$DEST/bin" "$STAGE/previous-bin"; fi
if ! mv "$STAGE/payload/bin" "$DEST/bin"; then
    [ ! -e "$STAGE/previous-bin" ] || mv "$STAGE/previous-bin" "$DEST/bin"
    exit 1
fi
touch "$DEST/.fetched"
echo "[fetch-coreutils] $(ls "$DEST/bin" | wc -l | tr -d ' ') verified binaries -> $DEST/bin"
