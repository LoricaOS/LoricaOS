/* Fuzz the DHCP client's option parser.
 *
 * parse_options() is static inside the daemon, so we include the TU with main
 * renamed away. Input is the raw options area of a server reply — fully
 * attacker-controlled on an untrusted network, and parsed before any
 * authentication whatsoever (DHCP has none).
 *
 * The buffer is heap-allocated at exactly `len` so ASan catches any read past
 * the end of the received datagram — the real bug class here, since the
 * on-wire options area is a fixed 308-byte field but only `n` bytes actually
 * arrived. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define main dhcp_daemon_main
#include "main.c"
#undef main

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    uint8_t *opts;
    uint8_t msg_type = 0xff;

    if (n > 4096)
        n = 4096;

    opts = malloc(n ? n : 1);
    if (!opts)
        return 0;
    memcpy(opts, d, n);

    parse_options(opts, (int)n, &msg_type);

    free(opts);
    return 0;
}
