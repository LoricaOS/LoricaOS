#!/usr/bin/env bash
# Headless ARM64 development baseline; QEMU virt does not emulate a native Mac.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-$REPO/build/loricaos-arm64-server.iso}"
FW="${QEMU_EFI:-}"
if [ -z "$FW" ]; then
    for f in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
        if [ -f "$f" ]; then FW="$f"; break; fi
    done
fi
[ -f "$ISO" ] || { echo "Missing ISO: $ISO" >&2; exit 1; }
[ -f "$FW" ] || { echo "Set QEMU_EFI to an AAVMF/QEMU_EFI firmware image" >&2; exit 1; }
exec qemu-system-aarch64 \
    -machine virt,gic-version=3 -accel tcg \
    -cpu cortex-a72 -smp "${QEMU_CPUS:-4}" -m "${QEMU_MEMORY:-2048M}" \
    -bios "$FW" -cdrom "$ISO" \
    -object rng-random,id=rng0,filename=/dev/urandom \
    -global virtio-mmio.force-legacy=false -device virtio-rng-device,rng=rng0 \
    -display none -nodefaults -monitor none -serial stdio -no-reboot
