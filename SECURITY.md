# Security Policy

LoricaOS is a capability-secure, POSIX-compatible operating system built on the
[Aegis kernel](https://github.com/LoricaOS/aegis). Its security posture is inherited
from Aegis and extended by the userland — init, shell, login, authentication, admin,
and the capability policy in `/etc/aegis/caps.d/`.

## The model

- **No ambient authority.** No process, including one running as `uid=0`, holds power
  it was not explicitly granted. `uid=0` is cosmetic; authority comes only from
  unforgeable capability tokens validated by the kernel. See the Aegis
  [SECURITY.md](https://github.com/LoricaOS/aegis/blob/main/SECURITY.md) for the kernel
  model.
- **No root.** The live user is `uid=0`, but that number grants nothing on its own.
  Privileged actions require a capability, and admin actions require an authenticated
  admin session (the sudo-style `admin` flow / `login -elevate`), not merely being
  logged in.
- **A small trusted base.** The trusted userland is deliberately minimal — init
  (`vigil`), shell (`stsh`), login/auth/admin, and net. The unprivileged coreutils are
  peeled into a separate package and carry no authority; that split *is* the
  trusted-base / TCB line.

## Current maturity — please read this

**LoricaOS is v1 software and is NOT production-hardened. Do not use it to protect real
secrets or isolate untrusted code yet.** The kernel TCB is young C and very likely
contains memory-safety bugs that could defeat the capability model from below (see the
Aegis SECURITY.md). The design is sound; the implementation has not been audited to a
production standard.

## Reporting a vulnerability

Please report privately — do not open a public issue for an unfixed vulnerability.

- Preferred: a GitHub private security advisory.
- Or email: **execxd@icloud.com**

Where to report:
- **Kernel-level issues and capability bypasses** → the [Aegis](https://github.com/LoricaOS/aegis)
  repository.
- **Userland issues** (login, authentication, admin/elevation, capability policy in
  `/etc/aegis/caps.d/`, the installer) → this repository.

Include the version or commit and a proof-of-concept if you have one. There is no
bug-bounty program; fixes are credited in the release notes unless you prefer anonymity.

## Supported versions

Only the latest release and the `main` branch are supported.
