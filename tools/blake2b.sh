#!/bin/sh
set -eu
if command -v b2sum >/dev/null 2>&1; then
    b2sum "$1" | awk '{print $1}'
else
    python3 -c 'import hashlib,sys; print(hashlib.blake2b(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
fi
