#!/usr/bin/env bash
# check-desktop-parity.sh — fail if the arm64 desktop build (build-arm64-desktop.sh)
# does not bake EXACTLY the component set the x86 desktop image ships
# (tools/components.list). Keeps the two arches 1:1: games/extras that are
# herald-only on x86 must not be baked into the arm64 image, and vice-versa.
#
# Run in CI (and locally) from anywhere; resolves paths relative to the repo.
set -euo pipefail
cd "$(dirname "$0")/.."

CL=tools/components.list
BD=tools/build-arm64-desktop.sh
[ -f "$CL" ] || { echo "parity: missing $CL" >&2; exit 2; }
[ -f "$BD" ] || { echo "parity: missing $BD" >&2; exit 2; }

# Expected = every herald id in components.list (comments/blank lines skipped).
expected=$(awk '!/^#/ && NF {print $1}' "$CL" | sort -u)

# The arm64 desktop bakes: the four core components (built specially against the
# local glyph toolkit) + everything in COMPONENTS (installed via each pack.sh).
core="lumen bastion citadel-dock lumen-shell"

# Pull the COMPONENTS value (may span multiple backslash-continued lines).
apps=$(awk '
    /^COMPONENTS=/ { inrepo=1 }
    inrepo {
        line=$0
        gsub(/COMPONENTS="/, "", line); gsub(/\\/, "", line); gsub(/"/, "", line)
        printf "%s ", line
        if ($0 !~ /\\[[:space:]]*$/ && $0 ~ /"/) exit
    }
' "$BD")

baked=$(printf '%s %s\n' "$core" "$apps" | tr ' ' '\n' | sed '/^$/d' | sort -u)

missing=$(comm -23 <(echo "$expected") <(echo "$baked"))   # in x86, not baked in arm64
extra=$(comm -13 <(echo "$expected") <(echo "$baked"))     # baked in arm64, not in x86

rc=0
if [ -n "$missing" ]; then
    echo "FAIL: in x86 components.list but NOT baked into arm64 desktop:" >&2
    echo "$missing" | sed 's/^/  - /' >&2
    rc=1
fi
if [ -n "$extra" ]; then
    echo "FAIL: baked into arm64 desktop but NOT in x86 components.list:" >&2
    echo "$extra" | sed 's/^/  + /' >&2
    rc=1
fi
[ $rc -eq 0 ] && echo "OK: arm64 desktop app set is 1:1 with components.list ($(echo "$baked" | wc -l | tr -d ' ') components)."
exit $rc
