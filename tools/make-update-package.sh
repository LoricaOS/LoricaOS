#!/usr/bin/env bash
# Build the signed system-release package consumed by `lorica-update`.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"

version="$(cat VERSION)"
kernel_version="$(cat KERNEL_VERSION)"
key="${HERALD_KEY:-tools/herald-keys/herald.key}"
if [[ ! -f "$key" ]]; then
    key=build/herald-keys/herald-dev.key
    [[ -f "$key" ]] || bash tools/herald-keygen.sh >/dev/null
fi
stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/apps" "$stage/etc/aegis/update" "$stage/var/lib/lorica-update"
printf 'id=loricaos-release\nname=LoricaOS System Release\nversion=%s\nclass=system\n' \
    "$version" > "$stage/manifest"
printf '%s\n' "$version" > "$stage/etc/lorica-version"
printf 'NAME=LoricaOS\nVERSION=%s\nKERNEL_VERSION=%s\n' \
    "$version" "$kernel_version" > "$stage/etc/lorica-release"
printf '%s\n' "$kernel_version" > "$stage/etc/aegis/update/kernel-version"
cp build/aegis-stripped.elf "$stage/etc/aegis/update/aegis.elf"
printf 'version=%s\nkernel=%s\n' "$version" "$kernel_version" \
    > "$stage/var/lib/lorica-update/pending"
[[ $(stat -c %s "$stage/etc/aegis/update/aegis.elf") -le 3145728 ]] || {
    echo "make-update-package: kernel exceeds 3 MiB ESP slot" >&2; exit 1;
}
mkdir -p build/pkgs
HERALD_KEY="$key" bash tools/herald-pack.sh "$stage" \
    "build/pkgs/loricaos-release_${version}_x86_64.hpkg"
