#!/usr/bin/env python3
"""Boot a disposable ARM64 RAM-root guest and verify setup/login/external commands.

Requires pexpect and the tools/firmware used by run-arm64.sh. No host disks or
network are attached. The test account exists only in guest RAM.
"""
import argparse
from pathlib import Path

import pexpect

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("iso", nargs="?", type=Path,
                    default=root / "build/loricaos-arm64-server.iso")
parser.add_argument("--timeout", type=int, default=60)
parser.add_argument("--log", type=Path,
                    default=root / "build/arm64-server/qemu-session.log")
args = parser.parse_args()
args.log.parent.mkdir(parents=True, exist_ok=True)

with args.log.open("w") as log:
    child = pexpect.spawn("bash", [str(root / "tools/run-arm64.sh"), str(args.iso.resolve())],
                          encoding="utf-8", timeout=args.timeout)
    child.logfile_read = log
    try:
        child.expect_exact("Username: ")
        child.sendline("dev")
        child.expect_exact("Password: ")
        child.sendline("qemu-test")
        child.expect_exact("Confirm password: ")
        child.sendline("qemu-test")
        child.expect_exact("Account 'dev' created")
        print("PASS: first-boot account setup", flush=True)
        child.expect_exact("login: ")
        child.sendline("dev")
        child.expect_exact("password: ")
        child.sendline("qemu-test")
        prompt = r"dev@loricaos:[^\r\n]*[$#] "
        child.expect_exact("[STSH] ready")
        child.expect(prompt)
        print("PASS: authenticated login reached stsh", flush=True)
        for command, output in (
            ("/bin/uname -m", "aarch64"),
            ("/bin/cat /etc/lorica-version", (root / "VERSION").read_text().strip()),
            ("/bin/ls /bin", "vigil"),
        ):
            child.sendline(command)
            child.expect_exact(output)
            child.expect(prompt)
            print(f"PASS: {command}", flush=True)
    finally:
        child.terminate(force=True)
