# LoricaOS Changelog

## 1.1.0 — UNRELEASED (running)

> **Status: not released.** Held for a Claude Fable 5 code-review pass
> (re-released 2026-07-01) — expect additional revisions, likely including deep
> security fixes, before this ships. This file is the running record of every
> 1.0.0 → 1.1.0 change; keep appending as work lands.
>
> The Aegis **kernel is unchanged** in this release — 1.1.0 is a GUI/userland
> depth pass across the Lumen ecosystem (compositor, toolkit, apps, packaging).

### Highlights
- The dock is a real taskbar: window minimize, running/minimized indicators,
  click-to-restore, and macOS-style app-icon + running-dot entries.
- Live window resize everywhere it makes sense: maximize + snap tiling, several
  apps made resizable.
- Deeper apps: a scientific/programmer **Calculator**, a themable **Terminal**
  with live prefs, a **Tunes** music-player redesign, an **Image Viewer** that
  pages through folders and rotates.
- Every app now has a real icon; package apps ship their own.

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

### Performance pass (2026-07-01 — kernel + OS)
> Note: this supersedes the "kernel is unchanged" line above — 1.1.0 now
> carries a new kernel artifact with the storage/net/mm fast paths below.
- **Kernel (aegis, commit `fe028c4`):** NVMe multi-page PRP transfers (one
  command moves up to 128 KiB instead of a serialized 4 KiB round-trip per
  page, clamped to controller MDTS); ext2 batches contiguous uncached blocks
  into single device reads (perfbench: read-pass device commands 464 → 30,
  VERIFY OK); file-mmap faults cluster-populate 16 pages per trap through one
  generation-validated read; TCP segments outbound data to the peer's real
  MSS (1460 typical) instead of 536; ranged (not per-page) cross-CPU TLB
  shootdown on kernel-page frees; FPU save/restore skipped for kernel tasks;
  word-wide kmem*/arm64-uaccess; 4 KiB (was 256 B) syscall write staging.
- **Compositor (lumen `d7dcb98`):** client frames no longer force a
  full-screen recomposite + re-blur of every frosted window — only the first
  present (reveal) does; routine frames ride the dirty-rect path and reuse
  cached blurs. Idle loop now blocks in `poll()` on all input fds instead of
  a 16 ms sleep-poll cycle (idle wakeups 60/s → 4/s, input wakes instantly).
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
- [ ] **Claude Fable 5 code review** — security pass; fold in resulting fixes here.
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
