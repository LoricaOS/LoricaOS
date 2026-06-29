/* feeds_net.c — network fetch for the Aegis "Feeds" RSS reader.
 *
 * Downloads happen by fork/exec of /bin/curl, NOT in-process: curl carries the
 * NET_SOCKET capability (via /etc/aegis/caps.d/curl) and this app does not, so
 * the network surface stays in curl. We fetch the feed URL to a pid-unique temp
 * file under /tmp, read it back into a freshly malloc'd NUL-terminated buffer,
 * and always remove the temp file before returning.
 *
 * Same trust/lib pattern as user/bin/herald/net.c. We exec curl with an argv
 * array (no shell), so a URL can never be interpreted as a shell command.
 */

#include "feeds.h"

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* Cap a single fetch at 1 MiB. A feed larger than this is fine to truncate —
 * the parser only keeps ITEMS_MAX items. */
#define FEED_FETCH_MAX  1048576u

/* Write a short reason into errbuf, guarding NULL / zero size. */
static void set_err(char *errbuf, size_t errsz, const char *msg)
{
    if (errbuf == NULL || errsz == 0)
        return;
    snprintf(errbuf, errsz, "%s", msg);
}

int feed_fetch(const char *url, char **out, size_t *len,
               char *errbuf, size_t errsz)
{
    char    tmppath[64];
    pid_t   pid;
    int     status = 0;
    int     fd = -1;
    char   *buf = NULL;
    size_t  total = 0;
    int     truncated = 0;

    /* ── validate args ─────────────────────────────────────────────────── */
    if (url == NULL || url[0] == '\0') {
        set_err(errbuf, errsz, "no url");
        return -1;
    }
    if (out == NULL || len == NULL) {
        set_err(errbuf, errsz, "bad arguments");
        return -1;
    }
    *out = NULL;
    *len = 0;

    /* pid-unique temp path so concurrent fetches don't collide. */
    snprintf(tmppath, sizeof(tmppath), "/tmp/feeds-%d.xml", (int)getpid());

    /* ── fork/exec curl ────────────────────────────────────────────────── */
    pid = fork();
    if (pid < 0) {
        set_err(errbuf, errsz, "fork failed");
        return -1;
    }
    if (pid == 0) {
        /* -s silent, -f fail on HTTP >=400, -L follow redirects (feeds redirect
         * a lot), -k skip TLS cert validation (Aegis curl ships no CA bundle;
         * the reader doesn't need a trusted transport), -m 25 cap at 25s (feeds
         * can be slow), -A set a User-Agent (some servers 403 a blank UA),
         * -o write to the temp file. */
        char *argv[] = {
            (char *)"/bin/curl", (char *)"-s", (char *)"-f", (char *)"-L",
            (char *)"-k", (char *)"-m", (char *)"25",
            (char *)"-A", (char *)"AegisFeeds/1.0",
            (char *)"-o", (char *)tmppath, (char *)url, (char *)0
        };
        char *envp[] = { (char *)0 };
        execve("/bin/curl", argv, envp);
        _exit(127);   /* exec failed (curl missing, etc.) */
    }

    if (waitpid(pid, &status, 0) < 0) {
        set_err(errbuf, errsz, "waitpid failed");
        goto fail;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char msg[STR_ERR];
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        snprintf(msg, sizeof(msg), "fetch failed (curl %d)", code);
        set_err(errbuf, errsz, msg);
        goto fail;
    }

    /* ── read the temp file back ───────────────────────────────────────── */
    fd = open(tmppath, O_RDONLY);
    if (fd < 0) {
        set_err(errbuf, errsz, "open failed");
        goto fail;
    }

    buf = malloc(FEED_FETCH_MAX + 1);
    if (buf == NULL) {
        set_err(errbuf, errsz, "out of memory");
        goto fail;
    }

    for (;;) {
        if (total >= FEED_FETCH_MAX) {
            truncated = 1;
            break;
        }
        ssize_t n = read(fd, buf + total, FEED_FETCH_MAX - total);
        if (n < 0) {
            set_err(errbuf, errsz, "read failed");
            goto fail;
        }
        if (n == 0)
            break;            /* EOF */
        total += (size_t)n;
    }

    close(fd);
    fd = -1;

    if (total == 0) {
        set_err(errbuf, errsz, "empty response");
        goto fail;
    }

    buf[total] = '\0';        /* always NUL-terminate */
    (void)truncated;          /* truncation is benign; parser caps items */

    unlink(tmppath);
    *out = buf;
    *len = total;
    return 0;

fail:
    if (fd >= 0)
        close(fd);
    if (buf != NULL)
        free(buf);
    unlink(tmppath);
    return -1;
}
