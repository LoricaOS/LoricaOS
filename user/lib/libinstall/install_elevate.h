/* Lightweight per-executable administrator authorization.
 * No installer, disk, crypto or credential-writing dependencies. */
#ifndef INSTALL_ELEVATE_H
#define INSTALL_ELEVATE_H

/* Fork/exec trusted /bin/login -elevate, which verifies the admin credential
 * and asks the kernel to elevate its direct parent (THIS process).
 * NULL uses the interactive tty; non-NULL supplies a password over a private
 * pipe (GUI callers). Returns 0 only after successful helper exit, -1 otherwise.
 * Neither the session nor a verified boolean may be delegated to a new child. */
int install_elevate(const char *admin_pw);

/* In-place administrator entry, for the fixed compiled-in executable path.
 * ready: 1 only with uid/gid 0, active kernel admin session, and every required
 * live cap right; 0 means not ready; -1 means unknown target/query failure.
 * enter: return 0 only when ready, stripping the loop marker; otherwise exec
 * trusted login --admin-exec in THIS process. Any returned failure is fatal.
 * admin_pw is optional, private-FD GUI input, never an argv/env credential.
 * Independent exec'd children must enter independently. */
int install_admin_ready(const char *fixed_target);
int install_admin_enter(const char *fixed_target, int *argc, char ***argv,
                        const char *admin_pw);

#endif
