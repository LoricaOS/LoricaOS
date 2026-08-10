#!/usr/bin/env bash
# build-microvm-image.sh — assemble a Firecracker / cloud-hypervisor / qemu-microvm
# image of LoricaOS server.
#
# The pieces already exist; this just pairs them and packages a runnable bundle:
#   - the Aegis "microvm" kernel (PVH direct boot, no ACPI/PCI, virtio-mmio) —
#     fetched from the Aegis release, same mechanism as the ISO kernel.
#   - the server root filesystem — build/rootfs-server.img is ALREADY a raw ext2
#     (tools/build-rootfs.sh mke2fs's it; the ISO just wraps it in Limine). The
#     microVM boots it straight off a virtio-blk disk, no bootloader.
#
# Output: build/loricaos-microvm-<VERSION>.tar.gz containing
#   aegis-microvm.elf  rootfs.ext2  vm.json  README.md
#
# Run on a build host (the rootfs build needs the same tools as `make server-iso`).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

VERSION="$(cat VERSION)"
MVK_VERSION="$(cat MICROVM_KERNEL_VERSION)"
OUT="build/microvm"
BUNDLE="build/loricaos-microvm-${VERSION}.tar.gz"

echo "[microvm] LoricaOS $VERSION on Aegis microvm kernel v$MVK_VERSION"
rm -rf "$OUT"; mkdir -p "$OUT"

# 1. microVM kernel (PVH, no ACPI/PCI, virtio-mmio) — from the Aegis release.
bash tools/fetch-kernel.sh "$MVK_VERSION" "$OUT/aegis-microvm.elf" microvm

# 2. server rootfs — reuse the raw ext2 the ISO build already produces.
echo "[microvm] building server rootfs (raw ext2)"
make build/rootfs-server.img >/dev/null
cp build/rootfs-server.img "$OUT/rootfs.ext2"
ROOTMB=$(( ( $(stat -c%s "$OUT/rootfs.ext2") + 1048575 ) / 1048576 ))
echo "[microvm] rootfs.ext2: ${ROOTMB} MiB"

# 3. Firecracker config. Paths are relative — run firecracker from the extracted
#    dir. Block + serial only; networking is opt-in (see README) so the bundle
#    boots with zero host setup (LoricaOS's dhcp just exits when there's no NIC).
cat > "$OUT/vm.json" <<JSON
{
  "boot-source": {
    "kernel_image_path": "aegis-microvm.elf",
    "boot_args": "boot=text console=ttyS0"
  },
  "drives": [
    {
      "drive_id": "root",
      "path_on_host": "rootfs.ext2",
      "is_root_device": true,
      "is_read_only": false
    }
  ],
  "machine-config": { "vcpu_count": 1, "mem_size_mib": 512 }
}
JSON

# 4. README with the run recipes.
cat > "$OUT/README.md" <<'MD'
# LoricaOS microVM image

A LoricaOS **server** image that boots straight into a microVM — no bootloader,
no BIOS/UEFI, no PCI. The Aegis "microvm" kernel does PVH direct boot, discovers
its disk + NIC over **virtio-mmio**, and mounts the ext2 rootfs off a virtio-blk
device. Boots to a console `login:` in well under a second.

## Contents

| File               | What it is                                              |
|--------------------|---------------------------------------------------------|
| `aegis-microvm.elf`| Aegis microvm kernel (PVH, no ACPI/PCI, virtio-mmio)    |
| `rootfs.ext2`      | LoricaOS server root filesystem (writable ext2)         |
| `vm.json`          | Firecracker config (block + serial; run from this dir)  |

## Firecracker

Needs `/dev/kvm`. From the extracted directory:

```
firecracker --no-api --config-file vm.json
```

The guest serial is on the firecracker process stdio. `poweroff` inside the
guest cleanly stops the microVM (the no-ACPI kernel triggers a triple fault the
VMM honors); the orchestrator can also stop it via the Firecracker API.

### Networking (optional)

The default `vm.json` has no NIC (dhcp exits cleanly without one). To add one,
create a host tap and a `network-interfaces` entry:

```
sudo ip tuntap add tap0 mode tap && sudo ip addr add 172.16.0.1/24 dev tap0 && sudo ip link set tap0 up
```
then add to `vm.json`:
```
  "network-interfaces": [
    { "iface_id": "eth0", "host_dev_name": "tap0", "guest_mac": "AA:FC:00:00:00:01" }
  ]
```

## cloud-hypervisor

```
cloud-hypervisor --kernel aegis-microvm.elf --cmdline "boot=text console=ttyS0" \
  --disk path=rootfs.ext2 --serial tty --console off --cpus boot=1 --memory size=512M
```

## QEMU microvm (no KVM needed — software emulation)

```
qemu-system-x86_64 -machine microvm -global virtio-mmio.force-legacy=false \
  -kernel aegis-microvm.elf -append "boot=text console=ttyS0" \
  -drive id=root,file=rootfs.ext2,format=raw,if=none \
  -device virtio-blk-device,drive=root \
  -nodefaults -serial stdio -no-reboot -m 512M
```
MD

# 5. Package.
tar -C "$OUT" -czf "$BUNDLE" aegis-microvm.elf rootfs.ext2 vm.json README.md
echo "[microvm] -> $BUNDLE ($(( $(stat -c%s "$BUNDLE") / 1048576 )) MiB)"
