#!/bin/bash
# Build .hpkg packages for snake, minesweeper, imageviewer on the container.
set -e
cd /root/aegis
export PATH=/usr/local/bin:$PATH

# Ensure the dev signing key exists (herald-pack needs it)
if [ ! -f build/herald-keys/herald-dev.key ]; then
    echo "NO DEV KEY — generating"; bash tools/herald-keygen.sh || true
fi

OUT=/root/hpkgs
rm -rf "$OUT"; mkdir -p "$OUT"

pack() {
    id="$1"; name="$2"; elf="$3"; appini="$4"
    stage=$(mktemp -d)
    mkdir -p "$stage/apps/$id"
    cp "$elf" "$stage/apps/$id/$id"
    chmod 0755 "$stage/apps/$id/$id"
    if [ -f "$appini" ]; then cp "$appini" "$stage/apps/$id/app.ini";
    else printf 'name=%s\nexec=%s\n' "$name" "$id" > "$stage/apps/$id/app.ini"; fi
    printf 'id=%s\nname=%s\nversion=1.0.0\nexec=%s\n' "$id" "$name" "$id" > "$stage/manifest"
    bash tools/herald-pack.sh "$stage" "$OUT/${id}_1.0.0_x86_64.hpkg"
    rm -rf "$stage"
}

pack snake       Snake       user/bin/snake/snake.elf             user/bin/snake/app.ini
pack minesweeper Minesweeper user/bin/minesweeper/minesweeper.elf user/bin/minesweeper/app.ini
pack imageviewer Images      user/bin/imageviewer/imageviewer.elf rootfs/apps/imageviewer/app.ini

echo "=== built packages ==="
ls -la "$OUT"
