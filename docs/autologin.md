# Automatic login

LoricaOS boots to the **bastion** greeter (login screen) by default. A user who
wants their machine to skip the greeter and start a desktop session
automatically can opt into **autologin**.

## The mechanism

Autologin is driven by a single file:

```
/etc/aegis/autologin
```

It contains one line: the **username** to log in automatically, with no
password. When bastion starts and finds this file, it establishes that user's
session directly and launches the compositor — no greeter, no password prompt.

Bastion holds the `AUTH`/`SETUID` capabilities, so it binds the identity itself;
the file names *who*, not a credential. If the named user isn't in
`/etc/passwd`, bastion falls back to the normal greeter.

## Enabling it (installed systems)

The supported way is **Settings → Users → Automatic Login** (admin/root only),
which writes `/etc/aegis/autologin` for you. Manually:

```sh
echo alice > /etc/aegis/autologin      # as admin/root
```

Remove the file (or use Settings) to go back to the greeter.

> **Security note:** autologin means *anyone with physical access* boots
> straight into that user's session with no password. It is off by default and
> should stay off on shared or portable machines.

## Installed systems always start at the greeter

A fresh install **never inherits** autologin, even if the media it was installed
from had it enabled:

- Installation is a raw block copy of the live rootfs, so any
  `/etc/aegis/autologin` on the media is copied to disk too.
- On the **first installed boot**, `vigil` (init) strips it — the same
  first-boot cleanup that removes the live installer binaries. See
  `remove_installers_if_installed()` in `user/bin/vigil/main.c`.
- This runs *only* on that first boot (gated on the installer binaries still
  being present), so autologin a user enables **later** via Settings persists
  normally. We only drop what the live media carried in.

So: **live media may autologin; an installed system forces the greeter until the
owner opts in.**

## Developer / boot-profiling ISO

`make desktop-dev-iso` builds a **non-shipped** desktop ISO for boot profiling:

- bakes `/etc/aegis/autologin=live` into its rootfs (`AUTOLOGIN=live` →
  `tools/assemble-desktop-rootfs.sh`), so it drives all the way to the desktop
  unattended, and
- boots **non-quiet** (`dev` mode in `tools/gen-limine-conf.sh`) so vigil's
  `[UBOOT] <ms> <phase>` timeline and service logs reach the serial console.

It is otherwise identical to the shipped desktop ISO, and installing from it
still strips the autologin (above) — so it changes nothing about production
systems. The shipped `make desktop-iso` never sets `AUTOLOGIN` and stays quiet
with the greeter.
