#!/usr/bin/env python3
"""
gen-arm64-image.py — build a Linux arm64 Image from an Aegis ELF.

The Aegis ARM64 boot stub (kernel/arch/arm64/boot.S) embeds the 64-byte
Linux arm64 Image header directly at the top of the `.text.boot`
section. `aarch64-elf-objcopy -O binary` on the ELF therefore already
produces a flat binary whose first 64 bytes are a valid boot header —
we just stamp the `image_size` field (offset 0x10) with the actual
flat-binary size rounded up to 2MB, and sanity-check the magic and
code0 fields so a mis-assembled header is caught immediately.

Pi firmware (start4.elf / start5.elf) and QEMU `-kernel` both honour
this format, so the single `build/aegis.img` artefact boots on both.

Reference: https://www.kernel.org/doc/html/latest/arch/arm64/booting.html
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

HEADER_SIZE = 64

# Expected header fields (must match kernel/arch/arm64/boot.S).
EXPECTED_CODE0 = 0x14000010   # `b .+64`
EXPECTED_FLAGS = 0x02         # 4KB page, little-endian
EXPECTED_MAGIC = 0x644D5241   # "ARM\x64"


def die(msg: str) -> None:
    sys.stderr.write(f"gen-arm64-image: {msg}\n")
    sys.exit(1)


def flatten_elf(elf: Path, out_bin: Path) -> None:
    try:
        subprocess.run(
            ["aarch64-elf-objcopy", "-O", "binary", str(elf), str(out_bin)],
            check=True,
        )
    except FileNotFoundError:
        die("aarch64-elf-objcopy not found — install the aarch64-elf toolchain")
    except subprocess.CalledProcessError as e:
        die(f"objcopy failed (rc={e.returncode})")


def round_up(n: int, align: int) -> int:
    return (n + align - 1) & ~(align - 1)


def verify_and_stamp_header(flat: bytearray) -> None:
    if len(flat) < HEADER_SIZE:
        die(f"flat binary too small ({len(flat)} bytes)")

    code0, code1, text_offset, image_size, flags = struct.unpack_from(
        "<IIQQQ", flat, 0
    )
    magic, _res5 = struct.unpack_from("<II", flat, 0x38)

    if code0 != EXPECTED_CODE0:
        die(
            f"code0 mismatch: got 0x{code0:08x}, "
            f"expected 0x{EXPECTED_CODE0:08x} — boot.S header out of date?"
        )
    if magic != EXPECTED_MAGIC:
        die(
            f"magic mismatch at offset 0x38: got 0x{magic:08x}, "
            f"expected 0x{EXPECTED_MAGIC:08x} — is .text.boot header intact?"
        )
    if flags != EXPECTED_FLAGS:
        die(
            f"flags mismatch: got 0x{flags:016x}, expected 0x{EXPECTED_FLAGS:016x}"
        )

    # Stamp image_size (offset 0x10) — 2MB round-up of the flat size.
    eff_size = round_up(len(flat), 2 * 1024 * 1024)
    struct.pack_into("<Q", flat, 0x10, eff_size)

    # code1 stays 0 (already), text_offset stays 0 (already).
    _ = code1
    _ = text_offset


def main() -> int:
    ap = argparse.ArgumentParser(description="Build an arm64 Linux Image from Aegis ELF")
    ap.add_argument("elf", type=Path, help="input ELF (e.g. build/aegis-arm64.elf)")
    ap.add_argument("img", type=Path, help="output Image (e.g. build/aegis.img)")
    ap.add_argument("--quiet", "-q", action="store_true", help="suppress summary")
    args = ap.parse_args()

    if not args.elf.exists():
        die(f"{args.elf} not found — run `make -C kernel/arch/arm64` first")

    args.img.parent.mkdir(parents=True, exist_ok=True)

    tmp_bin = args.img.with_suffix(".flat")
    flatten_elf(args.elf, tmp_bin)
    flat = bytearray(tmp_bin.read_bytes())
    tmp_bin.unlink(missing_ok=True)

    verify_and_stamp_header(flat)
    args.img.write_bytes(flat)

    if not args.quiet:
        magic_bytes = struct.pack("<I", EXPECTED_MAGIC).hex(" ")
        code0_bytes = struct.pack("<I", EXPECTED_CODE0).hex(" ")
        eff_size = round_up(len(flat), 2 * 1024 * 1024)
        print(f"arm64 Image: {args.img}")
        print(f"  input ELF   : {args.elf}")
        print(f"  flat size   : {len(flat)} bytes ({len(flat) / 1024:.1f} KiB)")
        print(f"  image_size  : {eff_size} bytes (2MB rounded)")
        print(f"  code0       : {code0_bytes}  (b .+64)")
        print(f"  magic@0x38  : {magic_bytes}  (ARM\\x64)")
        print(f"  flags       : 0x{EXPECTED_FLAGS:016x}  (4K, little-endian)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
