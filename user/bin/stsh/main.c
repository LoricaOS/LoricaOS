#include "stsh.h"

/*
 * build_prompt — format "user@loricaos:path$ " or "user@loricaos:path# ".
 * Replaces HOME prefix with ~.
 */
static void
build_prompt(char *prompt, int len)
{
    const char *user = env_get("USER");
    if (!user) user = "aegis";

    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd)))
        strncpy(cwd, "?", sizeof(cwd));

    /* Replace HOME prefix with ~ */
    const char *home = env_get("HOME");
    char display_path[256];
    if (home && home[0] && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(display_path, sizeof(display_path), "~%s",
                 &cwd[strlen(home)]);
    } else {
        strncpy(display_path, cwd, sizeof(display_path));
        display_path[sizeof(display_path) - 1] = '\0';
    }

    char suffix = has_cap_delegate() ? '#' : '$';
    if (stsh_admin_active()) {
        /* Elevated session: red prompt + a clear [admin] marker so it's obvious
         * the shell can mutate the system (mirrors the Lumen red admin chrome). */
        snprintf(prompt, len,
                 "\033[1;31m[admin] %s@loricaos:%s#\033[0m ", user, display_path);
    } else {
        snprintf(prompt, len, "%s@loricaos:%s%c ", user, display_path, suffix);
    }
}

int
main(int argc, char **argv, char **envp)
{
    char line[LINE_SIZE];

    /* Initialize environment */
    env_init(envp);

    /* Ensure PATH is set */
    if (!env_get("PATH"))
        env_set("PATH", "/bin");

    /* Cache capability presence */
    caps_init();

    /* Initialize history (privileged = no disk persist) */
    hist_init(has_cap_delegate());

    /* -c command mode: run the string. Per POSIX the first operand after the
     * command_string is $0, and the rest are $1.. */
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        const char *a0     = (argc >= 4) ? argv[3] : argv[0];
        char      **params = (argc >= 5) ? &argv[4] : NULL;
        int         nparam = (argc >= 5) ? argc - 4 : 0;
        run_set_params(a0, params, nparam);
        return run_line(argv[2]);
    }

    /* Script-file mode: `stsh FILE [args...]` — run the file as a shell script
     * (this is what makes stsh usable as /bin/sh for `#!` shebangs). */
    if (argc >= 2 && argv[1][0] != '-') {
        return run_script(argv[1], &argv[2], argc - 2);
    }

    /* Display /etc/motd if it exists (login shell banner) */
    {
        FILE *motd = fopen("/etc/motd", "r");
        if (motd) {
            char buf[256];
            while (fgets(buf, sizeof(buf), motd))
                fputs(buf, stdout);
            fclose(motd);
        }
    }

    /* Ignore SIGCHLD delivery (the kernel does NOT auto-reap on SIG_IGN —
     * Aegis zombifies until an explicit waitpid; run_list()'s per-statement
     * WNOHANG drain does the actual reaping). */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigaction(SIGCHLD, &sa, NULL);
    }

    /* Test-harness marker — emitted once when stsh is about to draw its
     * first prompt. Lets Rust integration tests anchor reliably without
     * scraping the prompt itself (which has no trailing newline). */
    dprintf(2, "[STSH] ready\n");

    /* REPL — accumulates multi-line compound commands (if/for/while/case, open
     * quotes, trailing backslash) until the input is syntactically complete. */
    static char acc[1 << 16];
    int acc_len = 0;
    for (;;) {
        char prompt[512];

        if (acc_len == 0)
            build_prompt(prompt, sizeof(prompt));
        else
            snprintf(prompt, sizeof(prompt), "> ");   /* continuation prompt */

        int rlen = editor_readline(prompt, line, sizeof(line));
        if (rlen < 0) break;                     /* EOF */
        if (rlen == 0 && acc_len == 0) continue; /* empty or Ctrl-C at top */

        /* append this line (+ newline) to the accumulator */
        int add = (int)strlen(line);
        if (acc_len + add + 2 < (int)sizeof(acc)) {
            memcpy(acc + acc_len, line, add); acc_len += add;
            acc[acc_len++] = '\n'; acc[acc_len] = '\0';
        }

        if (sh_incomplete(acc))
            continue;                            /* keep reading */

        if (acc[0] && acc_len > 1) {
            hist_add(acc);
            run_program(acc);
        }
        acc_len = 0; acc[0] = '\0';
    }

    hist_save();
    return g_last_exit;
}
