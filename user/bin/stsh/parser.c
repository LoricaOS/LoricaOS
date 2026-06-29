#include "stsh.h"

/*
 * parse_command — tokenize a single command segment, pulling out redirects.
 * Modifies seg in-place (inserts NUL terminators).
 * Returns argc (number of argv entries, not counting the NULL terminator).
 */
int
parse_command(char *seg, cmd_t *cmd)
{
    cmd->argv[0]          = NULL;
    cmd->stdin_file       = NULL;
    cmd->stdout_file      = NULL;
    cmd->stdout_append    = 0;
    cmd->stderr_to_stdout = 0;

    int argc = 0;
    char *p  = seg;

    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;

        /* 2>&1 — must check before single '>' */
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            cmd->stderr_to_stdout = 1;
            p += 4;
            continue;
        }

        /* >> (append) — store path with append flag */
        if (p[0] == '>' && p[1] == '>') {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdout_file = p;
            cmd->stdout_append = 1;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* > (truncate) */
        if (*p == '>') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdout_file = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* < (stdin redirect) */
        if (*p == '<') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            cmd->stdin_file = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        /* Regular argument */
        if (argc >= MAX_ARGV) break;
        cmd->argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    cmd->argv[argc] = NULL;
    return argc;
}

/*
 * parse_pipeline — split line on '|' into up to max cmd_t entries.
 * Modifies line in-place. Returns number of stages (0 if line is empty).
 */
int
parse_pipeline(char *line, cmd_t *cmds, int max)
{
    int bg;
    return parse_pipeline_bg(line, cmds, max, &bg);
}

/* Like parse_pipeline but also detects a trailing '&' (background). */
int
parse_pipeline_bg(char *line, cmd_t *cmds, int max, int *bg_out)
{
    *bg_out = 0;

    /* Strip trailing whitespace and check for trailing '&' */
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n'))
        end--;
    if (end > line && end[-1] == '&') {
        *bg_out = 1;
        *--end = '\0';
    }

    int n     = 0;
    char *p   = line;
    char *seg = line;

    while (*p) {
        if (*p == '|') {
            *p++ = '\0';
            if (n < max) {
                if (parse_command(seg, &cmds[n]) > 0)
                    n++;
            }
            seg = p;
        } else {
            p++;
        }
    }
    /* Last (or only) segment */
    if (n < max && parse_command(seg, &cmds[n]) > 0)
        n++;

    return n;
}
