#!/usr/bin/env bash
# Host integration check for the signed package upgrade and FAT kernel swap.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
tmp="$(mktemp -d)"; server_pid=""
cleanup() {
    [[ -z "$server_pid" ]] || kill "$server_pid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT

# Exercise the exact FAT16 parser/writer against an installed-disk-shaped image.
dd if=/dev/zero of="$tmp/disk.img" bs=1M count=9 status=none
dd if=build/esp-server.img of="$tmp/disk.img" bs=1M seek=1 conv=notrunc status=none
make -C user/bin/lorica-update selftest ESP_IMAGE="$tmp/disk.img"

version="$(cat VERSION)"
port="${UPDATE_TEST_PORT:-18081}"
pkg="build/pkgs/loricaos-release_${version}_x86_64.hpkg"
repo="$tmp/repo"; root="$tmp/root"
mkdir -p "$repo/dists/stable/main/binary-x86_64" "$repo/pool" \
         "$root/bin" "$root/lib" "$root/etc/herald" "$root/var/lib/herald"
cp "$pkg" "$repo/pool/"
pkg_sha="$(sha256sum "$pkg" | cut -d' ' -f1)"
printf 'Package: loricaos-release\nVersion: %s\nArchitecture: x86_64\nFilename: pool/%s\nSHA256: %s\nDisplay-Name: LoricaOS System Release\n\n' \
    "$version" "$(basename "$pkg")" "$pkg_sha" \
    > "$repo/dists/stable/main/binary-x86_64/Packages"
packages="$repo/dists/stable/main/binary-x86_64/Packages"
packages_sha="$(sha256sum "$packages" | cut -d' ' -f1)"
printf 'Suite: stable\nSHA256:\n %s %s main/binary-x86_64/Packages\n' \
    "$packages_sha" "$(stat -c %s "$packages")" > "$repo/dists/stable/Release"
key="${HERALD_KEY:-tools/herald-keys/herald.key}"
[[ -f "$key" ]] || key=build/herald-keys/herald-dev.key
openssl dgst -sha256 -sign "$key" -out "$repo/dists/stable/Release.sig" \
    "$repo/dists/stable/Release"

cp user/bin/herald/herald.elf "$root/bin/herald"
cp build/curl/curl "$root/bin/curl"
cp build/musl-dynamic/usr/lib/libc.so "$root/lib/ld-musl-x86_64.so.1"
printf 'http://127.0.0.1:%s stable main\n' "$port" > "$root/etc/herald/sources.list"
printf 'loricaos-release\t0.0.0\t\told\n' > "$root/var/lib/herald/db"
python3 -m http.server "$port" -d "$repo" >"$tmp/http.log" 2>&1 & server_pid=$!
for _ in 1 2 3 4 5; do
    curl -sf "http://127.0.0.1:$port/dists/stable/Release" >/dev/null && break
    sleep .1
done
chroot "$root" /bin/herald sync
chroot "$root" /bin/herald upgrade
grep -Fq pending-kernel "$root/var/lib/herald/transaction/state"
grep -Fq etc/lorica-release "$root/var/lib/herald/owners/loricaos-release.list"
chroot "$root" /bin/herald recover rollback
grep -Fq $'loricaos-release\t0.0.0' "$root/var/lib/herald/db"
test ! -e "$root/etc/lorica-release"

chroot "$root" /bin/herald upgrade
chroot "$root" /bin/herald recover complete
grep -Fq $'loricaos-release\t'"$version" "$root/var/lib/herald/db"
cmp build/aegis-stripped.elf "$root/etc/aegis/update/aegis.elf"
grep -Fq "VERSION=$version" "$root/etc/lorica-release"
test ! -e "$root/var/lib/herald/transaction"
test ! -e "$root/var/lib/lorica-update/pending"
echo "system update integration: PASS"
