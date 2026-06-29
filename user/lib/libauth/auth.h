/* auth.h — shared authentication library for Aegis.
 * Used by /bin/login (text) and /bin/bastion (graphical). */
#ifndef AUTH_H
#define AUTH_H

/* Look up username in /etc/passwd.
 * Returns 0 on success, -1 on failure.
 * Fills uid, gid, home, shell from the matching entry. */
int auth_lookup_passwd(const char *username, int *uid, int *gid,
                       char *home, int homelen, char *shell, int shelllen);

/* Look up shadow hash for username in /etc/shadow.
 * Requires CAP_KIND_AUTH. Returns 0 on success, -1 on failure. */
int auth_lookup_shadow(const char *username, char *hash, int hashlen);

/* Verify password against a shadow hash using crypt().
 * Returns 0 on match, -1 on mismatch or error. */
int auth_verify(const char *password, const char *hash);

/* Full authentication check: passwd + shadow + crypt.
 * Returns 0 on success, -1 on failure.
 * On success, fills uid, gid, home (homelen), shell (shelllen). */
int auth_check(const char *username, const char *password,
               int *uid, int *gid,
               char *home, int homelen, char *shell, int shelllen);

/* Set process identity (setuid + setgid). */
void auth_set_identity(int uid, int gid);

/* Legacy stub from the pre-46c exec_caps model. The Phase 46c cap-policy
 * redesign moved capability declaration into /etc/aegis/caps.d/<binary>
 * files read by the kernel at execve time, so this function no longer
 * pre-registers anything. Kept as a no-op so callers in libcitadel /
 * libauth that still reference it don't have to be touched in lockstep. */
void auth_grant_shell_caps(void);

/* Mark the current session as authenticated AND bind the identity that
 * auth_set_identity may switch to. The kernel uses the authenticated flag to
 * grant admin-tier capabilities to processes spawned after login/unlock, and
 * binds (uid, gid) so a subsequent setuid/setgid may ONLY assume that verified
 * identity. MUST be called with the just-authenticated user's uid/gid, BEFORE
 * auth_set_identity. */
void auth_elevate_session(int uid, int gid);

#endif /* AUTH_H */
