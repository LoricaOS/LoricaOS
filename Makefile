# AspisOS — the operating system built on the Aegis kernel.
# Builds the userland, root filesystem, and bootable ISO. The kernel itself is
# NOT built here: it is fetched as a versioned artifact (see KERNEL_VERSION /
# tools/fetch-kernel.sh). OS and kernel versions are independent.

# ── Versions ────────────────────────────────────────────────────────────────
# This OS release. Stamped into the rootfs (motd, About) via build-rootfs.sh.
AEGIS_OS_VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
# The Aegis kernel version this OS build runs on (fetched, not built).
KERNEL_VERSION   := $(shell cat KERNEL_VERSION 2>/dev/null || echo 0.0.0)
# build-rootfs.sh reads AEGIS_VERSION for the motd/About strings.
AEGIS_VERSION    := $(AEGIS_OS_VERSION)
export AEGIS_VERSION

BUILD  = build
# Two production profiles share one source tree (Phase 1 of the graphical peel):
#   desktop — full graphical stack (rootfs + rootfs-desktop skeletons/manifests)
#   server  — no graphical anything (rootfs/rootfs.manifest only)
ROOTFS_DESKTOP = $(BUILD)/rootfs-desktop.img
ROOTFS_SERVER  = $(BUILD)/rootfs-server.img

# C++ cross toolchain (used by build/cpptest fixture; provisioned out-of-repo)
CXX_AEGIS       = /opt/aegis-cxx/bin/x86_64-buildroot-linux-musl-g++
CXX_FLAGS_AEGIS = -static -O2 -std=c++23 -fno-pie -no-pie

.PHONY: all iso desktop-iso server-iso selftest-iso rootfs build-musl test clean version curl_bin
all: iso

# ── Kernel artifact: fetched, not built ─────────────────────────────────────
# fetch-kernel.sh resolves vendor/aegis-<ver>.elf (local cache) or downloads the
# release for KERNEL_VERSION. KERNEL_STRIPPED is the (already-stripped) kernel
# the ESP image and ISO embed — same variable name the OS rules below expect.
KERNEL_STRIPPED = $(BUILD)/aegis-stripped.elf
$(KERNEL_STRIPPED):
	bash tools/fetch-kernel.sh $(KERNEL_VERSION) $@

# ── User program builds ─��──────────────────────────────────���────────────────
# musl shared library (dynamic linker)
MUSL_BUILT = build/musl-dynamic/usr/lib/libc.so

build/musl-dynamic/usr/lib/libc.so:
	bash tools/build-musl.sh

build-musl: $(MUSL_BUILT)

# Simple user programs: depend only on musl, built by their own Makefile.
# Add new programs here — that's it. No other list to update.
SIMPLE_USER_PROGS = \
    ls cat echo pwd uname clear true false wc grep sort \
    mkdir rmdir touch rm cp mv whoami ln chmod chown readlink \
    shutdown reboot aegisctl login stsh httpd sshd nettest polltest poll-test sockreftest contresume spawnleak \
    sleep head tail basename dirname tee env date hostname sync \
    tr cut expand realpath stat yes find which uniq \
    ps kill free uptime du pgrep pkill xargs more diff dmesg df ip \
    smpstress futexstress mmfaultstress elffuzz sysfuzz fduaf blkuaf extabtest vforkstress dltest captest cowtest \
    perfbench-ipc forkbench selftest

# Generate rules: user/bin/foo/foo.elf depends on musl AND its own sources,
# so editing any .c/.h under user/bin/foo triggers a rebuild.  Without the
# wildcard dep, a stale .elf gets silently packed into the rootfs.
define SIMPLE_USER_RULE
user/bin/$(1)/$(1).elf: $$(MUSL_BUILT) $$(wildcard user/bin/$(1)/*.c) $$(wildcard user/bin/$(1)/*.h) user/bin/$(1)/Makefile
	$$(MAKE) -C user/bin/$(1)
endef
$(foreach p,$(SIMPLE_USER_PROGS),$(eval $(call SIMPLE_USER_RULE,$(p))))

# Programs with non-.elf output names
user/bin/vigil/vigil: user/bin/vigil/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/vigil

user/bin/vigictl/vigictl: user/bin/vigictl/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/vigictl

user/bin/dhcp/dhcp: user/bin/dhcp/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/dhcp

user/bin/chronos/chronos: user/bin/chronos/main.c $(MUSL_BUILT)
	$(MAKE) -C user/bin/chronos

# TinySSH (tinysshd) — built static with the HOST musl toolchain via its own
# build (compile-and-run feature detection needs host-native binaries). The
# makekey/printkey outputs are real copies (rootfs copies files, not symlinks).
build/tinyssh/tinysshd build/tinyssh/tinysshd-makekey: tools/build-tinyssh.sh
	bash tools/build-tinyssh.sh

# herald — package manager. Links BearSSL (already built for curl) and embeds
# the PRODUCTION trust anchor. tools/herald-keys/trusted_key.h is committed
# (public point only) and pins the anchor permanently; keygen copies it into
# build/ so `make clean`/`git clean` can never mint a new anchor and orphan the
# deployed repo. Only falls back to a generated dev key if the committed anchor
# is absent. (The matching private key is gitignored, kept on the repo host.)
build/herald-keys/trusted_key.h: tools/herald-keygen.sh tools/herald-keys/trusted_key.h
	bash tools/herald-keygen.sh

user/bin/herald/herald.elf: $(wildcard user/bin/herald/*.c) $(wildcard user/bin/herald/*.h) user/bin/herald/Makefile build/herald-keys/trusted_key.h build/bearssl-install/lib/libbearssl.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/herald

# Signed sample package "hello" — demo + herald_test fixture, signed with the
# production key whose public half is embedded in /bin/herald. The payload is
# deliberately 300 KB: it exceeds the 12 direct + 256 single-indirect blocks
# (at 1 KiB blocks, ~274 KB) so installing it exercises ext2 double-indirect
# block-write allocation end-to-end.
#
# Skipped cleanly when the PRIVATE signing key is absent (CI / a fresh clone:
# only the public anchor is committed). It can't be prod-signed without the key,
# and a dev-signed copy wouldn't verify against the embedded prod anchor — so
# rather than ship a broken fixture, the keyless build omits it. build-rootfs.sh
# already skips manifest sources that don't exist, so the ISO still builds; the
# fixture is only needed by herald_test, which runs where the key lives.
build/herald-sample.hpkg: tools/herald-pack.sh tools/herald-keygen.sh build/herald-keys/trusted_key.h
	@if [ ! -f build/herald-keys/herald-dev.key ]; then \
	    echo "[herald-sample] no signing key — skipping demo fixture (keyless/CI build)"; \
	else \
	    rm -rf build/herald-sample; \
	    mkdir -p build/herald-sample/apps/hello; \
	    printf 'id=hello\nname=Hello\nversion=1.0.0\nexec=hello\n' > build/herald-sample/manifest; \
	    yes AEGIS | head -c 300000 > build/herald-sample/apps/hello/hello; \
	    printf 'name=Hello\nexec=hello\n' > build/herald-sample/apps/hello/app.ini; \
	    bash tools/herald-pack.sh build/herald-sample build/herald-sample.hpkg; \
	fi
# .sig is produced as a side effect of packing the .hpkg (no separate recipe).
build/herald-sample.hpkg.sig: build/herald-sample.hpkg

# `test` produces TWO binaries — test.elf and bracket.elf (cp of test.elf).
# Listed separately because SIMPLE_USER_RULE only handles one .elf per dir.
user/bin/test/test.elf user/bin/test/bracket.elf: $(wildcard user/bin/test/*.c) user/bin/test/Makefile $(MUSL_BUILT)
	$(MAKE) -C user/bin/test

# cpptest — C++ runtime smoke test (browser-port P1 gate). Built with the
# Aegis C++ cross toolchain; proves exceptions/RTTI/STL/iostream run on Aegis.
build/cpptest: user/bin/cpptest/main.cpp tools/setup-cxx-toolchain.sh
	sh tools/setup-cxx-toolchain.sh
	$(CXX_AEGIS) $(CXX_FLAGS_AEGIS) -o $@ $<

# Static binary (no musl)
user/bin/shell/shell.elf:
	$(MAKE) -C user/bin/shell

# Libraries
user/lib/glyph/libglyph.a: $(wildcard user/lib/glyph/*.c user/lib/glyph/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/lib/glyph

user/lib/libauth/libauth.a: user/lib/libauth/auth.c user/lib/libauth/auth.h
	$(MAKE) -C user/lib/libauth

user/lib/libinstall/libinstall.a: $(wildcard user/lib/libinstall/*.c user/lib/libinstall/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/lib/libinstall

# Programs with extra library dependencies. The graphical stack (lumen, bastion,
# citadel-dock, applications, and the GUI apps) is no longer built here — it is
# fetched as herald packages and assembled into the desktop rootfs (see
# tools/fetch-components.sh + tools/assemble-desktop-rootfs.sh). Only the
# installer + gui-installer remain in-tree; gui-installer is the live-only
# graphical installer and the lone remaining consumer of libglyph.
user/bin/installer/installer.elf: user/bin/installer/main.c user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/installer

user/bin/gui-installer/gui-installer.elf: user/bin/gui-installer/main.c user/lib/glyph/libglyph.a user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/gui-installer

# BearSSL + curl (external builds)
build/bearssl-install/lib/libbearssl.a:
	bash tools/build-bearssl.sh

build/curl/curl: build/bearssl-install/lib/libbearssl.a
	bash tools/build-curl.sh || (mkdir -p build/curl && echo '#!/bin/sh' > $@ && chmod +x $@ && echo "[curl] build failed — using stub")

curl_bin: build/curl/curl

# Rune text editor (external Rust build)
user/bin/rune:
	bash tools/build-rune.sh

# ── Limine bootloader: host tool + installer ESP image ──────────────────
LIMINE_DIR = tools/limine
LIMINE_BIN = $(BUILD)/limine

# Limine binaries are fetched (pinned in $(LIMINE_DIR)/VERSION), not vendored.
# The stamp runs the fetch once; every limine file depends on it, so the ESP +
# ISO rules pull it in transitively (they already list limine files as prereqs).
LIMINE_STAMP = $(LIMINE_DIR)/.fetched
$(LIMINE_STAMP): tools/fetch-limine.sh $(LIMINE_DIR)/VERSION
	sh tools/fetch-limine.sh
	@touch $@
$(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine-bios-hdd.h $(LIMINE_DIR)/limine-bios.sys \
$(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin \
$(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI: $(LIMINE_STAMP)
ESP_DESKTOP = $(BUILD)/esp-desktop.img
ESP_SERVER  = $(BUILD)/esp-server.img
HOSTCC    ?= cc

# Build Limine's host tool from the vendored source (used by `limine bios-install`).
$(LIMINE_BIN): $(LIMINE_DIR)/limine.c $(LIMINE_DIR)/limine-bios-hdd.h
	@mkdir -p $(BUILD)
	$(HOSTCC) -std=c99 -O2 -I$(LIMINE_DIR) -o $@ $(LIMINE_DIR)/limine.c

# Installed-system ESP: 4 MiB FAT16 carrying Limine's UEFI binaries, the
# generated installed-mode limine.conf, and a copy of the kernel. Limine reads
# the kernel from this FAT ESP (boot():/boot/aegis.elf) — reliable, unlike its
# ext2 driver on our rootfs (1 KiB blocks / dir_index). The installer
# raw-copies this image to the target disk's ESP partition unchanged
# (user/lib/libinstall/copy.c); the kernel then mounts the real ext2 root from
# nvme itself.
# Size (8192 x 512B sectors = 4 MiB) MUST stay in lockstep with
# ESP_SIZE_BYTES in user/lib/libinstall/libinstall.h — the installer's GPT
# layout and raw copy both derive from that constant. Content is ~1.3 MiB
# (BOOTX64.EFI + stripped kernel + limine.conf); 4 MiB leaves headroom and
# clears the FAT16 minimum-cluster floor.
# $(1)=ESP image  $(2)=gen-limine installed-mode (installed | server-installed).
# The installed-boot menu differs by profile: desktop defaults to graphical,
# server is text-only (no compositor to boot into).
define ESP_RULE
	@mkdir -p $(BUILD)
	sh tools/gen-limine-conf.sh $(2) > $(1).conf
	dd if=/dev/zero of=$(1) bs=512 count=8192 2>/dev/null
	/sbin/mkfs.fat -F 16 -s 1 $(1) >/dev/null 2>&1   # -s 1 (512B clusters): 4 MiB needs >4085 clusters for FAT16
	mmd -i $(1) ::EFI
	mmd -i $(1) ::EFI/BOOT
	mmd -i $(1) ::boot
	mcopy -i $(1) $(LIMINE_DIR)/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $(1) $(LIMINE_DIR)/BOOTIA32.EFI ::EFI/BOOT/BOOTIA32.EFI
	mcopy -i $(1) $(KERNEL_STRIPPED) ::boot/aegis.elf
	mcopy -i $(1) $(1).conf ::limine.conf
endef

$(ESP_DESKTOP): $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(KERNEL_STRIPPED) tools/gen-limine-conf.sh
	$(call ESP_RULE,$@,installed)

$(ESP_SERVER): $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(KERNEL_STRIPPED) tools/gen-limine-conf.sh
	$(call ESP_RULE,$@,server-installed)

# ── Wallpaper / logo conversion ──���───────────────────────────────────────────
# Logo + Claude raster assets now ship with the lumen compositor package
# (AspisOS/lumen: assets/logo.raw + claude.raw); the desktop image fetches them,
# so there is no logo-conversion step in this build.

# (No wallpaper.raw: the default desktop is a compositor-drawn gradient with
# the Aegis logo centered. Drop a wallpaper.raw into /usr/share to override.)

# ── ISO construction (Limine: BIOS + UEFI from one image) ───────────────
# Shared staging + xorriso recipe for all three live ISOs.
#   $(1) = ISO output   $(2) = staging dir   $(3) = gen-limine-conf.sh mode
# $(1)=ISO output  $(2)=staging dir  $(3)=gen-limine live-mode
# $(4)=rootfs image  $(5)=ESP image
define LIMINE_ISO_RULE
	@rm -rf $(2)
	@mkdir -p $(2)/boot/limine $(2)/EFI/BOOT
	cp $(KERNEL_STRIPPED) $(2)/boot/aegis.elf
	cp $(4) $(2)/boot/rootfs.img
	cp $(5) $(2)/boot/esp.img
	sh tools/gen-limine-conf.sh $(3) > $(2)/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin $(LIMINE_DIR)/limine-uefi-cd.bin $(2)/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(2)/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image \
	    --protective-msdos-label \
	    $(2) -o $(1)
	$(LIMINE_BIN) bios-install $(1)
endef

# ── The two production ISOs ─────────────────────────────────────────────────
$(BUILD)/aspisos-desktop.iso: $(KERNEL_STRIPPED) $(ROOTFS_DESKTOP) $(ESP_DESKTOP) $(LIMINE_BIN) tools/gen-limine-conf.sh
	$(call LIMINE_ISO_RULE,$@,$(BUILD)/desktop-isodir,live,$(ROOTFS_DESKTOP),$(ESP_DESKTOP))

$(BUILD)/aspisos-server.iso: $(KERNEL_STRIPPED) $(ROOTFS_SERVER) $(ESP_SERVER) $(LIMINE_BIN) tools/gen-limine-conf.sh
	$(call LIMINE_ISO_RULE,$@,$(BUILD)/server-isodir,server,$(ROOTFS_SERVER),$(ESP_SERVER))

desktop-iso: $(BUILD)/aspisos-desktop.iso
server-iso:  $(BUILD)/aspisos-server.iso
iso: desktop-iso server-iso

# Self-test ISO: the desktop image (carries captest), kernel cmdline `selftest`
# so vigil runs the userland security probe (/bin/selftest -> /bin/captest).
$(BUILD)/aspisos-test.iso: $(KERNEL_STRIPPED) $(ROOTFS_DESKTOP) $(ESP_DESKTOP) $(LIMINE_BIN) tools/gen-limine-conf.sh
	$(call LIMINE_ISO_RULE,$@,$(BUILD)/selftest-isodir,selftest,$(ROOTFS_DESKTOP),$(ESP_DESKTOP))
selftest-iso: $(BUILD)/aspisos-test.iso

# Manifest + skeleton sources, per layer. A rootfs rebuilds when any binary it
# packs OR any skeleton file (cap policies, vigil services, /etc) changes.
MANIFEST_SRCS_BASE    := $(shell grep -v '^\#' rootfs.manifest 2>/dev/null | awk 'NF>=2 {print $$1}')
SKELETON_FILES_BASE    := $(shell find rootfs         -type f 2>/dev/null)
# rootfs-desktop/ now holds only the gui-installer bundle skeleton (app.ini +
# caps.d); the rest of the graphical stack is fetched as packages. The desktop
# rootfs is assembled, not built from a manifest, so there is no desktop manifest.
SKELETON_FILES_DESKTOP := $(shell find rootfs-desktop -type f 2>/dev/null)

# No kernel in the rootfs image: the installed system boots the kernel from the
# FAT ESP (boot():/boot/aegis.elf) — see the ESP rules above.
$(ROOTFS_SERVER): $(MANIFEST_SRCS_BASE) $(SKELETON_FILES_BASE)
	AEGIS_PROFILE=server bash tools/build-rootfs.sh $@

# The desktop rootfs is ASSEMBLED from fetched component packages (the released
# .hpkgs of the per-component AspisOS repos), not compiled from in-tree graphical
# source. fetch-components downloads + unpacks them into an overlay + emits the
# herald db; make-desktop-meta adds the `desktop` meta-package; assemble layers
# the overlay + the in-tree gui-installer + the db onto the server base.
$(BUILD)/desktop-overlay.db: tools/components.list tools/fetch-components.sh tools/make-desktop-meta.sh
	bash tools/fetch-components.sh
	bash tools/make-desktop-meta.sh

$(ROOTFS_DESKTOP): $(ROOTFS_SERVER) $(BUILD)/desktop-overlay.db user/bin/gui-installer/gui-installer.elf $(SKELETON_FILES_DESKTOP) tools/assemble-desktop-rootfs.sh
	bash tools/assemble-desktop-rootfs.sh $@

rootfs: $(ROOTFS_DESKTOP) $(ROOTFS_SERVER)

# ── Tests ───────────────────────────────────────────────────────────────────
# (1) boot the live ISO → GUI greeter ("[BASTION] greeter ready") = kernel +
#     userland + display stack all up.
# (2) boot the selftest ISO → "[CAPTEST] ALL PASS" = userland capability model
#     enforced (every privileged op denied to a baseline process).
# (1) desktop ISO  -> GUI greeter ("[BASTION] greeter ready").
# (2) server  ISO  -> text console login ("[SERVERTEST] ...") + NO graphical
#     binaries present.
# (3) selftest ISO -> "[CAPTEST] ALL PASS" (userland capability model).
test: desktop-iso server-iso $(BUILD)/aspisos-test.iso
	bash tools/ostest.sh $(BUILD)/aspisos-desktop.iso
	bash tools/servertest.sh $(BUILD)/aspisos-server.iso
	bash tools/selftest.sh $(BUILD)/aspisos-test.iso

version:
	@echo "AspisOS $(AEGIS_OS_VERSION) (kernel $(KERNEL_VERSION))"

clean:
	rm -rf $(BUILD)/aspisos-desktop.iso $(BUILD)/aspisos-server.iso $(BUILD)/aspisos-test.iso \
	       $(BUILD)/desktop-isodir $(BUILD)/server-isodir $(BUILD)/selftest-isodir \
	       $(BUILD)/rootfs-desktop.img $(BUILD)/rootfs-server.img \
	       $(BUILD)/esp-desktop.img $(BUILD)/esp-server.img \
	       $(BUILD)/esp-desktop.img.conf $(BUILD)/esp-server.img.conf
