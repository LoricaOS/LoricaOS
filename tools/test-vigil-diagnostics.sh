#!/bin/sh
set -eu

out="${TMPDIR:-/tmp}/vigil-diagnostics-test"
cc -O2 -Wall -Wextra -Werror -Wno-deprecated-declarations \
    tools/vigil-diagnostics-test.c -o "$out"
"$out"
rm -f "$out"
