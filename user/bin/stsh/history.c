#include "stsh.h"

static char s_entries[HIST_SIZE][LINE_SIZE];
static int  s_head;       /* next write slot */
static int  s_count;      /* total entries stored */
static int  s_cursor;     /* navigation position (offset from head) */
static int  s_privileged; /* if 1, never persist to disk */

static void
hist_path(char *buf, int buflen)
{
    const char *home = env_get("HOME");
    if (!home || !home[0])
        home = "/root";
    snprintf(buf, buflen, "%s/.stsh_history", home);
}

/*
 * hist_init — load history from disk if not privileged.
 */
void
hist_init(int privileged)
{
    s_head       = 0;
    s_count      = 0;
    s_cursor     = 0;
    s_privileged = privileged;

    if (privileged)
        return;

    char path[256];
    hist_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[LINE_SIZE];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (line[0] == '\0')
            continue;

        strncpy(s_entries[s_head], line, LINE_SIZE - 1);
        s_entries[s_head][LINE_SIZE - 1] = '\0';
        s_head = (s_head + 1) % HIST_SIZE;
        if (s_count < HIST_SIZE)
            s_count++;
    }
    fclose(f);
}

/*
 * hist_add — add entry, suppress consecutive duplicates.
 */
void
hist_add(const char *line)
{
    if (!line || !line[0])
        return;

    /* Suppress consecutive duplicates */
    if (s_count > 0) {
        int prev = (s_head - 1 + HIST_SIZE) % HIST_SIZE;
        if (strcmp(s_entries[prev], line) == 0)
            return;
    }

    strncpy(s_entries[s_head], line, LINE_SIZE - 1);
    s_entries[s_head][LINE_SIZE - 1] = '\0';
    s_head = (s_head + 1) % HIST_SIZE;
    if (s_count < HIST_SIZE)
        s_count++;

    s_cursor = 0;
}

/*
 * hist_prev — navigate backward (up arrow). Returns NULL if at oldest.
 */
const char *
hist_prev(void)
{
    if (s_cursor >= s_count)
        return NULL;
    s_cursor++;
    int idx = (s_head - s_cursor + HIST_SIZE) % HIST_SIZE;
    return s_entries[idx];
}

/*
 * hist_next — navigate forward (down arrow). Returns NULL if at newest
 * (empty line).
 */
const char *
hist_next(void)
{
    if (s_cursor <= 0)
        return NULL;
    s_cursor--;
    if (s_cursor == 0)
        return NULL; /* back to empty/current line */
    int idx = (s_head - s_cursor + HIST_SIZE) % HIST_SIZE;
    return s_entries[idx];
}

/*
 * hist_save — write history to disk if not privileged.
 */
void
hist_save(void)
{
    if (s_privileged)
        return;

    char path[256];
    hist_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    /* Write oldest to newest */
    int start = (s_count < HIST_SIZE) ? 0 : s_head;
    for (int i = 0; i < s_count; i++) {
        int idx = (start + i) % HIST_SIZE;
        fprintf(f, "%s\n", s_entries[idx]);
    }
    fclose(f);
}

/*
 * hist_reset_cursor — reset navigation position to most recent.
 */
void
hist_reset_cursor(void)
{
    s_cursor = 0;
}
