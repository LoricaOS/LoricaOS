#!/usr/bin/env bash
# relock-components.sh — re-pin tools/components.list to the current
# vendor/components/*.hpkg payloads.
#
# Run this AFTER intentionally changing what a component ships (rebuilding it
# from source and staging its .hpkg into vendor/components/, or pulling a new
# release). It recomputes each component's payload fingerprint and rewrites the
# 3rd column of components.list. Commit the result: the git diff is the
# provenance record of exactly which components changed.
#
# The whole point of the lockfile is that a stale/wrong binary in
# vendor/components/ fails the build (fetch-components.sh) instead of silently
# shipping — so re-pinning is a DELIBERATE act, never automatic in a build.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

LIST=tools/components.list
VENDOR=vendor/components

# Reuse the exact fingerprint function the build verifies against.
COMPONENTS_LIB=1 . tools/fetch-components.sh

tmp="$(mktemp)"
changed=0
while IFS= read -r line || [ -n "$line" ]; do
    # Preserve comments and blank lines verbatim.
    case "$line" in ''|\#*) printf '%s\n' "$line" >> "$tmp"; continue ;; esac
    set -- $line
    id="$1"; ver="$2"; oldpin="${3:-}"
    cache="$VENDOR/${id}-${ver}.hpkg"
    if [ ! -f "$cache" ]; then
        echo "relock: WARN: $cache missing — leaving $id unchanged" >&2
        printf '%s\n' "$line" >> "$tmp"
        continue
    fi
    fp="$(compute_fingerprint "$cache")"
    printf '%s %s %s\n' "$id" "$ver" "$fp" >> "$tmp"
    if [ "$fp" != "$oldpin" ]; then
        echo "relock: $id $ver -> $fp"
        changed=1
    fi
done < "$LIST"

mv "$tmp" "$LIST"
[ "$changed" = 0 ] && echo "relock: no changes (all pins already current)."
echo "relock: done — review 'git diff $LIST' and commit."
