/* sysfuzz — syscall boundary/robustness fuzzer.
 *
 * Hammers the syscall dispatch table with malformed and boundary arguments that
 * a normal libc never produces: kernel-space and non-canonical pointers, huge
 * and negative lengths, out-of-range and freed fds, unaligned addresses. The
 * kernel must reject every one with an errno (or handle it) and NEVER panic,
 * corrupt memory, or hang. This is a robustness/DoS smoke test for the uaccess
 * and bounds-validation layer, not a coverage-guided fuzzer.
 *
 * The test passes iff the process survives the whole battery and prints the
 * marker — i.e. no syscall took down the kernel. On a kernel with an unguarded
 * copy_from_user / missing bounds check, one of these calls panics and the VM
 * dies before the marker.
 *
 *   SYSFUZZ_OK count=<n>   — all <n> probe calls returned without killing us
 *
 * Baseline caps only.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#ifndef SYS_read
#include <sys/syscall.h>
#endif

/* A few hostile pointer values. */
#define KPTR   0xFFFFFFFF80000000ULL   /* kernel higher-half */
#define NONCAN 0x0000800000000000ULL   /* first non-canonical user address */
#define HUGE   0x7FFFFFFFFFFFFFFFULL
#define NEG1   0xFFFFFFFFFFFFFFFFULL

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
    return syscall(n, a, b, c, d, e, f);
}

int main(void)
{
    long count = 0;

    /* Helper macro: make a call, count it. We don't assert on the return —
     * the property under test is "the kernel survived", which holds iff we
     * reach the final print. */
#define PROBE(n,a,b,c,d,e,f) do { (void)sc((n),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f)); count++; } while (0)

    /* read/write/readv/writev with kernel + non-canonical buffers, huge lens */
    PROBE(SYS_read,  0, KPTR,   4096, 0,0,0);
    PROBE(SYS_read,  0, NONCAN, 4096, 0,0,0);
    PROBE(SYS_read,  0, 0x1000, HUGE, 0,0,0);
    PROBE(SYS_write, 1, KPTR,   4096, 0,0,0);
    PROBE(SYS_write, 1, NONCAN, HUGE, 0,0,0);
    PROBE(SYS_writev, 1, KPTR, 16, 0,0,0);
    PROBE(SYS_writev, 1, 0x1000, HUGE, 0,0,0);   /* huge iovcnt */
    PROBE(SYS_readv,  0, KPTR, NEG1, 0,0,0);

    /* fd-bounds: out-of-range, negative, never-opened */
    PROBE(SYS_close, HUGE, 0,0,0,0,0);
    PROBE(SYS_close, NEG1, 0,0,0,0,0);
    PROBE(SYS_close, 9999, 0,0,0,0,0);
    PROBE(SYS_fstat, 9999, KPTR, 0,0,0,0);
    PROBE(SYS_lseek, 9999, 0, 0,0,0,0);
    PROBE(SYS_dup,   9999, 0,0,0,0,0);
    PROBE(SYS_dup2,  9999, 9998, 0,0,0,0);
    PROBE(SYS_ioctl, 9999, 0x5401, KPTR, 0,0,0);

    /* path syscalls with hostile path pointers */
    PROBE(SYS_open,   KPTR,   0, 0,0,0,0);
    PROBE(SYS_open,   NONCAN, 0, 0,0,0,0);
    PROBE(SYS_stat,   KPTR,   0x1000, 0,0,0,0);
    PROBE(SYS_lstat,  KPTR,   0x1000, 0,0,0,0);
    PROBE(SYS_execve, KPTR,   0, 0,0,0,0);
    PROBE(SYS_mkdir,  NONCAN, 0, 0,0,0,0);
    PROBE(SYS_unlink, KPTR,   0, 0,0,0,0);
    PROBE(SYS_rename, KPTR,   NONCAN, 0,0,0,0);
    PROBE(SYS_readlink, KPTR, 0x1000, HUGE, 0,0,0);

    /* mmap/mprotect/munmap with bad addrs/lens/prot */
    PROBE(SYS_mmap,  KPTR,   HUGE, 0xFFFF, 0, NEG1, 0);  /* bad prot+len+fd */
    PROBE(SYS_mmap,  0,      0,    0,      0x20, NEG1, 0); /* len 0 */
    PROBE(SYS_munmap, KPTR,  HUGE, 0,0,0,0);
    PROBE(SYS_mprotect, KPTR, HUGE, 7, 0,0,0);            /* W+X on kernel range */

    /* signal/proc with hostile pointers + out-of-range signums */
    PROBE(SYS_rt_sigaction, 9999, KPTR, KPTR, 8,0,0);    /* signum out of range */
    PROBE(SYS_rt_sigaction, 9, NONCAN, 0, 8,0,0);
    PROBE(SYS_rt_sigprocmask, 0, KPTR, KPTR, 8,0,0);
    PROBE(SYS_kill, NEG1, 9, 0,0,0,0);
    PROBE(SYS_nanosleep, KPTR, 0, 0,0,0,0);

    /* socket family with hostile sockaddr/optval/len */
    PROBE(SYS_socket, 2 /*AF_INET*/, 1 /*STREAM*/, 0, 0,0,0);
    PROBE(SYS_setsockopt, 9999, 1, 20 /*SO_RCVTIMEO*/, KPTR, 16, 0);
    PROBE(SYS_getsockname, 9999, KPTR, KPTR, 0,0,0);
    PROBE(SYS_sendto, 9999, KPTR, HUGE, 0, NONCAN, 16);
    PROBE(SYS_recvfrom, 9999, KPTR, HUGE, 0, KPTR, KPTR);

    /* clone with junk flags (should EINVAL, not crash) */
    PROBE(SYS_clone, NEG1, 0, 0,0,0,0);

    /* getdents64 with a kernel buffer + huge count */
    PROBE(SYS_getdents64, 9999, KPTR, HUGE, 0,0,0);

    printf("SYSFUZZ_OK count=%ld\n", count);
    return 0;
}
