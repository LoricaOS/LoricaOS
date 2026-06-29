/* net.c — herald network fetch.
 *
 * Downloads happen by fork/exec of /bin/curl, NOT in-process: curl carries the
 * NET_SOCKET capability (via /etc/aegis/caps.d/curl) and herald does not, so
 * the network surface stays in curl. Integrity does not depend on the
 * transport — every downloaded package and the repo index are signature- or
 * hash-verified after the fetch.
 *
 * We exec curl with an argv array (no shell), so a URL or filename can never
 * be interpreted as a shell command.
 */

#include "net.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

int herald_fetch(const char *url, const char *outpath)
{
    pid_t pid;
    int status = 0;
    struct stat sb;

    if (url == NULL || outpath == NULL)
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* -s quiet, -f fail on HTTP errors (404 etc.), -o write to file.
         * -k skips TLS certificate validation: Aegis's curl ships no CA bundle,
         * and herald does not need a trusted transport — every Release, Packages
         * list, and package is ECDSA-P256/SHA-256 verified after the fetch, so a
         * MITM cannot substitute content without the (offline) signing key. This
         * makes plain HTTP and HTTPS equally safe, per the herald trust model. */
        char *argv[] = {
            (char *)"/bin/curl", (char *)"-s", (char *)"-f", (char *)"-k",
            (char *)"-o", (char *)outpath, (char *)url, (char *)0
        };
        char *envp[] = { (char *)0 };
        execve("/bin/curl", argv, envp);
        _exit(127);   /* exec failed */
    }

    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;

    /* Confirm a non-empty file actually landed. */
    if (stat(outpath, &sb) != 0 || sb.st_size <= 0)
        return -1;
    return 0;
}
