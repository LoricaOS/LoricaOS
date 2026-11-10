# TinySSH migration to strict Aegis: sessions blocked

The source pin in `tools/build-tinyssh.sh` improves **source reproducibility
only**. It does not fix privilege separation or establish working SSH login.
It fetches the immutable upstream archive:

```
commit: 6a60c2aac18da9035a7401316789d6f4df7e295f
URL: https://codeload.github.com/janmojzis/tinyssh/tar.gz/6a60c2aac18da9035a7401316789d6f4df7e295f
SHA-256: 14cec5eaeab63a40322381f16ed4a9128e85419d6c60e7d0a2ffb81ef4226d87
```

The builder verifies downloaded and cached archives before extraction, uses a
fresh temporary source tree under `build/tinyssh/`, and leaves any old
`references/tinyssh` checkout untouched. The existing fchdir/ttyname/TTY
compatibility patches are preserved, not endorsed as secure privsep fixes.
The pin does not make toolchains or resulting binaries reproducible.

## Exact launch/authentication boundary

Strict Aegis independent children start with **empty capabilities, session
state, and identity binding**. File descriptors do not supply the missing
authority. At this upstream revision:

- `subprocess_sign.c` forks, then changes directory, loads the host secret key
  and writes a signature pipe without exec. The worker has no authority for
  these operations.
- `subprocess_auth.c` forks, then resolves an account, changes credentials and
  opens `authorized_keys` without exec. Fresh exec alone would not authorize
  changing to an unverified identity.
- `channel_forkpty.c` performs terminal setup and writes its readiness pipe
  before exec. The capless worker cannot assume that these operations succeed.
  Non-PTY session launch also changes identity before the final shell exec.

Upstream `packet_auth.c` verifies the public-key signature in the exposed
network parser; its auth worker checks key membership, not the signature proof.
Giving an AUTH helper only a username/key or a caller-claimed "verified" flag
would therefore turn it into an identity-forging service. Moving signature
verification into a helper also needs a trustworthy binding to the live SSH
session: a caller-supplied session ID and recorded valid signature alone can
be replayed.

Aegis syscall 364 (`sys_auth_session`) binds the **calling process** to the
verified uid/gid; independent session children do not inherit that binding.
A secure session design must specify who independently verifies the proof and
live-session provenance, who owns the binding, and how the bound process
becomes the authorized session image. This requires independent security
review, not a launch workaround.

No helper, AUTH/network-issuer grant, inherited authority, credential bypass,
service disable, or fallback is introduced here. The existing `tinysshd`
policy prohibiting AUTH on the exposed parser remains unchanged. Real SSH
handshake, authentication and PTY/non-PTY session validation remain blocked;
a successful server console login boot is **not** SSH validation.

## Source-acquisition regression check

Given a local copy of the archive above:

```sh
bash tools/test-tinyssh-source.sh /path/to/tinyssh-source.tar.gz
```

This host check uses the real checksum and extraction with mocked download and
build commands. It checks the immutable download URL, cache revalidation,
fresh extraction, preservation of compatibility patches, rejection of corrupt
sources before build, and non-use of an old checkout. It does not compile
TinySSH or test helper execution, kernel behavior, or SSH. On macOS the test
also translates the existing Linux builder's GNU sed/stat command-line options
to their BSD equivalents; production compatibility patches remain unchanged.
