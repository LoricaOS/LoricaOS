#!/usr/bin/env bash
# assemble-desktop-rootfs.sh — build the desktop rootfs by ASSEMBLING fetched
# component packages, not by compiling graphical source.
#
#   desktop rootfs = server base
#                  + the fetched component overlay (build/desktop-overlay/)
#                  + the in-tree gui-installer (the live-only installer, not peeled)
#                  + a pre-seeded herald db (build/desktop-overlay.db)
#
# The overlay comes from tools/fetch-components.sh (the per-component repos'
# released .hpkgs). The kernel zero-extends the ramdisk to the ext2 superblock's
# block count, so we grow the fs to a fixed logical size and let trailing zeros
# be truncated for the ISO.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

OUT="${1:?usage: assemble-desktop-rootfs.sh <out.img>}"
SERVER=build/rootfs-server.img
OVERLAY=build/desktop-overlay
DB=build/desktop-overlay.db
GUI_ELF=user/bin/gui-installer/gui-installer.elf
GUI_INI=rootfs-desktop/apps/gui-installer/app.ini
GUI_CAPS=rootfs-desktop/etc/aegis/caps.d/gui-installer
DEBUGFS="${DEBUGFS:-debugfs}"
STRIP="${STRIP_TOOL:-$(command -v x86_64-linux-gnu-strip || command -v strip || true)}"
FS_KIB=49152   # 48 MiB logical fs (kernel zero-extends to this in RAM)

[ -f "$SERVER" ]  || { echo "assemble: need $SERVER (build the server rootfs first)" >&2; exit 1; }
[ -d "$OVERLAY" ] || { echo "assemble: need $OVERLAY (run fetch-components.sh first)" >&2; exit 1; }
[ -f "$GUI_ELF" ] || { echo "assemble: need $GUI_ELF (build gui-installer first)" >&2; exit 1; }

# Server base, grown so there's room for the graphical overlay.
cp "$SERVER" "$OUT"
truncate -s "${FS_KIB}K" "$OUT"
e2fsck -fy "$OUT" >/dev/null 2>&1 || true
resize2fs "$OUT" "${FS_KIB}K" >/dev/null 2>&1 || { echo "assemble: resize2fs failed" >&2; exit 1; }

declare -A MKDIRD
ensure_dir() {
    local d="$1"
    [ -n "${MKDIRD[$d]:-}" ] && return
    local p; p="$(dirname "$d")"
    if [ "$p" != "/" ] && [ "$p" != "." ]; then ensure_dir "$p"; fi
    $DEBUGFS -w -R "mkdir $d" "$OUT" >/dev/null 2>&1 || true
    MKDIRD["$d"]=1
}
write_file() {   # <src> <dest-abs> <octal-mode>
    local src="$1" dest="$2" mode="$3"
    ensure_dir "$(dirname "$dest")"
    $DEBUGFS -w -R "rm $dest" "$OUT" >/dev/null 2>&1 || true
    $DEBUGFS -w -R "write $src $dest" "$OUT" >/dev/null 2>&1
    $DEBUGFS -w -R "set_inode_field $dest mode $mode" "$OUT" >/dev/null 2>&1
}

# 1. The fetched component overlay. Executables (under /bin, and the app binary
#    in each /apps/<id>/ bundle — anything not *.ini) get the exec bit.
n=0
while IFS= read -r f; do
    rel="${f#"$OVERLAY"/}"; dest="/$rel"; mode=0100644
    case "$dest" in
        /bin/*)  mode=0100755 ;;
        /apps/*) [[ "$dest" != *.ini ]] && mode=0100755 ;;
    esac
    write_file "$f" "$dest" "$mode"
    n=$((n+1))
done < <(find "$OVERLAY" -type f)
echo "[assemble] wrote $n overlay files"

# 2. gui-installer (built in-tree; the live-only installer, not a herald package).
tmpelf="$(mktemp)"
"$STRIP" -o "$tmpelf" "$GUI_ELF" 2>/dev/null || cp "$GUI_ELF" "$tmpelf"
write_file "$tmpelf" "/apps/gui-installer/gui-installer" 0100755
rm -f "$tmpelf"
[ -f "$GUI_INI" ]  && write_file "$GUI_INI"  "/apps/gui-installer/app.ini"        0100644
[ -f "$GUI_CAPS" ] && write_file "$GUI_CAPS" "/etc/aegis/caps.d/gui-installer"     0100644

# 3. Pre-seed the herald db (components + the desktop meta).
if [ -f "$DB" ]; then
    ensure_dir "/var/lib/herald"
    write_file "$DB" "/var/lib/herald/db" 0100644
    echo "[assemble] seeded herald db ($(grep -c . "$DB") packages)"
fi

# Truncate trailing zero blocks (kernel re-expands from the superblock).
python3 - "$OUT" <<'PYEOF'
import os, sys
p = sys.argv[1]; d = open(p,'rb').read(); i = len(d)
while i > 0 and d[i-1] == 0: i -= 1
os.truncate(p, max(4096, ((i+4095)//4096)*4096))
PYEOF
echo "[assemble] desktop rootfs -> $OUT"
