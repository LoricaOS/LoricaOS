#include "stsh.h"

#define MAX_CANDIDATES 64

/*
 * is_first_token — returns 1 if pos is within the first token (command name).
 */
static int
is_first_token(const char *buf, int pos)
{
    for (int i = 0; i < pos; i++) {
        if (buf[i] == ' ' || buf[i] == '\t')
            return 0;
    }
    return 1;
}

/*
 * extract_prefix — extract the word being completed at cursor position.
 * Returns start index of the word in buf.
 */
static int
extract_prefix(const char *buf, int pos, char *prefix, int plen)
{
    int start = pos;
    while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '\t')
        start--;

    int wlen = pos - start;
    if (wlen >= plen)
        wlen = plen - 1;
    memcpy(prefix, &buf[start], wlen);
    prefix[wlen] = '\0';
    return start;
}

/*
 * find_last_slash — find directory prefix and base name from a path.
 * Returns pointer to base name within prefix string.
 */
static const char *
split_dir_base(const char *prefix, char *dir, int dirlen)
{
    const char *slash = strrchr(prefix, '/');
    if (!slash) {
        strncpy(dir, ".", dirlen);
        dir[dirlen - 1] = '\0';
        return prefix;
    }

    int dlen = (int)(slash - prefix) + 1;
    if (dlen >= dirlen)
        dlen = dirlen - 1;
    memcpy(dir, prefix, dlen);
    dir[dlen] = '\0';
    return slash + 1;
}

/*
 * longest_common_prefix — find length of longest common prefix among
 * candidates.
 */
static int
longest_common_prefix(char **cands, int ncands)
{
    if (ncands <= 0)
        return 0;

    int lcp = (int)strlen(cands[0]);
    for (int i = 1; i < ncands; i++) {
        int j = 0;
        while (j < lcp && cands[i][j] && cands[0][j] == cands[i][j])
            j++;
        lcp = j;
    }
    return lcp;
}

/*
 * complete — perform tab completion on buf at position *pos.
 * Modifies buf, *pos, *len in place.
 */
void
complete(char *buf, int *pos, int *len, const char *prompt)
{
    char prefix[256];
    int word_start = extract_prefix(buf, *pos, prefix, sizeof(prefix));
    int plen = (int)strlen(prefix);
    int first_token = is_first_token(buf, *pos);

    char *candidates[MAX_CANDIDATES];
    int ncands = 0;

    if (first_token) {
        /* Builtin completion */
        static const char *builtins[] = {
            "cd", "exit", "help", "export", "env",
            "caps", "sandbox", "grant", NULL
        };
        for (int bi = 0; builtins[bi] && ncands < MAX_CANDIDATES; bi++) {
            if (plen == 0 || strncmp(builtins[bi], prefix, plen) == 0) {
                candidates[ncands] = strdup(builtins[bi]);
                if (candidates[ncands])
                    ncands++;
            }
        }

        /* Command completion — scan /bin */
        DIR *d = opendir("/bin");
        if (!d)
            goto done_first;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && ncands < MAX_CANDIDATES) {
            if (ent->d_name[0] == '.')
                continue;
            if (plen == 0 || strncmp(ent->d_name, prefix, plen) == 0) {
                candidates[ncands] = strdup(ent->d_name);
                if (candidates[ncands])
                    ncands++;
            }
        }
        closedir(d);
    done_first: ;
    } else {
        /* File completion */
        char dir[256];
        const char *base = split_dir_base(prefix, dir, sizeof(dir));
        int blen = (int)strlen(base);
        int show_hidden = (base[0] == '.');

        DIR *d = opendir(dir);
        if (!d)
            return;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && ncands < MAX_CANDIDATES) {
            /* Skip hidden files unless prefix starts with . */
            if (ent->d_name[0] == '.' && !show_hidden)
                continue;
            /* Skip . and .. */
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            if (blen == 0 || strncmp(ent->d_name, base, blen) == 0) {
                candidates[ncands] = strdup(ent->d_name);
                if (candidates[ncands])
                    ncands++;
            }
        }
        closedir(d);
    }

    if (ncands == 0)
        return;

    if (ncands == 1) {
        /* Single match — complete inline with trailing space */
        const char *match = candidates[0];
        int mlen = (int)strlen(match);
        int insert_len;

        if (first_token) {
            insert_len = mlen - plen;
        } else {
            /* For file completion, base was the part after last slash */
            char dir[256];
            const char *base = split_dir_base(prefix, dir, sizeof(dir));
            int blen = (int)strlen(base);
            insert_len = mlen - blen;
        }

        /* Check if the completed item is a directory */
        char fullpath[512];
        if (first_token) {
            snprintf(fullpath, sizeof(fullpath), "/bin/%s", match);
        } else {
            char dir[256];
            split_dir_base(prefix, dir, sizeof(dir));
            snprintf(fullpath, sizeof(fullpath), "%s%s", dir, match);
        }

        struct stat st;
        int is_dir = (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode));
        char suffix = is_dir ? '/' : ' ';

        /* Make room and insert */
        int to_insert = insert_len + 1; /* +1 for suffix */
        if (*len + to_insert < LINE_SIZE) {
            memmove(&buf[*pos + to_insert], &buf[*pos], *len - *pos);
            memcpy(&buf[*pos], &match[mlen - insert_len], insert_len);
            buf[*pos + insert_len] = suffix;
            *pos += to_insert;
            *len += to_insert;
            buf[*len] = '\0';
        }
    } else {
        /* Multiple matches — find longest common prefix and show candidates */
        int lcp = longest_common_prefix(candidates, ncands);
        int base_offset;

        if (first_token) {
            base_offset = plen;
        } else {
            char dir[256];
            const char *base = split_dir_base(prefix, dir, sizeof(dir));
            base_offset = (int)strlen(base);
        }

        if (lcp > base_offset) {
            /* Complete to common prefix */
            int insert_len = lcp - base_offset;
            if (*len + insert_len < LINE_SIZE) {
                memmove(&buf[*pos + insert_len], &buf[*pos], *len - *pos);
                memcpy(&buf[*pos], &candidates[0][base_offset], insert_len);
                *pos += insert_len;
                *len += insert_len;
                buf[*len] = '\0';
            }
        }

        /* Display all candidates below the prompt */
        write(STDOUT_FILENO, "\n", 1);
        for (int i = 0; i < ncands; i++) {
            write(STDOUT_FILENO, candidates[i], strlen(candidates[i]));
            write(STDOUT_FILENO, "  ", 2);
        }
        write(STDOUT_FILENO, "\n", 1);
    }

    /* Free candidates */
    for (int i = 0; i < ncands; i++)
        free(candidates[i]);
}
