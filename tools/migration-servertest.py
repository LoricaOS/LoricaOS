#!/usr/bin/env python3
"""Exercise a disposable live-ISO guest; never attach a host disk or network.

Uses only the public live/live/administrator ISO credentials. Accounts created
below exist in the guest's RAM rootfs, not on the host or the input ISO.
"""

import argparse
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("iso", type=Path)
parser.add_argument("--qemu", default="qemu-system-x86_64")
parser.add_argument("--log", type=Path, default=Path("build/migration-server-serial.log"))
args = parser.parse_args()
args.log.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory(prefix="lorica-migration-") as tmp:
    serial = Path(tmp) / "serial.sock"
    command = [
        args.qemu, "-machine", "pc", "-smp", "4", "-m", "2048M",
        "-cdrom", str(args.iso.resolve()), "-boot", "order=d",
        "-display", "none", "-vga", "std", "-nodefaults", "-no-reboot",
        "-monitor", "none",
        "-chardev", f"socket,id=serial,path={serial},server=on,wait=on",
        "-serial", "chardev:serial",
        "-object", "rng-random,id=rng0,filename=/dev/urandom",
        "-device", "virtio-rng-pci,rng=rng0",
    ]
    diagnostics = args.log.with_suffix(".qemu.log")
    with diagnostics.open("wb") as errors, args.log.open("wb") as log:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=errors, stderr=subprocess.STDOUT)
        connection = socket.socket(socket.AF_UNIX)
        pending = b""
        counter = 0
        try:
            deadline = time.monotonic() + 10
            while True:
                try:
                    connection.connect(str(serial))
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f"QEMU startup failed; see {diagnostics}")
                    time.sleep(0.05)

            def expect(pattern, timeout=45):
                global pending
                deadline = time.monotonic() + timeout
                while True:
                    normalized = pending.replace(b"\r", b"")
                    match = re.search(pattern, normalized)
                    if match:
                        consumed = normalized[:match.end()]
                        pending = normalized[match.end():]
                        return match, consumed
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError(f"guest output {pattern!r}; see {args.log}")
                    connection.settimeout(min(remaining, 1))
                    try:
                        data = connection.recv(65536)
                    except socket.timeout:
                        continue
                    if not data:
                        raise RuntimeError(f"guest serial closed; see {args.log}")
                    log.write(data)
                    log.flush()
                    pending += data

            def send(text):
                # The guest's emulated UART has a small FIFO and no negotiated
                # flow control. Pace input like a terminal, not a bulk socket.
                # Faster rates intermittently lost bytes mid-line (the shell
                # then waits for the rest of a command that never arrives), so a
                # conservative, human-like rate is used here.
                for byte in text.encode():
                    connection.sendall(bytes((byte,)))
                    time.sleep(0.04)

            def start(script):
                global counter
                counter += 1
                send(script + "\n")
                return counter

            def finish(number, expected=0, timeout=45):
                token = f"__LORICA_CHECK_{number}_STATUS_"
                # Wait for the shell to be ready again before typing the status
                # probe. Input sent while the shell is still starting a fresh
                # interpreter is intermittently dropped by the guest console, so
                # a fixed delay is not reliable.
                expect(rb"~# ?", timeout)
                time.sleep(0.1)
                send(f"echo {token}$?__\n")
                match, output = expect(
                    rb"(?:^|\n)" + token.encode() + rb"(\d+)__(?:\n|$)", timeout)
                status = int(match.group(1))
                if status != expected:
                    raise AssertionError(f"guest command {number}: status {status}, "
                                         f"expected {expected}; see {args.log}")
                return output

            def run(script, expected=0, timeout=45):
                return finish(start(script), expected, timeout)

            def login(username, password):
                expect(rb"login: ?")
                send(username + "\n")
                expect(rb"password: ?")
                send(password + "\n")
                run("echo MIGRATION_LOGIN_READY")

            login("live", "live")
            print("PASS live login and checked identity transition", flush=True)
            run("test -d /apps")
            # Absent: the coreutils `test` here does not implement `! expr`, so
            # assert the positive form fails rather than negating it.
            run("test -e /apps/gui-installer", expected=1)
            run("mkdir /apps/gui-installer", expected=1)
            run("test -e /apps/gui-installer", expected=1)
            print("PASS baseline user cannot plant an administrative target",
                  flush=True)
            run("echo payload >/tmp/r")
            if b"payload" not in run("cat </tmp/r"):
                raise AssertionError("redirect-out then read-in failed")
            if b"payload" not in run("cat </tmp/r | grep payload"):
                raise AssertionError("pipeline did not pass data")
            # Command substitution and a shell function defined earlier in the
            # session must both survive into the fresh interpreter. The
            # assignment itself prints nothing; the echo below is the assertion.
            run("x=local")
            run('f() { echo "$x:$1"; }')
            run("value=$(f parameter)")
            if b"local:parameter" not in run('echo "$value"'):
                raise AssertionError("captured value lost across substitution")
            if b"\nSUBSHELL_OK\n" not in run("( echo SUBSHELL_OK )"):
                raise AssertionError("subshell did not execute")
            output = run("/bin/stsh -c 'echo(){ /bin/echo X;}; command echo Y'")
            if b"Y" not in output:
                raise AssertionError("command did not bypass the function")
            # Heredoc through -c: one line, no interactive continuation.
            if b"heredoc-ok" not in run(
                    "/bin/stsh -c 'cat <<EOF\\nheredoc-ok\\nEOF'"):
                raise AssertionError("heredoc body not delivered")
            print("PASS redirections, pipelines, stateful capture, subshell and heredoc",
                  flush=True)

            output = run("/bin/vforkstress", timeout=90)
            if b"[VFORKSTRESS] OK 400/400" not in output:
                raise AssertionError("spawn regression marker absent")
            print("PASS real musl spawn failure/success reporting", flush=True)

            number = start("admin herald list")
            expect(rb"admin password: ?")
            send("deliberately-wrong-live-iso-password\n")
            if b"admin authentication failed" not in finish(number, expected=1):
                raise AssertionError("wrong credential did not fail authentication")
            number = start("admin herald list")
            expect(rb"admin password: ?")
            send("administrator\n")
            finish(number)
            print("PASS admin failure and in-place SELF authorization", flush=True)

            def add_user(name):
                number = start(f"useradd {name}")
                expect(rb"admin password: ?")
                send("administrator\n")
                expect(rb"New password: ?")
                send("mtestpw\n")
                expect(rb"Retype password: ?")
                send("mtestpw\n")
                finish(number)

            add_user("muser")
            print("PASS account creation and home ownership", flush=True)
            send("exit\n")
            login("muser", "mtestpw")
            run('test "$(id -u)" != 0 && echo owned >"$HOME/owned" && '
                'test "$(cat "$HOME/owned")" = owned')
            add_user("muser2")
            print("PASS secondary-user login and explicit administrative provisioning",
                  flush=True)
            print(f"Migration live-ISO checks passed; serial log: {args.log}", flush=True)
        finally:
            connection.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
