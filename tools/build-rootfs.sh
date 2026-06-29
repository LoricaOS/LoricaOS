#!/bin/bash
#
# build-rootfs.sh — Build the Aegis root filesystem ext2 image
#
# Reads rootfs.manifest for binary mappings and copies the rootfs/ skeleton.
# Single source of truth: add a binary to rootfs.manifest, done.
#
set -euo pipefail

ROOTFS_IMG="${1:?Usage: build-rootfs.sh <output.img> [wallpaper.raw] [logo.raw] [claude.raw]}"
WALLPAPER_RAW="${2:-}"
LOGO_RAW="${3:-}"
CLAUDE_RAW="${4:-}"

# Profile selects which manifests + skeleton trees compose the image:
#   desktop (default): base + graphical (compositor, toolkit, GUI apps, fonts)
#   server:            base only — no graphical stack, no fonts/wallpaper
PROFILE="${AEGIS_PROFILE:-desktop}"
if [[ "$PROFILE" == server ]]; then
    SKELETONS=("rootfs")
    MANIFESTS=("rootfs.manifest")
    WANT_ASSETS=0
else
    SKELETONS=("rootfs" "rootfs-desktop")
    MANIFESTS=("rootfs.manifest" "rootfs.desktop.manifest")
    WANT_ASSETS=1
fi
echo "[rootfs] profile: $PROFILE  (skeletons: ${SKELETONS[*]})"
# 44 MiB LOGICAL rootfs ext2 (512-byte sectors), but the image is TRUNCATED to
# its last non-zero block before shipping (see the truncate step at the end), so
# the ISO carries only the ~14 MiB of real content — NOT the ~30 MiB of trailing
# zero free blocks. The kernel zero-extends the ramdisk back to this full 44 MiB
# in RAM at boot (ramdisk.c reads s_blocks_count from the ext2 superblock), so:
#   - the live system's free space costs RAM, not ISO bytes;
#   - the installer still reads a full 44 MiB image off ramdisk0 -> installed
#     root keeps its ~27 MiB free space (no regression);
#   - the ISO shrinks by ~30 MiB.
# 4 KiB blocks are REQUIRED: with 1 KiB blocks a 44 MiB fs spans ~6 block groups
# and sparse_super scatters backup superblocks into the tail (~24/40 MiB), so the
# tail wouldn't be zero and couldn't be truncated. 4 KiB blocks -> one block
# group (cap 128 MiB) -> no tail backups -> trailing bytes are all zero.
# P1_SECTORS (LOGICAL size) must stay in lockstep with the Makefile.
P1_SECTORS=90112
DEBUGFS="/sbin/debugfs"

# ── Create empty ext2 image ──────────────────────────────────────────────────
rm -f "$ROOTFS_IMG"
dd if=/dev/zero of="$ROOTFS_IMG" bs=512 count=$P1_SECTORS 2>/dev/null
/sbin/mke2fs -t ext2 -F -b 4096 -L aegis-root "$ROOTFS_IMG" >/dev/null 2>&1

# ── Helper: batch debugfs commands ───────────────────────────────────────────
debugfs_run() {
    local out
    out="$($DEBUGFS -w "$ROOTFS_IMG" <<< "$1" 2>&1)"
    if echo "$out" | grep -qiE "could not allocate|no space left"; then
        echo "[rootfs] FATAL: ext2 image out of space writing: $1" >&2
        echo "[rootfs] grow P1_SECTORS (Makefile + tools/build-rootfs.sh)" >&2
        exit 1
    fi
}

# Track which directories we've already created
declare -A CREATED_DIRS

ensure_dir() {
    local dir="$1"
    if [[ -n "${CREATED_DIRS[$dir]:-}" ]]; then
        return
    fi
    # Ensure parent exists first
    local parent
    parent="$(dirname "$dir")"
    if [[ "$parent" != "/" && "$parent" != "." ]]; then
        ensure_dir "$parent"
    fi
    debugfs_run "mkdir $dir"
    CREATED_DIRS["$dir"]=1
}

# ── Copy skeleton directory tree ─────────────────────────────────────────────
# Walk the rootfs/ skeleton and replicate its structure + files into the image.
echo "[rootfs] Copying skeleton..."

# First pass: create all directories (across every skeleton tree in the profile)
for SK in "${SKELETONS[@]}"; do
    while IFS= read -r -d '' dir; do
        rel="${dir#$SK}"
        [[ -z "$rel" ]] && continue
        ensure_dir "$rel"
    done < <(find "$SK" -type d -print0 | sort -z)
done

# Second pass: copy all files. /etc/motd carries the __AEGIS_VERSION__ placeholder
# (single source of truth: the AEGIS_VERSION exported by the Makefile from git) —
# substitute it at pack time so the login banner always matches the real version.
MOTD_TMP=""
for SK in "${SKELETONS[@]}"; do
    while IFS= read -r -d '' file; do
        rel="${file#$SK}"
        if [[ "$rel" == "/etc/motd" ]]; then
            MOTD_TMP="$(mktemp)"
            sed "s/__AEGIS_VERSION__/${AEGIS_VERSION:-untracked}/g" "$file" > "$MOTD_TMP"
            debugfs_run "write $MOTD_TMP $rel"
        else
            debugfs_run "write $file $rel"
        fi
    done < <(find "$SK" -type f -print0 | sort -z)
done
[[ -n "$MOTD_TMP" ]] && rm -f "$MOTD_TMP"

# ── Process manifest: copy binaries ─────────────────────────────────────────
echo "[rootfs] Installing binaries from manifest..."

# Strip ELF binaries at packaging time. Build-tree originals stay unstripped
# (debugging / incremental rebuilds untouched); a stripped temp copy is what
# goes into the image. Non-ELF files (e.g. the curl stub shell script) make
# strip fail — fall back to writing the original.
STRIP_TOOL="$(command -v x86_64-linux-gnu-strip || command -v strip || true)"
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT

CMDS=""
CHMOD_CMDS=""

while IFS= read -r line; do
    # Strip comments and blank lines
    line="${line%%#*}"
    line="$(echo "$line" | xargs)"
    [[ -z "$line" ]] && continue

    src=$(echo "$line" | awk '{print $1}')
    dest=$(echo "$line" | awk '{print $2}')

    if [[ ! -f "$src" ]]; then
        echo "[rootfs] WARNING: $src not found, skipping"
        continue
    fi

    # Ensure parent directory exists
    dest_dir="$(dirname "$dest")"
    ensure_dir "$dest_dir"

    # Strip executables/libraries into the staging dir before writing
    write_src="$src"
    if [[ -n "$STRIP_TOOL" && ( "$dest" == /bin/* || "$dest" == /lib/* || "$dest" == /apps/* ) ]]; then
        stripped="$STAGE_DIR/$(basename "$dest")"
        if "$STRIP_TOOL" -o "$stripped" "$src" 2>/dev/null; then
            write_src="$stripped"
        fi
    fi

    # Queue the write
    debugfs_run "write $write_src $dest"

    # Auto-chmod executables in /bin, /lib and /apps bundles
    if [[ "$dest" == /bin/* || "$dest" == /lib/* || "$dest" == /apps/* ]]; then
        CHMOD_CMDS+="set_inode_field $dest mode 0100755\n"
    fi
done < <(cat "${MANIFESTS[@]}")

# /lib/libc.so is a symlink to the dynamic linker — one copy on disk instead
# of two. PT_INTERP in every dynamic binary is /lib/ld-musl-x86_64.so.1 (the
# real file), so the kernel's exec hot path never depends on symlink
# resolution; the kernel's ext2 follows absolute symlinks (Phase 41) for
# anything that does open /lib/libc.so. Target is < 60 chars → debugfs
# creates a fast symlink, which ext2_read_symlink_target handles.
debugfs_run "symlink /lib/libc.so /lib/ld-musl-x86_64.so.1"

# Defense-in-depth: the admin-elevation credential hash is owner-only (0600).
# The real protection is the kernel CAP_KIND_AUTH read-gate (vfs.c / sys_file.c,
# inode recorded as s_admin_ino in ext2.c), which survives a bad file mode; this
# 0600 is belt-and-suspenders so a non-AUTH reader is denied even before the
# gate. Only added if the file was actually written into the image.
if [[ -e rootfs/etc/aegis/admin ]]; then
    CHMOD_CMDS+="set_inode_field /etc/aegis/admin mode 0100600\n"
fi

# Batch chmod
if [[ -n "$CHMOD_CMDS" ]]; then
    echo "[rootfs] Setting permissions..."
    printf "$CHMOD_CMDS" | $DEBUGFS -w "$ROOTFS_IMG" >/dev/null 2>&1
fi

# ── Optional graphical assets (DESKTOP profile only) ─────────────────────────
# Logo/wallpaper raws and TTF fonts exist only for the GUI greeter + Glyph text
# rendering. A server image has no compositor and uses the kernel's built-in
# console font, so it ships none of these.
ensure_dir "/usr"
ensure_dir "/usr/share"

if [[ "$WANT_ASSETS" == 1 ]]; then
    for raw_file in "$WALLPAPER_RAW" "$LOGO_RAW" "$CLAUDE_RAW"; do
        if [[ -n "$raw_file" && -f "$raw_file" && -s "$raw_file" ]]; then
            name="$(basename "$raw_file")"
            debugfs_run "write $raw_file /usr/share/$name"
        fi
    done

    if [[ -d assets ]]; then
        ensure_dir "/usr/share/fonts"
        for ttf in assets/*.ttf; do
            [[ -f "$ttf" ]] || continue
            name="$(basename "$ttf")"
            debugfs_run "write $ttf /usr/share/fonts/$name"
        done
    fi
fi

# NOTE: no kernel copy in the rootfs. The installed system boots the kernel
# from the FAT ESP (esp.img carries boot/aegis.elf; see the Makefile $(ESP_IMG)
# rule and gen-limine-conf.sh installed mode) — a rootfs /boot/aegis.elf would
# be dead weight inside this LOGICAL 44 MiB image.

# ── Truncate trailing zero blocks ────────────────────────────────────────────
# The fs is one block group (4 KiB blocks) so it has no backup superblocks in
# the tail; everything past the last used block is zero. Drop those zeros so the
# ISO carries ~14 MiB instead of 44 MiB. The kernel (kernel/drivers/ramdisk.c)
# reads s_blocks_count from the superblock and zero-extends the ramdisk back to
# the full 44 MiB in RAM — byte-identical to the untruncated image. The 4 KiB
# round-up keeps a whole final block; the superblock (at byte 1024) is never in
# the tail, so the kernel always recovers the true size.
python3 - "$ROOTFS_IMG" <<'PYEOF'
import os, sys
path = sys.argv[1]
data = open(path, 'rb').read()
i = len(data)
while i > 0 and data[i-1] == 0:
    i -= 1
size = max(4096, ((i + 4095) // 4096) * 4096)   # block-align, keep >= 1 block
os.truncate(path, size)
print(f"[rootfs] truncated {len(data)//1024} KiB -> {size//1024} KiB "
      f"(kernel zero-extends back to {len(data)//1024} KiB in RAM)")
PYEOF

echo "[rootfs] Done: $ROOTFS_IMG"
