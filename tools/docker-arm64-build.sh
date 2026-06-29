#!/bin/bash
# Build ARM64 musl-static user binaries inside Docker.
# Usage: bash tools/docker-arm64-build.sh
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"

# Create a simple init.c that uses x86-64 syscall numbers
# (Aegis syscall_dispatch uses x86 numbering)
cat > /tmp/aegis-arm64-init.c << 'INITEOF'
#include <unistd.h>
#include <string.h>

/* Aegis uses x86-64 syscall numbers in syscall_dispatch.
 * musl's syscall() uses the native arch numbers (ARM64).
 * We need a raw SVC wrapper that passes the x86 number. */
static long aegis_syscall(long num, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = num;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

/* x86-64 syscall numbers used by Aegis */
#define SYS_WRITE 1
#define SYS_EXIT  60

int main(void) {
    const char msg[] = "[INIT] Hello from ARM64 musl libc!\n";
    aegis_syscall(SYS_WRITE, 1, (long)msg, sizeof(msg) - 1);
    aegis_syscall(SYS_EXIT, 0, 0, 0);
    return 0;
}
INITEOF

echo "Building ARM64 musl-static init..."
docker run --rm \
    -v "$REPO:/aegis" \
    -v "/tmp/aegis-arm64-init.c:/src/init.c:ro" \
    alpine:latest \
    sh -c '
        apk add --no-cache gcc musl-dev >/dev/null 2>&1
        gcc -static -O2 -nostartfiles -o /aegis/user/init-arm64/init-musl.elf \
            -Wl,-e,main /src/init.c
        ls -la /aegis/user/init-arm64/init-musl.elf
        echo "Done: init-musl.elf built"
    '
