# Contributing to LoricaOS

LoricaOS is a capability-secure, POSIX-compatible operating system built on the
[Aegis kernel](https://github.com/LoricaOS/aegis). This repository is the **OS**: the
userland, the GUI (lumen compositor + glyph toolkit + apps), the rootfs, and the ISO.
It does **not** build the kernel — it fetches a released, versioned `aegis.elf` via
`tools/fetch-kernel.sh`.

Thanks for helping out. Please read this before opening a PR.

## Security first

LoricaOS inherits Aegis's **no ambient authority** model (see [SECURITY.md](SECURITY.md)).
No process, including `uid=0`, holds power it wasn't granted, and there is no root. Any
change to the trusted base (init/shell/login/auth/admin/net) or the capability policy
(`/etc/aegis/caps.d/`) must not introduce ambient authority. Grant the minimum, and fail
closed.

## Build & test

You need a static-musl toolchain, `xorriso`, and Limine. The kernel comes from a
release — you do not build it here:

```
tools/fetch-kernel.sh      # download the pinned aegis.elf
make                       # build the userland + assemble the rootfs
make desktop-iso           # build/loricaos-desktop.iso  (graphical)
make server-iso            # build/loricaos-server.iso   (text console, no GUI)
make test                  # ostest + servertest
```

## How it fits together

- **Base rootfs** — the trusted userland: init (`vigil`), shell (`stsh`),
  login/auth/admin, net.
- **coreutils** — the unprivileged utilities, kept in a separate repo and fetched as a
  package. They carry no capabilities; this split is the TCB line.
- **GUI** — `lumen` (compositor) and `glyph` (toolkit), plus the desktop apps — each its
  own repo/package.
- **Profiles** — `desktop` (graphical) and `server` (text console); the ISO is built per
  profile.

## House style

- Match the surrounding code, and keep the trusted base small — no new abstractions
  without a second caller.
- Every binary declares the capabilities it needs in `/etc/aegis/caps.d/<name>`. Request
  the least authority that works.
- Kernel identifiers stay "Aegis"; the OS is "LoricaOS".

## Pull requests

- One logical change per PR; explain the *why*.
- Make `make test` (ostest + servertest) pass.
- Sign off your commits with a real name and email.

By contributing, you agree that your contributions are licensed under the project's
[MIT License](LICENSE).
