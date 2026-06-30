# CLAUDE.md — LoricaOS

Guidance for working in this repo. Read it before making changes.

## What this repo is

LoricaOS is the **operating system** — userland, root filesystem, and boot
images — built on the from-scratch [Aegis](https://github.com/LoricaOS/Aegis)
kernel. It does **not** build the kernel or the GUI apps; it **fetches them as
versioned artifacts** and assembles bootable ISOs:

- `tools/fetch-kernel.sh` → the `aegis.elf` artifact (pinned in `KERNEL_VERSION`).
- `tools/fetch-components.sh` + `tools/components.list` → the desktop GUI
  component `.hpkg` packages, each from its own `LoricaOS/<id>` repo's release.
- `tools/assemble-desktop-rootfs.sh` → unpacks the components into the desktop
  rootfs and seeds the herald package DB.
- `tools/fetch-limine.sh` → the pinned Limine bootloader for the ISO.

In-tree here: the init (`user/bin/vigil`), the installers
(`user/bin/{installer,gui-installer}`), coreutils/net tooling, and the rootfs
skeletons (`rootfs/`, `rootfs-desktop/`).

## Two profiles

```
make desktop-iso   # build/loricaos-desktop.iso  — graphical (Lumen desktop)
make server-iso    # build/loricaos-server.iso    — text console, zero graphical
```

`rootfs/` is the shared base; `rootfs-desktop/` overlays the graphical profile.

## Branding: LoricaOS is the OS, Aegis is the kernel

A rebrand moved OS-level identity to **LoricaOS** (one word). Keep "Aegis" ONLY
where it genuinely names the kernel:

- **Keep:** `aegis.elf`, the `/etc/aegis/caps.d/*` capability paths, "the Aegis
  kernel", links to `LoricaOS/Aegis`.
- **Rebrand to LoricaOS:** anything user-facing — installer text, banners/motd,
  hostnames, window titles, app strings, ISO filenames.

When unsure whether a string is a kernel reference, it probably isn't — default
to LoricaOS for user-visible text.

## The capability model carries up from the kernel

The kernel grants no ambient authority. A program touches the system only
through capabilities declared in `/etc/aegis/caps.d/<exec>`. When adding or
packaging a program, give it the **minimum** policy: `service` for a pure GUI
client, add tokens like `NET_SOCKET` only when it genuinely needs them. Never
hand out `admin`/`SETUID`/`DISK_ADMIN` to anything that doesn't require it.

## GUI apps live in their own repos

Each Lumen desktop app is its own `LoricaOS/lumen-*` repo that publishes a
`class=system` herald `.hpkg`; this repo only lists and assembles them
(`tools/components.list`). To add an app to the **default desktop image**, add
its `<herald-id> <version>` line to `components.list`. Apps not in that list are
still installable on demand via herald once they're on the Chancery repo — use
this for optional extras (e.g. games) rather than bloating the base image.

## Conventions

- Match the surrounding code. Prefer general fixes over per-caller patches.
- Build/test produces real ISOs — validate a change by booting the resulting
  ISO (greeter for desktop, login for server), not just by compiling.
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
