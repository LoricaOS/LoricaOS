#!/bin/bash
# build-arm64-hpkgs.sh — build + sign the aarch64 .hpkg for every LoricaOS
# component, and optionally add them to a Chancery repo (→ binary-arm64,
# published alongside the x86_64 slice). This is the "everything gets arm64"
# step: it reuses build-arm64-desktop.sh's per-component aarch64 build, then
# runs each repo's arch-aware tools/pack.sh with ARCH=arm64.
#
#   OUT=<dir>            where the .hpkg land (default ~/arm64-hpkgs)
#   HERALD_KEY=<key>     signing key (default build/herald-keys/herald-dev.key)
#   CHANCERY_REPO=<dir>  if set, `chancery add` each package here + publish
# Runs on CT117 (needs the mirrored component source + arm64 toolchain).
set -euo pipefail
REPO="${REPO:-/root/loricaos}"
export PATH="/usr/local/bin:$PATH"
KEY="${HERALD_KEY:-$REPO/build/herald-keys/herald-dev.key}"
STRIP="${STRIP:-aarch64-linux-gnu-strip}"
OUT="${OUT:-$HOME/arm64-hpkgs}"
CHANCERY_REPO="${CHANCERY_REPO:-}"
CH="${CH:-/root/chancery/target/release/chancery}"
COMPONENTS="lumen bastion citadel-dock lumen-terminal lumen-calculator \
    lumen-editor lumen-filemanager lumen-settings lumen-sysmon lumen-netman \
    lumen-calendar lumen-imageviewer lumen-tunes lumen-run lumen-feeds \
    lumen-2048 lumen-snake lumen-minesweeper lumen-applications-menu lumen-dbg"
log(){ echo "[arm64-hpkgs] $*"; }

# 1. Build every component's aarch64 binary (leaves lumen/bastion/dock .elf +
#    each app's component.elf in its repo — exactly what tools/pack.sh consumes).
log "== build component binaries (arm64) =="
bash "$REPO/tools/build-arm64-desktop.sh" >/dev/null

rm -rf "$OUT"; mkdir -p "$OUT"
n=0
for r in $COMPONENTS; do
    R="/root/$r"; [ -d "$R" ] || { log "SKIP $r (not mirrored)"; continue; }
    # bastion is compiled to bastion.elf (for /bin) but its generic pack.sh
    # (like the apps') reads component.elf — provide it. lumen/dock have bespoke
    # pack.sh that read their own .elf, so this only matters for bastion.
    [ -f "$R/component.elf" ] || { [ -f "$R/bastion.elf" ] && cp "$R/bastion.elf" "$R/component.elf"; }
    if ! ( cd "$R" && ARCH=arm64 STRIP="$STRIP" HERALD_KEY="$KEY" sh tools/pack.sh ) >/tmp/pack-$r.log 2>&1; then
        log "SKIP $r (pack failed; /tmp/pack-$r.log)"; continue
    fi
    for f in "$R"/*-arm64.hpkg; do
        [ -f "$f" ] || continue
        cp "$f" "$OUT/"; [ -f "$f.sig" ] && cp "$f.sig" "$OUT/"; n=$((n+1))
    done
done
log "arm64 .hpkg built: $n"
ls "$OUT"/*.hpkg 2>/dev/null | sed 's|.*/||' | sort

# 2. Optionally populate a Chancery repo (auto-registers arm64 + publishes
#    binary-arm64/Packages next to binary-x86_64).
if [ -n "$CHANCERY_REPO" ]; then
    log "== chancery add → $CHANCERY_REPO =="
    ( cd "$CHANCERY_REPO"
      for f in "$OUT"/*-arm64.hpkg; do "$CH" add "$f" --suite stable --component main >/dev/null; done
      "$CH" publish )
    log "published; arches: $(ls "$CHANCERY_REPO"/dists/stable/main/ | tr '\n' ' ')"
fi
