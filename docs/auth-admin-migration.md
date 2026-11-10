# In-place administrative authorization

## Authentication and identity

Text login verifies the account password, binds that account's numeric uid/gid
with `auth_elevate_session`, switches **gid before uid**, then execs its shell.
All four steps stay in the same process. Binding or identity-switch failure is
fatal: it must not exec a shell under the old identity. Independent children do
not inherit the authenticated binding or admin session.

Despite its legacy name, `auth_elevate_session` is not admin authorization.
Both it and `auth_set_identity` now return checked status. Updating this local
library does **not** fix previously released Bastion/Glyph/libcitadel wrappers:
their authentication/fork order and return checking need separate upstream
changes and component releases.

## Administrative principal and fixed executable boundary

The separate `/etc/aegis/admin` credential authorizes the administrative
principal **uid/gid 0**, not an account-0 password check. Numeric 0 still carries
no ambient kernel authority. An ordinary nonzero account can authorize an
allowlisted administrative operation without weakening filesystem DAC.

The entry protocol is:

```
login --admin-exec <fixed-target-path> [--password-fd N] -- <target arguments>
```

`login` rejects any target not exactly in `admin_targets.h`. It does not search
`PATH`, trust a caller-supplied target `argv[0]`, accept an arbitrary executable,
or launch a shell. The allowed paths are:

- `/bin/installer`
- `/apps/gui-installer/gui-installer`
- `/bin/useradd`
- `/bin/adminpw`
- `/bin/herald`
- `/bin/lorica-update`
- `/bin/lorica-recover` (entry protocol reserved; standalone image blocker below)

After verifying the admin credential once, login checks binding to 0/0,
`setgid(0)`, `setuid(0)`, the new SELF operation **syscall 521**, and active
admin-session query 519. It clears credentials, closes non-stdio descriptors
(the current Aegis ABI has 256 slots), replaces the caller's environment with a
small fixed environment, and execs the fixed target **in the same process**.
No failure has a shell fallback. Older kernels return `ENOSYS` for 521 and fail
closed; do not emulate SELF with `517,2` (legacy kernels interpret nonzero
selectors as parent elevation).

`install_admin_enter` uses the caller's compiled-in fixed path. It returns
success only after checking:

1. active kernel admin state via 519;
2. actual uid **and** gid 0;
3. a complete 64-slot capability snapshot via 362, with the target's required
   WRITE rights (AUTH additionally requires READ).

Installer targets require AUTH, SETUID, INSTALL and DISK_ADMIN; useradd requires
AUTH, SETUID and INSTALL; adminpw requires AUTH and INSTALL; herald requires
INSTALL; the updater requires DISK_ADMIN. Failed/unknown kernel queries fail
closed. A reduced/masked capability set is not accepted.

Login inserts `--admin-ready` solely to detect failed reexec loops. The target
checks live kernel state before removing it; a forged marker grants nothing.
Ordinary adminpw invocation always re-proves the current credential. An
already-authorized caller can forge the marker and skip that extra UX prompt;
the marker is deliberately **not** represented as proof of reauthentication.
The legacy `install_elevate` parent-elevation API remains available for source
compatibility, but migrated executables do not use it to cross the DAC boundary.

The GUI installer gathers its password before disk selection, passes it through
a private pipe FD (never argv/environment), destroys its old window/IPC state,
and reexecs through login. The fresh authorized GUI starts at disk selection.
Invalid authentication terminates the reexec rather than resuming an old UI
with alleged verified state.

## Command dispatch and child processes

Herald authorizes `sync`, `upgrade`, `install`, `remove`, and `recover` before
transaction recovery or any operation. `list`, `search`, `info`, and `verify`
neither prompt nor run recovery as a hidden write side effect. Invalid command
shapes also do not prompt.

`lorica-update` independently authorizes raw disk access. Each freshly exec'd
Herald child must independently authorize its own mutation/recovery; this may
mean several password prompts during an update. No plaintext credential,
verified boolean, or inherited elevated shell skips that boundary.

Herald links the standalone `libelevate.a` (only `install_elevate.o`), not bulk
installer/disk/crypto implementation.

## Provisioning and remaining limits

### Factory granting-anchor invariant

The common `tools/build-rootfs.sh` creates real `/bin`, `/sbin`, and `/apps`
directories for **both server and desktop**, even when no profile payload
uses a directory. It rejects file/symlink skeleton entries at those anchors,
creates the directories before copying payloads, and checks the final ext2
inodes are directories before publishing the image. An empty `/apps` adds no
GUI payload to server.

This is security-significant: ext2 records builtin protected-root inodes at
mount. If `/apps` were absent then, a baseline uid-0 process with owner-write
permission on `/` could create a writable replacement. The policy lookup's
inode-protection check would refuse its policy caps, but pathname allowlisting
alone would not stop login from executing that planted body after a **valid**
admin credential, with the credential-backed session preserved across exec.
The factory invariant ensures the shipped tree is protected before any user
process can populate it; it does not change root-directory DAC or the existing
credential/admin-exec contract.

**Scope:** this fixes the rootfs artifacts produced by this common OS factory.
The existing kernel may still accept an externally supplied/malformed rootfs
missing a granting-anchor inode. It is not a generic untrusted-rootfs fix.
Binding admin-exec to an actually trusted resolved executable object is a
stronger kernel follow-up, not implemented here.

Run `python3 user/tests/factory-anchors/run.py` for full-builder mocked-tool
regressions; add `--real` on Linux with e2fsprogs for actual ext2 layout checks.
The guest expectation, to validate separately with the resulting kernel/image:

1. On **server**, `/apps` exists as an empty directory at boot, with no
   gui-installer payload and no `/apps` unprotected-anchor warning.
2. A baseline **uid-0** process without INSTALL cannot create
   `/apps/gui-installer` or plant
   `/apps/gui-installer/gui-installer`; the operation is denied, not merely
   ignored by the package manager.
3. A direct admin-exec request for the absent GUI target cannot execute a
   replacement body. Valid authentication does not make a nonexistent target
   exist.

Factory-layout tests do not themselves prove these runtime kernel denials;
the parent integration workflow owns the actual VM check.

### Account provisioning

Home creation builds the tree before ownership transfer, transfers Pictures
before the home directory, skips chown only when lstat proves the exact uid/gid,
and propagates every directory/stat/chown failure. Credential writing checks
close/chmod and home failures. Useradd does not print success if home creation
fails; account files may already have been appended, which its error reports.
Multi-file account provisioning is not transactional in this migration.

Existing SETUID + INSTALL and owner/sensitive-inode/DAC checks remain intact.
The administrative-principal transition permits the normal root-owned system
files and primary-uid-0 workflow without making uid 0 omnipotent. Administrators
cannot bypass a contrary inode owner/DAC policy; unexpected ownership still
fails closed and needs explicit repair.

**Standalone offline recovery is not migrated.** Its rootfs currently installs
the recovery binary as `/bin/vigil`, declares explicit service disk authority,
and has no trusted login/admin credential. `--authorized` there is a control-flow
marker, not evidence of authentication. Making this image require login without
first designing its trusted credential/bootstrap path would make it unusable.
No new recovery credential, exemption, or bootstrap success flag is invented
here. This requires a separate image/authority design.

## Focused validation

Run `python3 user/tests/auth-migration/run.py` on a host with C compiler and
crypt support. It compiles the actual library/login code with mocked Aegis
syscalls and I/O, exercises real host crypt success/mismatch, injects every
home/credential syscall failure, and compiles the production Herald dispatcher
with side-effect-free operation mocks. It does not issue host identity-changing
syscalls.

These tests are not evidence of an ISO boot, GUI compositor behavior, or real
kernel SELF/exec policy transitions. Validate the integrated kernel and image
in Linux/QEMU, including a nonzero account invoking an admin operation, separate
Herald authorization inside updater, and the external component limitations.

## Validation status

Recorded from the migration effort; see each repository's docs for the kernel
side.

Done:

- All host suites in this document, plus `user/tests/factory-anchors/run.py`,
  `make -C user/bin/stsh host-test` (24 tests), and `tools/test-local-kernel.sh`.
- Patched musl 1.2.5, Vigil and vforkstress built with a real Linux musl
  toolchain; server ISO assembled from a checksum-verified, explicitly
  selected unpublished kernel.
- Canonical `tools/servertest.sh` boot passed on QEMU/KVM: console login with
  no graphical stack.
- An interactive run logged in with the live credentials and then exposed a
  pre-existing Aegis framebuffer race (`_fb_draw_char` writing the row just
  past the shadow buffer while a concurrent kernel log scrolled the terminal).
  That kernel bug is fixed and covered by a deterministic regression; it is not
  a migration regression.

Still required: a full interactive guest run of the rebuilt image — logins as
primary and nonzero users, redirections/heredocs/subshells/`command`, spawn
failure reporting, admin credential failure then success, account creation and
home ownership, and the baseline-user `/apps` planting denial.
`tools/migration-servertest.py` exercises this set over the serial console.
