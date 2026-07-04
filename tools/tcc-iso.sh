#!/usr/bin/env bash
# tcc-iso.sh — build a server ISO that, at boot, runs tcc ON AEGIS to compile a
# freestanding C program and then executes the result. The on-device proof that
# a C compiler runs on Aegis and emits working Aegis binaries (self-hosting).
#
# Injects the tcc toolchain (build/tcc-install) + a test program + a boot-run
# vigil service into a copy of the server rootfs, then wraps a text-boot ISO
# gated on the `tcctest` cmdline token. Needs: make server-iso once, and
# tools/build-tcc.sh (build/tcc-install). Run from the repo root on CT117.
set -eu
REPO="$(git rev-parse --show-toplevel)"; cd "$REPO"

TCCROOT="$REPO/build/tcc-install/usr/local"
SERVER=build/rootfs-server.img
OUT=build/rootfs-tcc.img
ISO=build/loricaos-tcc.iso
DEBUGFS="${DEBUGFS:-debugfs}"
FS_KIB=131072   # 128 MiB — room for tcc + its runtime on top of the server fs

[ -f "$SERVER" ]        || { echo "tcc-iso: need $SERVER (make server-iso)"; exit 1; }
[ -x "$TCCROOT/bin/tcc" ] || { echo "tcc-iso: need $TCCROOT/bin/tcc (bash tools/build-tcc.sh)"; exit 1; }

cp "$SERVER" "$OUT"
truncate -s "${FS_KIB}K" "$OUT"
e2fsck -fy "$OUT" >/dev/null 2>&1 || true
resize2fs "$OUT" "${FS_KIB}K" >/dev/null 2>&1 || { echo "tcc-iso: resize2fs failed"; exit 1; }

declare -A MKDIRD
ensure_dir() {
    local d="$1"; [ -n "${MKDIRD[$d]:-}" ] && return
    local p; p="$(dirname "$d")"
    [ "$p" != "/" ] && [ "$p" != "." ] && ensure_dir "$p"
    $DEBUGFS -w -R "mkdir $d" "$OUT" >/dev/null 2>&1 || true
    MKDIRD["$d"]=1
}
put() { # <src> <dest-abs> <octal-mode>
    ensure_dir "$(dirname "$2")"
    $DEBUGFS -w -R "rm $2" "$OUT" >/dev/null 2>&1 || true
    $DEBUGFS -w -R "write $1 $2" "$OUT" >/dev/null 2>&1
    $DEBUGFS -w -R "set_inode_field $2 mode $3" "$OUT" >/dev/null 2>&1
}

# 1. tcc binary + its runtime tree (TCCDIR = /usr/local/lib/tcc).
put "$TCCROOT/bin/tcc" /bin/tcc 0100755
( cd "$TCCROOT/lib" && find tcc -type f ) | while read -r rel; do
    put "$TCCROOT/lib/$rel" "/usr/local/lib/$rel" 0100644
done

# 2. A freestanding test program (no headers, raw syscalls) + a build script.
#    Aegis execve does NOT honor a #! shebang, so the service runs the script
#    through the shell (`stsh <script>`) rather than exec'ing it directly.
cat > /tmp/free.c <<'CEOF'
static long sys_write(int fd,const void*b,long n){long r;__asm__ volatile("syscall":"=a"(r):"a"(1),"D"(fd),"S"(b),"d"(n):"rcx","r11","memory");return r;}
static void sys_exit(int c){__asm__ volatile("syscall"::"a"(60),"D"(c));for(;;){}}
void _start(void){
    int s=0; for(int i=1;i<=10;i++) s+=i;               /* real codegen: loop + branch */
    const char*m=(s==55)?"TCC_ON_AEGIS_OK sum=55\n":"TCC_ON_AEGIS_FAIL\n";
    int n=0; while(m[n])n++;
    sys_write(2,m,n);                                    /* fd 2: vigil nulls stdout */
    sys_exit(0);
}
CEOF
cat > /tmp/build.sh <<'SEOF'
echo "[TCCTEST] compiling on Aegis with tcc" >&2
tcc -B/usr/local/lib/tcc -nostdlib -static /tcctest/free.c -o /tcctest/out
if [ -f /tcctest/out ]; then
  echo "[TCCTEST] compiled; running the tcc-built binary:" >&2
  /tcctest/out
else
  echo "[TCCTEST] compile FAILED" >&2
fi
SEOF
put /tmp/free.c   /tcctest/free.c   0100644
put /tmp/build.sh /tcctest/build.sh 0100644

# 3. Boot-run vigil service, gated on the `tcctest` cmdline token. A non-slash
#    run string is executed via `sh -c`, so `stsh <script>` runs the test.
sd=/etc/vigil/services/tcctest
printf 'tcctest'                 > /tmp/t.cmdline
printf 'root'                    > /tmp/t.user
printf 'stsh /tcctest/build.sh'  > /tmp/t.run
printf 'oneshot'                 > /tmp/t.policy
put /tmp/t.cmdline "$sd/cmdline" 0100644
put /tmp/t.user    "$sd/user"    0100644
put /tmp/t.run     "$sd/run"     0100644
put /tmp/t.policy  "$sd/policy"  0100644

# Truncate trailing zero blocks (kernel re-expands from the superblock).
python3 - "$OUT" <<'PYEOF'
import os,sys
p=sys.argv[1]; d=open(p,'rb').read(); i=len(d)
while i>0 and d[i-1]==0: i-=1
os.truncate(p, max(4096, ((i+4095)//4096)*4096))
PYEOF
echo "[tcc-iso] rootfs -> $OUT ($(du -h "$OUT"|cut -f1))"

# 4. Build the ISO (text boot + the tcctest token; reuse the ffsmoke template).
D=build/tcc-isodir
rm -rf "$D"; mkdir -p "$D/boot/limine" "$D/EFI/BOOT"
cp build/aegis-stripped.elf "$D/boot/aegis.elf"
cp "$OUT" "$D/boot/rootfs.img"
cp build/esp-server.img "$D/boot/esp.img"
sh tools/gen-limine-conf.sh ffsmoke | sed 's/ffsmoke/tcctest/g' > "$D/boot/limine/limine.conf"
cp tools/limine/limine-bios.sys tools/limine/limine-bios-cd.bin tools/limine/limine-uefi-cd.bin "$D/boot/limine/"
cp tools/limine/BOOTX64.EFI tools/limine/BOOTIA32.EFI "$D/EFI/BOOT/"
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label "$D" -o "$ISO" >/dev/null 2>&1
# El Torito CD boot works without the hybrid-MBR patch; keep it best-effort.
build/limine/limine bios-install "$ISO" >/dev/null 2>&1 || true
echo "[tcc-iso] $ISO ($(du -h "$ISO"|cut -f1))"
