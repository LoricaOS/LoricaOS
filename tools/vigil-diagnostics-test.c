#define VIGIL_LOG_DIR  "/tmp/lorica-vigil-diagnostics-test"
#define VIGIL_ERROR_LOG VIGIL_LOG_DIR "/errors.log"
#define main vigil_program_main
#include "../user/bin/vigil/main.c"
#undef main

static int
contains(const char *path, const char *needle)
{
    char buf[VIGIL_LOG_MAX + 1];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, VIGIL_LOG_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void
cleanup(void)
{
    unlink(VIGIL_LOG);
    unlink(VIGIL_PREVIOUS_LOG);
    unlink(VIGIL_ERROR_LOG);
    rmdir(VIGIL_LOG_DIR);
}

int
main(void)
{
    cleanup();
    diagnostics_init();

    service_t svc;
    memset(&svc, 0, sizeof(svc));
    memcpy(svc.name, "dock", 5);
    service_error(&svc, "exited with status 7");
    persistent_log("info", "vigil", "end=clean");

    diagnostics_init();
    if (!contains(VIGIL_PREVIOUS_LOG, "dock: exited with status 7") ||
        !contains(VIGIL_PREVIOUS_LOG, "end=clean") ||
        !contains(VIGIL_ERROR_LOG, "recorded 1 error(s)"))
        return 1;

    /* The second boot has no clean-end marker: the third must report it. */
    diagnostics_init();
    if (!contains(VIGIL_ERROR_LOG, "ended uncleanly") ||
        !contains(VIGIL_LOG, "level=warning previous-boot"))
        return 1;

    cleanup();
    puts("vigil diagnostics: PASS");
    return 0;
}
