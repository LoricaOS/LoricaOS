# LoricaOS Changelog

## 1.1.0 — UNRELEASED (running)

> **Status: not released.** The Claude Fable 5 security review is **done**
> (2026-07-02) — it found no security regressions across the elevation
> boundary, the memfd COW-on-fork fix, or the perf fast paths; the one real
> bug it surfaced (the NVMe low-MDTS regression below) is fixed. What remains
> before shipping is the mechanical publish sequence (version bumps + releases,
> see "Pending" at the bottom). This file is the running record of every
> 1.0.0 → 1.1.0 change; keep appending as work lands.
>
> 1.1.0 started as a GUI/userland depth pass across the Lumen ecosystem
> (compositor, toolkit, apps, packaging). As of 2026-07-01 it ALSO carries a
> new Aegis **kernel** artifact: the global performance pass (see the
> "Performance pass" section below).

### Highlights
- The dock is a real taskbar: window minimize, running/minimized indicators,
  click-to-restore, and macOS-style app-icon + running-dot entries.
- Live window resize everywhere it makes sense: maximize + snap tiling, several
  apps made resizable.
- Deeper apps: a scientific/programmer **Calculator**, a themable **Terminal**
  with live prefs, a **Tunes** music-player redesign, an **Image Viewer** that
  pages through folders and rotates.
- Every app now has a real icon; package apps ship their own.
- **Accounts & sessions**: add users on a running system with `useradd` (home
  created automatically), and the graphical session now opens terminals in your
  home directory instead of `/`.
- **x86 boots via the Limine protocol** now (the kernel's primary path), not the
  legacy multiboot2 fallback.
- **Fixed a rare whole-system freeze** on multicore machines under heavy desktop
  use (an SMP TLB-shootdown deadlock in the AF_UNIX socket path).
- **Global performance pass** (kernel + compositor + installer + boot): NVMe
  moves 128 KiB per command instead of 4 KiB, sequential file reads issue ~16x
  fewer device commands, mmap faults read ahead 16 pages, TCP sends full-size
  segments, app frames no longer repaint the whole desktop, the idle desktop
  no longer wakes 60x/sec, and installs issue ~16x fewer I/O round-trips.
- **FFmpeg ported** as the media engine (toward a native video player): a
  trimmed static build (H.264/HEVC/VP9 + AAC/MP3/…, mp4/mkv/avi) that demuxes
  and thread-decodes H.264 on Aegis, verified bit-exact against the same
  decode on Linux. `make ffsmoke-test` is a permanent boot-and-decode gate.

### Media engine (FFmpeg port)
- `tools/fetch-ffmpeg.sh` + `tools/build-ffmpeg.sh`: FFmpeg 6.1.2 as trimmed
  static libs (avformat/avcodec/swscale/swresample), decode-only, x86 SIMD +
  pthreads on. Cross-parametrized (CC/ARCH/SUFFIX) for the arm64 port later.
- `user/bin/ffsmoke` (+ vigil `ffsmoke` service, `make ffsmoke-iso` /
  `ffsmoke-test`): demux → threaded H.264 decode → swscale to RGB on Aegis.

### Compositor & window management (lumen)
- Window **minimize** + push the live window list to the dock (taskbar protocol).
- Live window **resize** — maximize + snap tiling.
- Clamp windows below the top bar; add an always-on-top layer.
- Wallpapers: load png/jpg/bmp (not just `.raw`); Settings picker switches the
  backdrop live.
- Use the shared `glyph_cursor`; fix the deformed arrow-tail geometry.

### Toolkit (glyph)
- Dock **window-list protocol** + window `minimize`/`gid` fields.
- Window resize protocol (`RESIZE_BUFFER` + `apply_resize`) and
  `LUMEN_WIN_FLAG_ONTOP` / always-on-top flag.
- **Layout geometry module** — `inset` / `grid_cell` / `cut_*` helpers so apps
  stop hardcoding pixel coords.
- `glyph_term`: color schemes, cursor styles, resize/reflow, terminal prefs.
- Shared save-under **arrow cursor** module (`glyph_cursor`).
- Icon policy: glyph keeps procedural icons only for the **core desktop
  launchers**; package apps ship their own `icon.png` (see App icons below).
- Build: auto-generate header dependencies (`-MMD -MP`); relink on toolkit change.

### Dock (citadel-dock)
- Task area shows running/minimized windows with restore-on-click.
- Open windows render as the **app icon + a running dot** (accent = focused,
  dimmed = minimized) — replacing the title-text pills.

### Applications
- **Calculator** — Basic / Scientific / Programmer modes; buttons laid out via
  the glyph layout helpers. *(VERSION already 1.1.0.)*
- **Terminal** — live color/cursor/font/scrollback prefs; resizable window.
- **Editor** — resizable window + reflow to live size (maximize + snap).
- **System Monitor** — resizable window (maximize + snap).
- **File Manager** — resizable window; toolbar laid out via glyph `cut_*` helpers.
- **Settings** — new Terminal pane.
- **Tunes** — redesigned as a proper music-player card: vinyl album art, seek bar
  with knob + elapsed/total, round restart / play-stop transport. Honest UI (no
  fake pause/seek/playlist — libaudio has only play + stop).
- **Image Viewer** — page through the opened image's folder (←/→, Space/Bksp,
  `[i/n]` counter); rotate 90° CW/CCW (`r`/`R`); zoom/pan unchanged.

### App icons (packaging)
- Nine package apps now ship their own icon in the bundle at `/apps/<id>/icon.png`
  (glyph prefers it over procedural art, black keyed out for transparency):
  **2048, snake, minesweeper, calendar, netman, sysmon, imageviewer, tunes, run**.
- The artwork itself: calendar (page + rings), network signal bars, sysmon bar
  chart, framed photo, music note, play triangle, 2048 tile grid, snake body, and
  a spiked minesweeper mine. These live in the packages, **not** in the toolkit,
  so the OS doesn't carry art for apps that may not be installed.

### Admin password management (2026-07-02)
- **Both installers can now set a SEPARATE admin password** during install.
  The CLI installer prompts for it after the account password (Enter keeps
  the sudo-style default of the account password); the GUI installer's User
  screen gained "Admin password (blank = account password)" + confirm fields,
  and the Confirm screen summarizes which mode was chosen.
- **New `adminpw` tool** — change the admin-elevation credential on an
  installed (or live) system. Verifies the CURRENT admin password through
  `login -elevate` (which also elevates the session), then rewrites
  `/etc/aegis/admin` with the new SHA-512 crypt hash. Its caps.d policy is
  admin-tier `AUTH INSTALL`: AUTH because the kernel gates any open of the
  credential file (shadow-grade), INSTALL because the file is
  install-protected. No new kernel surface.
- libinstall: `install_write_credentials` / `install_run_all` take an
  `admin_hash` (NULL/"" = account hash, unchanged behavior).
- Verified end-to-end in QEMU: installed with a separate admin password;
  on the installed disk the account password does NOT elevate and the admin
  one does; `adminpw` rotated the credential; the old admin password is
  rejected and the new one elevates.

### Installer fixes (2026-07-02 — bare-metal testing)
- **GUI installer no longer freezes at the admin-password step.** Root cause
  was a pre-existing kernel bug (present since 1.0.0): a process that `fork()`s
  while holding a `MAP_SHARED` memfd (its Lumen window buffer) had that mapping
  silently COW-broken — the installer's `login -elevate` fork made every frame
  it drew afterward land in a private copy the compositor never saw, so the
  window stuck on the welcome screen (read as a system hang). Fixed in the
  kernel with a distinct `VMM_FLAG_SHARED_OWNED` PTE flag so fork inherits
  refcounted shared frames as-is instead of copying them.
- **CLI installer "ERROR: write /etc/passwd failed" fixed.** The installer had
  `DISK_ADMIN` but not `INSTALL`, and `/etc/aegis/admin` is an install-protected
  path — writing the admin credential hit EPERM. Both installer cap policies now
  include `INSTALL`.
- **CLI installer skips the admin prompt when already elevated** (launched from
  a red `[admin]` shell) — probes disk access first and only runs
  `login -elevate` if needed.
- Kernel now logs `[SIGNAL] pid=N <exe> killed by signal S` on abnormal
  termination (silent kills were undiagnosable).
- Verified end-to-end on CT117: both installers complete a real NVMe write and
  the installed disk boots to the greeter/login under OVMF.

### Security review follow-ups (2026-07-02 — Fable 5)
- **NVMe install fixed on low-MDTS controllers (kernel, aegis `3819f8b`).** The
  performance pass raised whole-disk copies and ext2 run reads to 64 KiB
  transfers, but the driver rejects any command over the controller's MDTS
  limit. On a controller whose MDTS clamps below 64 KiB (or the single-page
  bounce fallback), the install died with `EIO` — a regression from the perf
  pass (the old 4 KiB transfers always fit). `nvme_blkdev_read/write` now split
  large transfers into per-command chunks internally, so no block caller needs
  to know MDTS. Fails safe either way (clean error, never corruption).
- **Installers now zero password buffers.** The CLI and GUI installers left
  plaintext account/admin passwords on the stack / in wizard state after use;
  both now wipe them once hashed/consumed (matching `adminpw`). Low severity —
  single-user install-time tools with no exfiltration path — but tidied for a
  security-branded release.

### Kernel: fd access-mode enforcement (2026-07-04 — found by the FFmpeg port)
- **`sys_write` now enforces the fd's access mode (aegis).** A read-only fd
  on a regular file was writable — the mode was checked only at `open()`
  (capability-gated) and never at the write. Surfaced when a service's media
  file, opened O_RDONLY, landed on the freed stdout fd and got overwritten by
  a stdio flush. `sys_read`/`readv`/`pread64` reject O_WRONLY fds and
  `sys_write`/`writev` reject O_RDONLY fds (`VFS_O_ACCMODE`); every
  fd-creation site sets an honest mode (pipe ends, sockets, memfds, init's
  0/1/2). No ambient authority: an fd carries exactly the direction it was
  opened with. Fails closed (`EBADF`).
- **vigil reopens stdout to `/dev/null`** instead of `close(1)` in quiet mode,
  so a service's next `open()` can't inherit fd 1 (defense in depth alongside
  the kernel fix).

### Stability (2026-07-02 — graphical soak)
- **Fixed a whole-system SMP deadlock under desktop load (kernel, aegis
  `bfc21fe`).** `unix_socket.c` freed socket ring buffers with `kva_free_pages`
  — which the performance pass made issue a synchronous cross-CPU TLB shootdown
  — *while holding `unix_lock` with interrupts disabled*. On a multicore
  machine this deadlocks: the freeing CPU spins (IRQs off) waiting for every
  core to ACK the shootdown IPI, while another core spins for `unix_lock` in
  `unix_sock_read` (also IRQs off) and so can never ACK it. The whole machine
  wedges — no panic, no log. It needs several cores **plus** the compositor's
  AF_UNIX client churn **plus** concurrent address-space frees to trigger, so
  it was invisible to single-CPU tests and only bit multicore bare metal under
  GUI load. Found and fixed via a new graphical **soak harness** (`stresstest`
  + a `soak` ISO: 12 GB, 4 CPUs, 10 min of GUI-app/memfd/fork/socket churn),
  which reproduced it deterministically (hard hang <10s) and then ran clean for
  the full 10 minutes with the fix. All four ring-free sites now drop
  `unix_lock` before freeing.

### Boot protocol & user accounts (2026-07-02 — bare-metal testing)
- **x86 now boots via the Limine protocol** (`tools/gen-limine-conf.sh`:
  `protocol: multiboot2` → `protocol: limine` on every entry). The kernel
  switched its primary boot path to the Limine protocol, and its own smoke ISOs
  use it, but the OS boot config had never been updated — so bare metal was
  still entering through the legacy 32-bit multiboot2 header. The OS now boots
  on the kernel's primary, tested path (modules + framebuffer come through the
  Limine requests). The multiboot2 header stays as the microvm/GRUB fallback.
  Unblocks the higher-half direct map (HHDM) the kernel already requests.
- **Graphical sessions start in the user's home.** The Bastion greeter set
  `$HOME` but never `chdir`'d to it (unlike the text-console `login`), so the
  whole GUI session — and every terminal Lumen launched — inherited init's cwd
  of `/`. Bastion now `chdir`s to the authenticated user's home, so terminals
  and file pickers open in `$HOME`.
- **New `/bin/useradd`** — create additional login accounts on a running
  system: `useradd [-m|-M] [-s <shell>] <username>`. Assigns the next free uid
  at/above 1000 in a private group, prompts for a password (crypt SHA-512),
  appends `/etc/passwd`/`/etc/shadow`/`/etc/group`, and creates the home by
  default. Being a privileged action with no ambient root, it first demands the
  admin credential via `login -elevate` (the caps it needs — `AUTH` to write
  shadow, `SETUID` to chown the home — are granted to any authenticated
  session, so the elevation check is the real gate). caps.d: `admin AUTH SETUID`.
- **Home directory always created with the account.** Home creation is now one
  shared `install_make_home()` in libinstall, used by both the installer (the
  primary uid-0 user) and `useradd` — so any account creation makes its home.

### Beautification pass (2026-07-02 — glyph + lumen + apps)
- **Anti-aliased rendering primitives** (glyph): filled circles, rounded-rect
  fills, translucent rounded rects, and rounded outlines now draw with
  per-pixel edge coverage — every button, card, pill, toggle and highlight in
  the OS loses its jagged corners at once. `draw_rounded_outline` also went
  from O(w·h) to O(perimeter).
- **Traffic lights redrawn** — new shared `draw_traffic_light()` widget: smooth
  AA circles with a darker rim, and engraved ×/−/zoom symbols on the focused
  window (the old ones were rough Bresenham circles with a text "x").
- **Window shadows reworked** — the 5-layer stacked shadow (visible banding)
  is now a per-pixel penumbra falling off quadratically with distance to the
  window, with naturally rounded corners. Cheaper as well: only the visible
  ring outside the window body is computed.
- **Theme recolor: "obsidian".** The dark palette moved from blue-slate to
  near-black neutrals with a violet undertone; the default accent is now
  indigo (accent picker unchanged — blue is still available). Compositor
  frost tints retuned to match. Light palette and the traffic-light/status
  colors untouched.
- **Applications menu**: keyboard selection is an accent-tinted pill with a
  1px accent ring instead of a flat white wash; hover is subtler.
- **Greeter cleaned up (bastion)**: the v1 disclaimer and the network-status
  line are gone (the warning lives in About and the terminal banner; the net
  line is now diagnostics-only under `greeter_diag`, like the input counters).
  Desktop-gradient background, alpha-blended logo, rounded input wells with an
  accent focus ring + caret, rounded button with centered text.
- **Topbar context menu**: accent rounded hover pill with on-accent text, and
  the rounded corners now restore the real wallpaper instead of stamping flat
  theme-color pixels (visible dots on photo wallpapers).
- All 16 desktop components rebuilt against the new toolkit and re-pinned in
  the component lockfile (`lumen-calculator` pin corrected to its 1.1.0
  artifact — the list still pointed at the stale 1.0.0 package).

### Performance pass (2026-07-01 — kernel + OS)

**Kernel — storage (aegis commit `fe028c4`).** The three storage changes
compound: an mmap fault used to mean one trap + one 4 KiB ext2 read + one
4 KiB NVMe command *per page*; it now means 1/16th the traps, each satisfied
by one large read that reaches the device as one command.
- **NVMe multi-page PRP transfers** — the driver had a single 4 KiB bounce
  page and rejected anything larger, so a 128 KiB read was 32 fully
  serialized submit+busy-poll round-trips. The bounce is now a 32-page
  (128 KiB) set described to the controller via a PRP list, clamped to the
  controller's advertised MDTS; the failed-command quarantine reallocates the
  whole set. The blkdev syscall bounce grew 4 KiB → 64 KiB to match.
- **ext2 contiguous-run reads** — `ext2_read` batches runs of blocks that are
  consecutive on disk and *not in the block cache* into ONE direct device
  read into the caller's buffer (cached/dirty blocks stay authoritative; bulk
  data no longer evicts the hot indirect/bitmap blocks from the 16-slot
  cache). Measured with the in-kernel perfbench: the read pass over a 464-
  block file dropped from ~1 device read per block to **30 device commands**,
  content VERIFY OK.
- **mmap fault readahead** — file-backed faults cluster-populate up to 16
  pages through one generation-validated ext2 read (per-CPU fault buffer
  grew to 16 pages). Sequential mmap reads take ~1/16th the page faults and
  device commands — this was the known ext2/mmap perf wall (Lantern/ports).

**Kernel — everything else (same commit).**
- **TCP send MSS** — we advertised MSS 1460 but chunked our own sends to the
  RFC-879 default 536. The peer's MSS option is now parsed from its SYN and
  outbound data segments to it (1460 typical, clamped, 536 only when the peer
  offers none): ~3x fewer packets/checksums for uploads and served responses.
- **Batched TLB shootdown** — freeing N kernel pages used to broadcast one
  shootdown IPI (+ ack spin) per page; `kva_free_pages`/`kva_unmap_keep_frames`
  now clear all PTEs and issue ONE ranged shootdown (task teardown alone was
  ~14 IPI rounds per exit).
- **FPU context-switch skip** — XSAVE/XRSTOR of the 1 KiB FPU area ran on
  every switch; it is now skipped for kernel tasks on both the save and
  restore side (kernel is -mno-sse and never owns live FPU state).
- **Word-wide copies** — `kmemcpy`/`kmemset`/`kmemcmp` (byte loops used at
  ~70 call sites across net/fs/drivers) and the arm64 user-copy path
  (`uaccess.S`, byte-at-a-time on EVERY syscall transfer) now move 8 bytes
  per iteration; the arm64 word ops carry exception-table entries like the
  byte ops did.
- **Write staging** — `sys_write`/`sys_writev` staged user data in 256-byte
  chunks (a 4 KiB write = 16 `ops->write` calls, each redoing fd cache
  lookups); staging is now one page.

**Compositor (lumen `d7dcb98`).**
- **Client frames stopped defeating damage tracking** — `handle_damage` set
  `full_redraw` on EVERY client frame, so each terminal keystroke / sysmon
  tick refilled the wallpaper and re-blurred every frosted window. Only the
  first present (window reveal) forces a full recomposite now; routine frames
  ride the existing dirty-rect path and reuse every other window's cached
  blur. This was the single biggest lever in the graphical stack.
- **Event-driven idle loop** — the main loop woke 60x/sec (read/waitpid/poll
  syscalls + 16 ms nanosleep) even on an idle desktop. It now blocks in
  `poll()` on stdin, the mouse, the server socket, every client fd (new
  `lumen_server_collect_fds()`), and every PTY master, with a 250 ms cap for
  the clock/prefs check: idle wakeups 60/s → 4/s, and input wakes the loop
  instantly instead of mid-sleep. The once-a-second clock/prefs refresh is
  keyed to wall-clock seconds instead of an iteration count.
- **Installer:** 64 KiB transfer chunks (was 4 KiB) through the enlarged
  kernel blkdev bounce — the 4-pass copy+verify install issues ~16x fewer
  syscall→NVMe round-trips.
- **Boot:** vigil now honors an optional per-service `cmdline` token file and
  skips gated services without spawning them — the 8 stress/diagnostic
  services (smpstress, futexstress, mmfaultstress, vforkstress, forkbench,
  perfbench-ipc, dltest, selftest) no longer cost a fork+exec+ELF-load each
  on every production boot (they self-gated to a no-op before). vigil also
  reaps crashed services immediately (SIGCHLD wakes the supervisor) instead
  of up to 1 s late.
- **Slimming:** startup.mp3 + sample.bmp moved to the desktop skeleton
  (server image drops ~150 KB); unused Inter-Regular.otf removed from the
  repo (only the .ttf was ever packaged).
- Gates: ostest (desktop→greeter), servertest, selftest (CAPTEST 10/10 —
  proving the cmdline gate still fires with the token present), x86+arm64
  kernel suites, perfbench VERIFY.
- Deferred (surveyed, not built): HHDM physical direct-map for the VMM (the
  remaining structural win — removes the locked map-window + invlpg pairs
  from every fault/fork; big vmm.c rewrite), ext2 dentry/name cache, e1000
  RX IRQ (virtio-net already has one), scoping full_redraw out of window
  raise/open-anim paths.

---

### Pending before release
- [x] **Claude Fable 5 code review** — security pass DONE (2026-07-02); no
      regressions found. Fixes folded in: NVMe low-MDTS chunking + installer
      password zeroing (see "Security review follow-ups" above).
- [ ] Bump **glyph → 1.1.0** and cut its toolkit release.
- [ ] Bump `GLYPH_VERSION → 1.1.0` on components that use new glyph APIs:
      citadel-dock, lumen-calculator, lumen-filemanager, lumen-editor,
      lumen-sysmon, lumen-terminal. *(applications-menu does NOT need it — it
      gets the new icons via each package's `icon.png`, a runtime file, not an
      API.)*
- [ ] Bump `VERSION → 1.1.0` on the deepened components (calculator done).
      Icon-only packages (2048/snake/minesweeper/calendar/netman/run) can go
      1.0.1 or fold into 1.1.0 — decide at publish.
- [ ] Re-cut + publish the herald `.hpkg`s to Chancery (green-light gated;
      scrub the signing key after).
- [ ] **Publish the new Aegis kernel release** (perf pass `fe028c4` + memfd
      fork fix `23f2f35` + NVMe low-MDTS fix `3819f8b` + AF_UNIX SMP
      deadlock fix `bfc21fe`) and bump `KERNEL_VERSION` — fetch-kernel.sh still pins the pre-pass 1.0.0
      artifact; until the release is cut, builds only get the new kernel via
      the local vendor/ cache.
