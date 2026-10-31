# LoricaOS

A capability-secured operating system built on the [Aegis](https://github.com/LoricaOS/Aegis)
kernel. LoricaOS supplies the userland, GUI stack (Lumen compositor, Glyph toolkit,
Citadel shell, Bastion display manager), package manager (herald), installer, and
root filesystem; Aegis enforces the no-ambient-authority security model beneath it.

The Aegis kernel is consumed as a **versioned artifact**, not built here — LoricaOS
fetches `aegis-<KERNEL_VERSION>.elf` (see `tools/fetch-kernel.sh`). The OS version
(`VERSION`) and the kernel version (`KERNEL_VERSION`) move independently.

## Build

Requires `musl-gcc`, `nasm`-free userland toolchain, `xorriso`, `mtools`,
`dosfstools`, the vendored Limine in `tools/`, and `curl` (to fetch the kernel).

```
make           # build/loricaos-desktop.iso  (live, bootable)
make test      # boot the ISO headless; expect "[BASTION] greeter ready"
make version   # LoricaOS <ver> (kernel <ver>)
make clean
```

To build offline, drop the kernel image at `vendor/aegis-<KERNEL_VERSION>.elf`.

## Live session credentials

| | |
|---|---|
| login | `live` |
| password | `live` |
| admin password | `administrator` |

The admin password unlocks **capability elevation** (Settings, the installer,
`aegisctl`) in the live session. There is no root to log in as — `uid=0` is
cosmetic under Aegis and grants no authority by itself. On installed systems
the installer sets your own user + admin passwords (the admin password may be
separate, or default to your account password), and `adminpw` changes the
admin credential later; the live defaults above apply only to the ISO.

## Herald state

Herald keeps its installed database, ownership lists, and recovery transaction
under `/etc/aegis/herald`, where writes require INSTALL authority. Downloads and
reverified repository lists remain under `/var/lib/herald`. The system-update
marker lives at `/etc/aegis/update/pending`.

Legacy `/var/lib/herald/{db,owners,transaction}` state is never imported or
recovered: an unelevated uid-0 process could have forged it. Transition using a
rebuilt image with its trusted package database; do not copy legacy metadata
into the protected directory or resume a legacy transaction with the new client.
System-release packages using the old update marker must also be rebuilt.
