#!/usr/bin/env python3
"""Drive a QEMU serial console through a fixed login conversation and assert.

Used by desktop-pkg-test.sh to prove `herald install <desktop.hpkg>` works for a
class=system package on a real boot: it logs in, elevates to an admin session
(herald's CAP_KIND_INSTALL is admin-tier by design), installs the baked package,
and checks the graphical stack + a cap policy actually landed on the filesystem.

No `expect` on the build box, so this is a tiny select()-based expect loop. It
matches on stable serial markers (login:/password:/[STSH] ready/admin
password:/admin session active) and the herald success line.
"""
import sys, re, time, select, subprocess

def main():
    iso = sys.argv[1]
    qemu = ["qemu-system-x86_64", "-machine", "pc", "-cdrom", iso,
            "-boot", "order=d", "-display", "none", "-vga", "std",
            "-nodefaults", "-serial", "stdio", "-no-reboot", "-m", "2048M"]

    # (wait_for_regex, send_after_match, step_timeout_s). Every entry ends in a
    # wait (pump), so the final step's output is captured before QEMU is killed.
    # The last step cats the installed cap policy for lumen and waits for its
    # contents — proving the class=system payload actually landed on the fs (not
    # just that herald claimed success) and that the POWER cap policy is in place.
    steps = [
        (r"login: ",                           "root\n",                                  120),
        (r"password: ",                         "forevervigilant\n",                       30),
        (r"\[STSH\] ready",                     "admin\n",                                 30),
        (r"admin password: ",                   "administrator\n",                         30),
        (r"admin session active",               "herald install /root/desktop.hpkg\n",     30),
        (r"installed system package desktop",   "cat /etc/aegis/caps.d/lumen\n",           60),
        # sideload a local app that depends on 'desktop' (now installed) plus
        # 'lumen-nonexistent' (not installed): herald must warn about ONLY the
        # missing one — proving deps already present are skipped.
        (r"THREAD_CREATE PROC_READ POWER",      "herald install /root/deptest.hpkg\n",     30),
        (r"install it with: herald install lumen-nonexistent", "",                         30),
    ]
    # all of these must appear somewhere in the transcript for a PASS
    must = [r"installed system package desktop",
            r"kernel cap policy \+ anchors reloaded",
            r"THREAD_CREATE PROC_READ POWER",
            r"installed deptest",
            r"install it with: herald install lumen-nonexistent"]
    # none of these may appear — 'desktop' is installed, so it must NOT be
    # listed as a missing prerequisite of deptest.
    must_not = [r"install it with: herald install desktop"]
    # any of these = immediate FAIL
    fail = [r"SIGNATURE VERIFICATION FAILED", r"install denied", r"install failed",
            r"invalid manifest", r"No such file"]

    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0)
    transcript = []

    def pump(pattern, timeout):
        """Read serial until `pattern` seen (return True) or timeout (False)."""
        rx = re.compile(pattern)
        frx = [re.compile(f) for f in fail]
        window = ""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            r, _, _ = select.select([p.stdout], [], [], 0.5)
            if not r:
                if p.poll() is not None:
                    return False
                continue
            chunk = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else p.stdout.read(4096)
            if not chunk:
                if p.poll() is not None:
                    return False
                continue
            text = chunk.decode("utf-8", "replace")
            sys.stdout.write(text); sys.stdout.flush()
            transcript.append(text)
            window = (window + text)[-8192:]
            for f in frx:
                if f.search(window):
                    # let the failing line be matched by `must` check below too
                    return rx.search(window) is not None
            if rx.search(window):
                return True
        return False

    ok = True
    for pat, send, to in steps:
        if not pump(pat, to):
            print(f"\n[driver] TIMEOUT/EXIT waiting for: {pat!r}", flush=True)
            ok = False
            break
        if send:
            try:
                p.stdin.write(send.encode()); p.stdin.flush()
            except BrokenPipeError:
                ok = False; break

    try:
        p.terminate()
        p.wait(timeout=5)
    except Exception:
        p.kill()

    full = "".join(transcript)
    missing = [m for m in must if not re.search(m, full)]
    hit_fail = [f for f in fail if re.search(f, full)]
    hit_forbidden = [m for m in must_not if re.search(m, full)]
    if missing:
        print(f"\n[driver] FAIL — missing markers: {missing}", flush=True)
        ok = False
    if hit_fail:
        print(f"\n[driver] FAIL — error markers present: {hit_fail}", flush=True)
        ok = False
    if hit_forbidden:
        print(f"\n[driver] FAIL — forbidden markers present: {hit_forbidden}", flush=True)
        ok = False
    print(f"\n[driver] {'PASS' if ok else 'FAIL'}", flush=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
