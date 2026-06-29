#!/usr/bin/env python3
"""Boot a live ISO, log in, run `herald list`, and assert on its output.

Usage: herald-list-driver.py <iso> <must-csv> <mustnot-csv> <done-pattern>
  must-csv     comma-separated substrings that MUST appear in the herald list
  mustnot-csv  comma-separated substrings that must NOT appear ("" = none)
  done-pattern a regex known to appear once `herald list` finishes (so we
               capture the full output before tearing down)

Used by desktop-pkg-test.sh Part C to prove the desktop live system lists the
whole graphical stack as installed while the server live system lists nothing.
`herald list` only reads the db, so no admin elevation is needed.
"""
import sys, re, time, select, subprocess

def main():
    iso = sys.argv[1]
    must    = [s for s in sys.argv[2].split(",") if s]
    mustnot = [s for s in sys.argv[3].split(",") if s]
    done    = sys.argv[4]
    qemu = ["qemu-system-x86_64", "-machine", "pc", "-cdrom", iso, "-boot", "order=d",
            "-display", "none", "-vga", "std", "-nodefaults", "-serial", "stdio",
            "-no-reboot", "-m", "2048M"]
    steps = [
        (r"login: ",        "root\n",                          120),
        (r"password: ",      "forevervigilant\n",               30),
        (r"\[STSH\] ready",  "herald list\n",                   30),
        (done,               "",                                30),
    ]
    p = subprocess.Popen(qemu, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0)
    transcript = []
    def pump(pat, timeout):
        rx = re.compile(pat); window = ""; end = time.monotonic() + timeout
        while time.monotonic() < end:
            r,_,_ = select.select([p.stdout], [], [], 0.5)
            if not r:
                if p.poll() is not None: return False
                continue
            chunk = p.stdout.read1(4096) if hasattr(p.stdout,"read1") else p.stdout.read(4096)
            if not chunk:
                if p.poll() is not None: return False
                continue
            t = chunk.decode("utf-8","replace"); sys.stdout.write(t); sys.stdout.flush()
            transcript.append(t); window = (window+t)[-8192:]
            if rx.search(window): return True
        return False
    ok = True
    for pat, send, to in steps:
        if not pump(pat, to):
            print(f"\n[hl-driver] TIMEOUT waiting for {pat!r}", flush=True); ok = False; break
        if send:
            try: p.stdin.write(send.encode()); p.stdin.flush()
            except BrokenPipeError: ok = False; break
    try: p.terminate(); p.wait(timeout=5)
    except Exception: p.kill()
    full = "".join(transcript)
    # Only look at output AFTER the `herald list` command was issued.
    seg = full.split("herald list", 1)[-1]
    miss = [m for m in must if m not in seg]
    bad  = [m for m in mustnot if m in seg]
    if miss: print(f"\n[hl-driver] FAIL missing: {miss}", flush=True); ok = False
    if bad:  print(f"\n[hl-driver] FAIL forbidden: {bad}", flush=True); ok = False
    print(f"\n[hl-driver] {'PASS' if ok else 'FAIL'}", flush=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
