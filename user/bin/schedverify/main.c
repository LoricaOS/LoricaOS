#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 32
#define EXEC_ROUNDS 6
#define MEM_BYTES (4u * 1024u * 1024u)

static const char *g_phase = "startup";
static int g_pass, g_fail, g_skip;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void timeout_handler(int sig)
{
    (void)sig;
    const char a[] = "\n[SCHEDVERIFY] FAIL timeout in ";
    write(2, a, sizeof(a) - 1);
    write(2, g_phase, strlen(g_phase));
    write(2, "\n", 1);
    _exit(124);
}

static void phase(const char *name)
{
    g_phase = name;
    alarm(30);
    printf("[SCHEDVERIFY] RUN  %s\n", name);
    fflush(stdout);
}

static void result(const char *name, int ok, const char *detail)
{
    alarm(0);
    printf("[SCHEDVERIFY] %s %s%s%s\n", ok ? "PASS" : "FAIL", name,
           detail && *detail ? " — " : "", detail ? detail : "");
    fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

static void skip(const char *name, const char *why)
{
    alarm(0);
    printf("[SCHEDVERIFY] SKIP %s — %s\n", name, why);
    fflush(stdout);
    g_skip++;
}

static int topology(unsigned char mask[128])
{
    memset(mask, 0, 128);
    long n = syscall(SYS_sched_getaffinity, 0, 128, mask);
    if (n < 0) return -1;
    int cpus = 0;
    for (int i = 0; i < n; i++)
        for (unsigned char b = mask[i]; b; b >>= 1) cpus += b & 1;
    return cpus;
}

static volatile int g_run, g_start, g_ready;
struct worker_count {
    volatile uint64_t value;
    char pad[56];
};
static struct worker_count g_counts[MAX_WORKERS];

static void *spin_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    __atomic_add_fetch(&g_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&g_start, __ATOMIC_SEQ_CST)) sched_yield();
    while (__atomic_load_n(&g_run, __ATOMIC_RELAXED)) g_counts[id].value++;
    return 0;
}

static int spin_group(int n, unsigned ms, uint64_t *total, uint64_t *min,
                      uint64_t *max)
{
    pthread_t th[MAX_WORKERS];
    memset((void *)g_counts, 0, sizeof(g_counts));
    g_ready = g_start = 0; g_run = 1;
    int made = 0;
    for (int i = 0; i < n; i++)
        if (pthread_create(&th[made], 0, spin_worker, (void *)(intptr_t)made) == 0)
            made++;
    while (__atomic_load_n(&g_ready, __ATOMIC_SEQ_CST) < made) sched_yield();
    __atomic_store_n(&g_start, 1, __ATOMIC_SEQ_CST);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
    __atomic_store_n(&g_run, 0, __ATOMIC_SEQ_CST);
    *total = 0; *min = UINT64_MAX; *max = 0;
    for (int i = 0; i < made; i++) {
        pthread_join(th[i], 0);
        *total += g_counts[i].value;
        if (g_counts[i].value < *min) *min = g_counts[i].value;
        if (g_counts[i].value > *max) *max = g_counts[i].value;
    }
    return made;
}

static void test_parallel(int cpus)
{
    phase("parallelism + fairness");
    uint64_t one, omin, omax, all, amin, amax;
    int one_made = spin_group(1, 750, &one, &omin, &omax);
    int want = cpus > MAX_WORKERS ? MAX_WORKERS : cpus;
    int made = spin_group(want, 750, &all, &amin, &amax);
    unsigned scale = one ? (unsigned)(all * 100 / one) : 0;
    char d[160];
    snprintf(d, sizeof(d), "workers=%d/%d scale=%u.%02ux min=%llu max=%llu",
             made, want, scale / 100, scale % 100,
             (unsigned long long)amin, (unsigned long long)amax);
    int parallel = cpus == 1 || scale >= 150;
    int fair = made == want && amin > 0 && amax <= amin * 20;
    result("parallelism + fairness", one_made == 1 && parallel && fair, d);
}

static volatile int g_sleep_done;
static void *sleep_worker(void *arg)
{
    (void)arg;
    struct timespec ts = { 0, 10000000L };
    for (int i = 0; i < 50; i++) nanosleep(&ts, 0);
    __atomic_add_fetch(&g_sleep_done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void test_sleep(int cpus)
{
    phase("sleep/wakeup");
    int n = cpus > MAX_WORKERS ? MAX_WORKERS : cpus;
    if (n < 4) n = 4;
    pthread_t th[MAX_WORKERS]; g_sleep_done = 0;
    uint64_t start = now_ns(); int made = 0;
    for (int i = 0; i < n; i++)
        if (pthread_create(&th[made], 0, sleep_worker, 0) == 0) made++;
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);
    uint64_t ms = (now_ns() - start) / 1000000ull;
    char d[96]; snprintf(d, sizeof(d), "threads=%d completed=%d elapsed=%llums",
                         made, g_sleep_done, (unsigned long long)ms);
    result("sleep/wakeup", made == n && g_sleep_done == made && ms >= 350 && ms < 10000, d);
}

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile uint64_t g_mutex_count;
static void *mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 25000; i++) {
        pthread_mutex_lock(&g_mutex); g_mutex_count++; pthread_mutex_unlock(&g_mutex);
    }
    return 0;
}

static void test_futex(int cpus)
{
    phase("futex contention");
    int n = cpus > 16 ? 16 : cpus; if (n < 4) n = 4;
    pthread_t th[16]; int made = 0; g_mutex_count = 0;
    for (int i = 0; i < n; i++)
        if (pthread_create(&th[made], 0, mutex_worker, 0) == 0) made++;
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);
    uint64_t expect = (uint64_t)made * 25000;
    char d[96]; snprintf(d, sizeof(d), "threads=%d count=%llu/%llu", made,
                         (unsigned long long)g_mutex_count, (unsigned long long)expect);
    result("futex contention", made == n && g_mutex_count == expect, d);
}

static void test_exec(void)
{
    phase("fork/exec/reap storm");
    const int burst = 4;
    int fork_fail = 0, child_fail = 0, launched = 0;
    char *argv[] = { "/bin/true", 0 };
    for (int r = 0; r < EXEC_ROUNDS; r++) {
        pid_t pids[16]; int n = 0;
        for (int i = 0; i < burst; i++) {
            pid_t p = fork();
            if (p == 0) { execve(argv[0], argv, 0); _exit(127); }
            if (p < 0) fork_fail++; else { pids[n++] = p; launched++; }
        }
        for (int i = 0; i < n; i++) {
            int st = 0;
            if (waitpid(pids[i], &st, 0) != pids[i] || !WIFEXITED(st) || WEXITSTATUS(st))
                child_fail++;
        }
    }
    char d[96]; snprintf(d, sizeof(d), "children=%d fork_fail=%d child_fail=%d",
                         launched, fork_fail, child_fail);
    result("fork/exec/reap storm", !fork_fail && !child_fail, d);
}

static volatile int g_mem_errors;
static uint8_t *g_mem_regions[4];

static void *memory_worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    uint8_t *p = g_mem_regions[id];
    for (size_t i = 0; i < MEM_BYTES; i += 4096)
        p[i] = (uint8_t)(id + i / 4096);
    for (size_t i = 0; i < MEM_BYTES; i += 4096)
        if (p[i] != (uint8_t)(id + i / 4096)) {
            __atomic_add_fetch(&g_mem_errors, 1, __ATOMIC_SEQ_CST);
            break;
        }
    return 0;
}

static void test_memory(int cpus)
{
    phase("concurrent memory pressure");
    int n = cpus < 4 ? cpus : 4;
    if (n < 2) n = 2;
    pthread_t th[4];
    int mapped = 0, made = 0;
    g_mem_errors = 0;
    for (int i = 0; i < n; i++) {
        g_mem_regions[i] = mmap(0, MEM_BYTES, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_mem_regions[i] == MAP_FAILED) break;
        mapped++;
    }
    for (int i = 0; i < mapped; i++)
        if (pthread_create(&th[made], 0, memory_worker,
                           (void *)(uintptr_t)made) == 0) made++;
    for (int i = 0; i < made; i++) pthread_join(th[i], 0);
    for (int i = 0; i < mapped; i++) munmap(g_mem_regions[i], MEM_BYTES);
    char d[80]; snprintf(d, sizeof(d), "threads=%d resident=%uMiB errors=%d",
                         made, made * (MEM_BYTES / 1024 / 1024), g_mem_errors);
    result("concurrent memory pressure",
           mapped == n && made == n && !g_mem_errors, d);
}

static int beat(int fd, int ms)
{
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, ms) != 1) return 0;
    char c; return read(fd, &c, 1) == 1;
}

static void test_stop_continue(void)
{
    phase("stop/continue wakeup");
    int p[2], ok = 0;
    if (pipe(p) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            for (;;) { write(p[1], ".", 1); usleep(20000); }
        }
        if (pid > 0) {
            close(p[1]);
            int initial = beat(p[0], 2000);
            int stopped = kill(pid, SIGSTOP) == 0;
            while (beat(p[0], 20)) {}
            int quiet = !beat(p[0], 250);
            int resumed = kill(pid, SIGCONT) == 0 && beat(p[0], 2000);
            ok = initial && stopped && quiet && resumed;
            kill(pid, SIGKILL); waitpid(pid, 0, 0); close(p[0]);
        }
    }
    result("stop/continue wakeup", ok, ok ? "heartbeat stopped and resumed" : "heartbeat transition failed");
}

int main(void)
{
    signal(SIGALRM, timeout_handler);
    uint64_t started = now_ns();
    struct utsname u;
    if (uname(&u) == 0) printf("[SCHEDVERIFY] Aegis %s %s\n", u.release, u.machine);

    phase("CPU topology");
    unsigned char mask[128]; int cpus = topology(mask);
    char d[96]; snprintf(d, sizeof(d), "online=%d mask[0]=0x%02x", cpus, mask[0]);
    result("CPU topology", cpus > 0, d);
    if (cpus < 1) cpus = 1;

    phase("affinity mutation");
    errno = 0;
    long ar = syscall(203, 0, sizeof(mask), mask);
    if (ar < 0 && errno == ENOSYS) skip("affinity mutation", "sched_setaffinity is not implemented");
    else result("affinity mutation", ar == 0, ar == 0 ? "accepted" : strerror(errno));

    test_exec();
    test_stop_continue();
    test_memory(cpus);
    test_parallel(cpus);
    test_sleep(cpus);
    test_futex(cpus);

    uint64_t elapsed = (now_ns() - started) / 1000000ull;
    printf("[SCHEDVERIFY] SUMMARY pass=%d fail=%d skip=%d elapsed=%llums — %s\n",
           g_pass, g_fail, g_skip, (unsigned long long)elapsed,
           g_fail ? "FAIL" : "ALL REQUIRED TESTS PASSED");
    return g_fail ? 1 : 0;
}
