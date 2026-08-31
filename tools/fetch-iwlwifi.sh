#!/bin/sh
set -eu

out=${1:-vendor/iwlwifi-cc-a0-59.ucode}
url='https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/iwlwifi-cc-a0-59.ucode?h=20210315'
want='e18cbc975bdde4f5bc14796f23ca19e712f05b4c365c4be0a19f1a2b3b768af8'
tmp="${out}.tmp"
mkdir -p "$(dirname "$out")"
curl -fL "$url" -o "$tmp"
got=$(sha256sum "$tmp" | awk '{print $1}')
if [ "$got" != "$want" ]; then
    rm -f "$tmp"
    echo "iwlwifi firmware checksum mismatch" >&2
    exit 1
fi
mv "$tmp" "$out"
