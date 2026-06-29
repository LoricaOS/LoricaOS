#include "stsh.h"

/* KEY=VALUE storage — 64 entries, 256 bytes each */
static char s_env_store[ENV_MAX][256];
static char *s_env_ptrs[ENV_MAX + 1]; /* NULL-terminated for execve */
static int  s_env_count;

/*
 * env_init — copy environment from login's envp.
 */
void
env_init(char **envp)
{
    s_env_count = 0;
    if (!envp)
        return;

    for (int i = 0; envp[i] && s_env_count < ENV_MAX; i++) {
        strncpy(s_env_store[s_env_count], envp[i],
                sizeof(s_env_store[0]) - 1);
        s_env_store[s_env_count][sizeof(s_env_store[0]) - 1] = '\0';
        s_env_ptrs[s_env_count] = s_env_store[s_env_count];
        s_env_count++;
    }
    s_env_ptrs[s_env_count] = NULL;
}

/*
 * env_get — find value by key. Returns NULL if not found.
 */
const char *
env_get(const char *key)
{
    int klen = (int)strlen(key);
    for (int i = 0; i < s_env_count; i++) {
        if (strncmp(s_env_store[i], key, klen) == 0 &&
            s_env_store[i][klen] == '=')
            return &s_env_store[i][klen + 1];
    }
    return NULL;
}

/*
 * env_set — add or update a KEY=VALUE pair.
 */
void
env_set(const char *key, const char *value)
{
    int klen = (int)strlen(key);

    /* Try to update existing */
    for (int i = 0; i < s_env_count; i++) {
        if (strncmp(s_env_store[i], key, klen) == 0 &&
            s_env_store[i][klen] == '=') {
            snprintf(s_env_store[i], sizeof(s_env_store[0]),
                     "%s=%s", key, value);
            return;
        }
    }

    /* Add new */
    if (s_env_count >= ENV_MAX) {
        fprintf(stderr, "stsh: env full\n");
        return;
    }
    snprintf(s_env_store[s_env_count], sizeof(s_env_store[0]),
             "%s=%s", key, value);
    s_env_ptrs[s_env_count] = s_env_store[s_env_count];
    s_env_count++;
    s_env_ptrs[s_env_count] = NULL;
}

/*
 * env_print_all — print all environment variables.
 */
void
env_print_all(void)
{
    for (int i = 0; i < s_env_count; i++)
        printf("%s\n", s_env_store[i]);
}

/*
 * env_as_array — return NULL-terminated array for execve.
 */
char **
env_as_array(void)
{
    return s_env_ptrs;
}

/*
 * env_expand — expand $VAR, ${VAR}, and $? in src into dst.
 * Unset variables expand to empty string. No command substitution.
 */
void
env_expand(const char *src, char *dst, int dstlen, int last_exit)
{
    int di = 0;
    int si = 0;
    int slen = (int)strlen(src);

    while (si < slen && di < dstlen - 1) {
        if (src[si] != '$') {
            dst[di++] = src[si++];
            continue;
        }

        si++; /* skip '$' */
        if (si >= slen) {
            /* trailing $ */
            dst[di++] = '$';
            break;
        }

        /* $? — last exit status */
        if (src[si] == '?') {
            si++;
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d", last_exit);
            for (int j = 0; tmp[j] && di < dstlen - 1; j++)
                dst[di++] = tmp[j];
            continue;
        }

        /* ${VAR} */
        if (src[si] == '{') {
            si++;
            char varname[128];
            int vi = 0;
            while (si < slen && src[si] != '}' && vi < (int)sizeof(varname) - 1)
                varname[vi++] = src[si++];
            varname[vi] = '\0';
            if (si < slen && src[si] == '}')
                si++;
            const char *val = env_get(varname);
            if (val) {
                for (int j = 0; val[j] && di < dstlen - 1; j++)
                    dst[di++] = val[j];
            }
            continue;
        }

        /* $VAR — alphanumeric + underscore */
        {
            char varname[128];
            int vi = 0;
            while (si < slen && vi < (int)sizeof(varname) - 1 &&
                   (src[si] == '_' ||
                    (src[si] >= 'a' && src[si] <= 'z') ||
                    (src[si] >= 'A' && src[si] <= 'Z') ||
                    (src[si] >= '0' && src[si] <= '9'))) {
                varname[vi++] = src[si++];
            }
            varname[vi] = '\0';
            if (vi == 0) {
                /* bare $ followed by non-var char */
                dst[di++] = '$';
                continue;
            }
            const char *val = env_get(varname);
            if (val) {
                for (int j = 0; val[j] && di < dstlen - 1; j++)
                    dst[di++] = val[j];
            }
        }
    }
    dst[di] = '\0';
}
