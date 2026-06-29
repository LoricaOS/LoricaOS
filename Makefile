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
ROOTFS = $(BUILD)/rootfs.img
ISO_DIR = $(BUILD)/isodir

# C++ cross toolchain (used by build/cpptest fixture; provisioned out-of-repo)
CXX_AEGIS       = /opt/aegis-cxx/bin/x86_64-buildroot-linux-musl-g++
CXX_FLAGS_AEGIS = -static -O2 -std=c++23 -fno-pie -no-pie

.PHONY: all iso rootfs build-musl test clean version curl_bin
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
    perfbench-ipc forkbench

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

user/lib/citadel/libcitadel.a: $(wildcard user/lib/citadel/*.c user/lib/citadel/*.h) user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/lib/citadel

user/lib/libinstall/libinstall.a: $(wildcard user/lib/libinstall/*.c user/lib/libinstall/*.h) $(MUSL_BUILT)
	$(MAKE) -C user/lib/libinstall

# Programs with extra library dependencies
user/bin/lumen/lumen.elf: $(wildcard user/bin/lumen/*.c user/bin/lumen/*.h) user/lib/glyph/libglyph.a user/lib/citadel/libcitadel.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/lumen

user/bin/bastion/bastion.elf: user/bin/bastion/main.c user/lib/glyph/libglyph.a user/lib/libauth/libauth.a user/lib/audio/libaudio.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/bastion

user/lib/audio/libaudio.a: user/lib/audio/audio.c user/lib/audio/audio.h user/lib/audio/minimp3.h
	$(MAKE) -C user/lib/audio

user/bin/installer/installer.elf: user/bin/installer/main.c user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/installer

user/bin/gui-installer/gui-installer.elf: user/bin/gui-installer/main.c user/lib/glyph/libglyph.a user/lib/libinstall/libinstall.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/gui-installer

user/bin/settings/settings.elf: user/bin/settings/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/settings

user/bin/terminal/terminal.elf: user/bin/terminal/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/terminal

user/bin/calculator/calculator.elf: user/bin/calculator/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/calculator

user/bin/editor/editor.elf: user/bin/editor/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/editor

user/bin/filemanager/filemanager.elf: user/bin/filemanager/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/filemanager

user/bin/run/run.elf: user/bin/run/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/run

user/bin/tunes/tunes.elf: user/bin/tunes/main.c user/lib/glyph/libglyph.a user/lib/audio/libaudio.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/tunes

user/bin/calendar/calendar.elf: user/bin/calendar/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/calendar

user/bin/netman/netman.elf: user/bin/netman/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/netman

user/bin/applications/applications.elf: user/bin/applications/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/applications

user/bin/imageviewer/imageviewer.elf: user/bin/imageviewer/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/imageviewer

user/bin/sysmon/sysmon.elf: user/bin/sysmon/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/sysmon

user/bin/lumen-probe/lumen-probe.elf: user/bin/lumen-probe/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/lumen-probe

user/bin/citadel-dock/citadel-dock.elf: user/bin/citadel-dock/main.c user/lib/glyph/libglyph.a $(MUSL_BUILT)
	$(MAKE) -C user/bin/citadel-dock

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
ESP_IMG    = $(BUILD)/esp.img
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
$(ESP_IMG): $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI $(KERNEL_STRIPPED) tools/gen-limine-conf.sh
	@mkdir -p $(BUILD)
	sh tools/gen-limine-conf.sh installed > $(BUILD)/limine-installed.conf
	dd if=/dev/zero of=$(ESP_IMG) bs=512 count=8192 2>/dev/null
	/sbin/mkfs.fat -F 16 -s 1 $(ESP_IMG) >/dev/null 2>&1   # -s 1 (512B clusters): 4 MiB needs >4085 clusters for FAT16
	mmd -i $(ESP_IMG) ::EFI
	mmd -i $(ESP_IMG) ::EFI/BOOT
	mmd -i $(ESP_IMG) ::boot
	mcopy -i $(ESP_IMG) $(LIMINE_DIR)/BOOTX64.EFI ::EFI/BOOT/BOOTX64.EFI
	mcopy -i $(ESP_IMG) $(LIMINE_DIR)/BOOTIA32.EFI ::EFI/BOOT/BOOTIA32.EFI
	mcopy -i $(ESP_IMG) $(KERNEL_STRIPPED) ::boot/aegis.elf
	mcopy -i $(ESP_IMG) $(BUILD)/limine-installed.conf ::limine.conf

# ── Wallpaper / logo conversion ──���───────────────────────────────────────────
$(BUILD)/logo.raw: tools/aegis-logo.png
	@mkdir -p $(BUILD)
	@if command -v python3 >/dev/null 2>&1 && [ -f tools/convert-logo.py ]; then \
	    python3 tools/convert-logo.py $< $@; \
	else touch $@; fi

$(BUILD)/claude.raw: assets/claude-white.png
	@mkdir -p $(BUILD)
	@if command -v python3 >/dev/null 2>&1 && [ -f tools/convert-logo.py ]; then \
	    python3 tools/convert-logo.py $< $@; \
	else touch $@; fi

# (No wallpaper.raw: the default desktop is a compositor-drawn gradient with
# the Aegis logo centered. Drop a wallpaper.raw into /usr/share to override.)

# ── ISO construction (Limine: BIOS + UEFI from one image) ───────────────
# Shared staging + xorriso recipe for all three live ISOs.
#   $(1) = ISO output   $(2) = staging dir   $(3) = gen-limine-conf.sh mode
define LIMINE_ISO_RULE
	@rm -rf $(2)
	@mkdir -p $(2)/boot/limine $(2)/EFI/BOOT
	cp $(KERNEL_STRIPPED) $(2)/boot/aegis.elf
	cp $(ROOTFS) $(2)/boot/rootfs.img
	cp $(ESP_IMG) $(2)/boot/esp.img
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

$(BUILD)/aegis.iso: $(KERNEL_STRIPPED) $(ROOTFS) $(ESP_IMG) $(LIMINE_BIN) tools/gen-limine-conf.sh
	$(call LIMINE_ISO_RULE,$@,$(ISO_DIR),live)

iso: $(BUILD)/aegis.iso
MANIFEST_SRCS := $(shell grep -v '^\#' rootfs.manifest 2>/dev/null | awk 'NF>=2 {print $$1}')
# Also depend on every file under the rootfs/ skeleton tree (cap policies,
# vigil services, etc.) so editing /etc files actually triggers a rebuild.
SKELETON_FILES := $(shell find rootfs -type f 2>/dev/null)

# No kernel in the rootfs image: the installed system boots the kernel from
# the FAT ESP (gen-limine-conf.sh installed mode reads boot():/boot/aegis.elf,
# baked into esp.img above) — the old rootfs /boot/aegis.elf copy was dead
# weight inside the fixed 44 MiB image.
$(ROOTFS): $(MANIFEST_SRCS) $(SKELETON_FILES) $(BUILD)/logo.raw $(BUILD)/claude.raw
	bash tools/build-rootfs.sh $@ "" $(BUILD)/logo.raw $(BUILD)/claude.raw

rootfs: $(ROOTFS)

# ── OS boot test: boot the live ISO headless; success = the GUI greeter comes
# up ("[BASTION] greeter ready"), proving kernel + userland + display stack. ──
test: iso
	bash tools/ostest.sh $(BUILD)/aegis.iso

version:
	@echo "AspisOS $(AEGIS_OS_VERSION) (kernel $(KERNEL_VERSION))"

clean:
	rm -rf $(BUILD)/aegis.iso $(BUILD)/isodir $(BUILD)/rootfs.img $(BUILD)/esp.img
