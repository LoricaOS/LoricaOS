#!/usr/bin/env python3
"""Seed corpora for the herald/dhcp fuzzers. Valid inputs so libFuzzer starts
from real structure instead of rediscovering ustar headers from scratch."""
import os, struct, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."


def w(corpus, name, data):
    d = os.path.join(root, corpus)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


# ---- tar ------------------------------------------------------------------
def tar_hdr(name, size, typeflag=b"0", prefix=b"", mode=0o644):
    h = bytearray(512)
    h[0:len(name)] = name
    h[100:108] = b"%07o\0" % mode
    h[108:116] = b"0000000\0"
    h[116:124] = b"0000000\0"
    h[124:136] = b"%011o\0" % size
    h[136:148] = b"00000000000\0"
    h[148:156] = b" " * 8            # checksum placeholder (herald ignores it)
    h[156:157] = typeflag
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    h[345:345 + len(prefix)] = prefix
    return bytes(h)


def tar_entry(name, payload, typeflag=b"0", mode=0o644):
    body = payload + b"\0" * (-len(payload) % 512)
    return tar_hdr(name, len(payload), typeflag, mode=mode) + body


MANIFEST = (b"id=fuzz\nname=Fuzz App\nversion=1.0.0\nexec=fuzz\n"
            b"caps=service\npaths=lib/engine/\n")

arc = (tar_entry(b"manifest", MANIFEST)
       + tar_entry(b"apps/", b"", b"5", 0o755)
       + tar_entry(b"apps/fuzz/", b"", b"5", 0o755)
       + tar_entry(b"apps/fuzz/fuzz", b"\x7fELF" + b"A" * 60, mode=0o755)
       + tar_entry(b"lib/engine/libx.so", b"B" * 100)
       + b"\0" * 1024)
# the harness eats one leading mode byte
w("corpus_tar", "basic", b"\x01" + arc)
w("corpus_tar", "basic_noprefix", b"\x00" + arc)
w("corpus_tar", "longprefix", b"\x01" + tar_entry(b"x" * 99, b"hi") + b"\0" * 1024)
w("corpus_tar", "withprefix",
  b"\x01" + tar_hdr(b"f", 2, b"0", b"apps/fuzz") + b"hi" + b"\0" * 510 + b"\0" * 1024)

# ---- manifest -------------------------------------------------------------
w("corpus_manifest", "app", MANIFEST)
w("corpus_manifest", "system",
  b"id=lumen\nname=Lumen\nversion=2.1.0\nclass=system\n"
  b"paths=bin/ usr/share/lumen/\ncaps.lumen-shell=admin NET_SOCKET\n")
w("corpus_manifest", "crlf", b"id=a\r\nname=b\r\nversion=1\r\nexec=a\r\n")
w("corpus_manifest", "comments", b"# hi\n\nid = a \nname=b\nversion=1\nexec=a\n")

# ---- repo (Release + Packages share the harness) --------------------------
w("corpus_repo", "release",
  b"Origin: Chancery\nSuite: stable\nComponents: main\n"
  b"SHA256:\n"
  b" " + b"a" * 64 + b" 1234 main/binary-x86_64/Packages\n"
  b" " + b"b" * 64 + b" 99 main/binary-arm64/Packages\n")
w("corpus_repo", "packages",
  b"Package: lumen-files\nVersion: 1.4.2\nArchitecture: x86_64\n"
  b"Filename: pool/main/lumen-files_1.4.2.hpkg\n"
  b"SHA256: " + b"c" * 64 + b"\n"
  b"Depends: lumen-shell\nDisplay-Name: Files\nExec: lumen-files\n"
  b"Caps: service\n\n"
  b"Package: lumen-term\nVersion: 0.9\nArchitecture: x86_64\n"
  b"Filename: pool/main/lumen-term_0.9.hpkg\nSHA256: " + b"d" * 64 + b"\n\n")

# ---- dhcp -----------------------------------------------------------------
def opt(tag, payload):
    return bytes([tag, len(payload)]) + payload


w("corpus_dhcp", "offer",
  opt(53, b"\x02") + opt(1, b"\xff\xff\xff\x00") + opt(3, b"\xc0\xa8\x01\x01")
  + opt(6, b"\x08\x08\x08\x08") + opt(51, struct.pack(">I", 86400))
  + opt(54, b"\xc0\xa8\x01\x01") + b"\xff")
w("corpus_dhcp", "ack",
  opt(53, b"\x05") + opt(1, b"\xff\xff\x00\x00") + opt(51, b"\x00\x00\x0e\x10")
  + b"\xff" + b"\x00" * 8)
w("corpus_dhcp", "pad", b"\x00" * 16 + opt(53, b"\x05") + b"\xff")

print("seeds written under", os.path.abspath(root))
