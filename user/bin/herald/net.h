#ifndef HERALD_NET_H
#define HERALD_NET_H

/* Fetch `url` to `outpath` by fork/exec of /bin/curl (-s -f -o). curl runs
 * with its own NET_SOCKET policy; herald itself needs no network capability.
 * Returns 0 on success (curl exited 0 and the output file is non-empty),
 * negative otherwise. Each call starts at offset zero, even when outpath
 * exists: callers reuse paths for different versions of an object. */
int herald_fetch(const char *url, const char *outpath);

#endif
