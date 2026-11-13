# ARM64 development baseline and Apple Silicon boundary

QEMU `virt` exercises the existing ARM64 Aegis/LoricaOS stack on an x86-64
host using software emulation. It does not emulate Apple Silicon devices.
Passing here does not establish native Mac compatibility.

## Build

Use sibling checkouts of `LoricaOS`, `Aegis`, and `coreutils`. Required host
tools include an AArch64 GCC/binutils cross toolchain, make, a native C compiler,
Python with kconfiglib, curl, OpenSSL, xorriso, e2fsprogs, QEMU system ARM,
and AArch64 UEFI firmware (AAVMF). A non-root image build also needs fakeroot.

Generate the kernel configuration using the same public fallback as CI:

```sh
cd ../Aegis
mkdir -p build/generated
python3 - <<'PY'
import kconfiglib
k = kconfiglib.Kconfig("Kconfig", warn_to_stderr=False)
k.load_config("configs/full_defconfig")
k.write_config(".config")
k.write_config("build/auto.conf")
k.write_autoconf("build/generated/autoconf.h")
PY
make -f Makefile.arm64 iso build/arm64/aegis-arm64-test.iso dist -j8
make -f Makefile.arm64 test
cd ../LoricaOS
ISO="$PWD/build/loricaos-arm64-server.iso" fakeroot bash tools/build-arm64-server.sh
bash tools/run-arm64.sh
```

Set `AEGIS` and `COREUTILS` for other checkout locations. Set `MKE2FS` if
the image tool is not named `mke2fs` on PATH. fakeroot records uid/gid 0 inside
the image without changing host ownership; these uid values grant no kernel
authority. A privileged build can omit fakeroot.

The server script currently permits some optional tools to fail compilation;
inspect its log for `SKIP`/`failed` before treating an image as complete.
This build is a development image, not a production signed release.

The runner defaults to four Cortex-A72 CPUs, 2 GiB RAM, TCG, GICv3, a serial
console and VirtIO entropy. It deliberately has no network device. Override
`QEMU_CPUS`, `QEMU_MEMORY`, or `QEMU_EFI` as needed, or pass an ISO path as the
first argument. This ARM64 server image includes `/bin/configure`, so its first
boot asks you to create an account before presenting the login prompt. The
rootfs is a RAM disk: that account and other changes disappear on restart.

With Python's `pexpect` installed, `python3 tools/test-arm64-server.py` checks
first-boot setup, authenticated shell startup, and execution of `uname`, `cat`,
and `ls`. It leaves a serial transcript in `build/arm64-server/qemu-session.log`.

## Shared architecture, separate platforms

Keep one ARM64 architecture implementation. Architectural exception entry,
syscalls, context switching and most VM/process code remain shared. Introduce
platform backends when the Apple implementation supplies a second caller;
do not duplicate the ARM64 tree or advertise unsupported hardware as working.

Select the native platform from the root device-tree compatible strings and
validated boot information. CPU implementer identification alone is insufficient:
Apple-hosted virtual machines expose different devices from a native Mac.
Unknown platforms must stop with a diagnostic rather than touch QEMU MMIO
addresses. The current QEMU path still has those defaults; this is the intended
porting boundary, not an implemented Apple detector.

Apple-specific work belongs below that boundary:

* AICv3 and timer FIQ handling, including IRQ/FIQ masking and lock/preemption
  semantics. Existing ARM64 critical sections mask IRQ only.
* Apple UART/debug transport, clocks, resets, power domains and watchdogs.
* CPU startup and IPIs, with shared scheduler/TLB-shootdown contracts.
* Mailbox/RTKit, device DMA translation/isolation, ANS storage and USB.
* DCP/display, G15 GPU firmware protocols and userspace rendering integration.

Resolve CPU page granules and device mapping constraints before implementing
GPU support: Aegis currently uses a global 4096-byte page size, whereas the
inspected Aurora GPU build requires 16 KiB CPU pages. GPU UAT geometry is a
separate decision from CPU translation geometry.

Use pinned Aurora revisions as hardware references, with per-file provenance
and license review for any reused code. The Linux tree as a whole is GPLv2;
selected GPU sources offer an MIT alternative. Its Rust/Linux integration
cannot be imported wholesale into Aegis's C-only kernel.

Hardware acceptance remains separate: boot handoff and memory reservations,
real timer/interrupt delivery, DMA/cache coherency, firmware ownership,
storage integrity and graphics must be verified on the target M3 Pro.
