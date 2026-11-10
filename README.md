# LoricaOS

A capability-secured operating system built on the [Aegis](https://github.com/LoricaOS/Aegis)
kernel. LoricaOS supplies the userland, GUI stack (Lumen compositor, Glyph toolkit,
Citadel shell, Bastion display manager), package manager (herald), installer, and
root filesystem; Aegis enforces the no-ambient-authority security model beneath it.

The Aegis kernel is consumed as a **versioned artifact**, not built here — LoricaOS
fetches `aegis-<KERNEL_VERSION>.elf` (see `tools/fetch-kernel.sh`). The OS version
(`VERSION`) and the kernel version (`KERNEL_VERSION`) move independently.

### Releasing against a new kernel

`tools/kernel.sha256` pins `(version, asset, sha256)` and `fetch-kernel.sh` fails
closed on any mismatch. So the order matters:

1. Tag and publish the Aegis release first (`aegis.elf`, `aegis-arm64.elf`, and,
   when the microVM bundle is cut, `aegis-microvm.elf`).
2. Bump `KERNEL_VERSION` (and `MICROVM_KERNEL_VERSION` if that artifact moves).
3. Record the hashes **of the published assets** — not of a local build. A CI
   rebuild is not byte-identical to a local one, so a locally-derived hash will
   make every downstream build fail closed:

   ```sh
   curl -fsSLO https://github.com/LoricaOS/Aegis/releases/download/v<KVER>/aegis.elf
   sha256sum aegis.elf        # paste into tools/kernel.sha256
   ```

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

### Verified local kernel (x86-64 only)

For an unpublished kernel, supply **both** an absolute artifact path and its
trusted expected SHA-256 (obtained independently, not recomputed to bless an
unknown artifact):

```sh
make server-iso LOCAL_KERNEL=/absolute/path/hardened.elf \
    LOCAL_KERNEL_SHA256='<trusted-64-digit-sha256>'
```

The build checks the copied bytes and ELF64/x86-64 architecture before atomically
replacing `build/aegis-stripped.elf`. This is the same canonical input used by the
ISO, ESP, and `rootfs.manifest`'s `/etc/aegis/update/aegis.elf` payload; there is no
second, unverified override. Missing/empty pairs, missing files, and mismatches
stop the build without release fallback, even if an older destination exists.
Every kernel build invocation revalidates and recopies the selected input, so
downstream images rebuild too.

Local provenance is recorded in `build/.kernel-local` (source and SHA-256), not
in release pins or `build/.kernel-version`. It does not claim a published kernel
version; `KERNEL_VERSION` remains the release default, not the local identity.
Omit both overrides (and unset them if exported in your environment) to restore
the checksum-locked release kernel on the next build, removing local provenance.
This only selects an artifact: it does not build, publish, or sign a kernel, or
establish that it boots. ARM/direct-script local support is not included.

Host-only regression check: `bash tools/test-local-kernel.sh`.

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
