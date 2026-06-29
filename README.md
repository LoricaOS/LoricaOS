# AspisOS

A capability-secured operating system built on the [Aegis](https://github.com/AspisOS/Aegis)
kernel. AspisOS supplies the userland, GUI stack (Lumen compositor, Glyph toolkit,
Citadel shell, Bastion display manager), package manager (herald), installer, and
root filesystem; Aegis enforces the no-ambient-authority security model beneath it.

The Aegis kernel is consumed as a **versioned artifact**, not built here — AspisOS
fetches `aegis-<KERNEL_VERSION>.elf` (see `tools/fetch-kernel.sh`). The OS version
(`VERSION`) and the kernel version (`KERNEL_VERSION`) move independently.

## Build

Requires `musl-gcc`, `nasm`-free userland toolchain, `xorriso`, `mtools`,
`dosfstools`, the vendored Limine in `tools/`, and `curl` (to fetch the kernel).

```
make           # build/aspisos-desktop.iso  (live, bootable)
make test      # boot the ISO headless; expect "[BASTION] greeter ready"
make version   # AspisOS <ver> (kernel <ver>)
make clean
```

To build offline, drop the kernel image at `vendor/aegis-<KERNEL_VERSION>.elf`.
