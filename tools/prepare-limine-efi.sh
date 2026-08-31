#!/bin/sh
set -eu
src="${1:?source EFI}" conf="${2:?limine.conf}" out="${3:?output EFI}"
cp "$src" "$out"
sum="$(tools/blake2b.sh "$conf")"
build/limine enroll-config --quiet "$out" "$sum"

if [ -n "${SECURE_BOOT_KEY:-}" ] || [ -n "${SECURE_BOOT_CERT:-}" ]; then
    [ -n "${SECURE_BOOT_KEY:-}" ] && [ -n "${SECURE_BOOT_CERT:-}" ] || {
        echo "secure boot needs both SECURE_BOOT_KEY and SECURE_BOOT_CERT" >&2; exit 1;
    }
    command -v sbsign >/dev/null 2>&1 || {
        echo "sbsign is required for a secure-boot build" >&2; exit 1;
    }
    sbsign --key "$SECURE_BOOT_KEY" --cert "$SECURE_BOOT_CERT" \
        --output "$out.signed" "$out" >/dev/null
    mv "$out.signed" "$out"
fi
