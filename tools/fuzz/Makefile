# tools/fuzz — host fuzzing for loricaos's network-sourced parsers.
#
# herald downloads and parses attacker-influenced bytes (a .hpkg ustar archive,
# its manifest, the repo's Release/Packages), and the DHCP client parses an
# unauthenticated server reply. All four are (buf,len) entry points with no
# syscall dependencies, so they fuzz on the host directly.
#
#   make && python3 seeds.py . && ./fuzz_tar corpus_tar
#
# Needs clang with libFuzzer. See README.md for coverage limits.

SRC    := $(abspath $(CURDIR)/../..)
HERALD := $(SRC)/user/bin/herald
DHCP   := $(SRC)/user/bin/dhcp

# Plain `=`, not `?=`: make predefines CC=cc, which `?=` would not override.
CC     = clang
SAN    := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
CFLAGS := -g -O1 -fno-omit-frame-pointer -Wall $(SAN)

TARGETS := fuzz_manifest fuzz_tar fuzz_repo fuzz_dhcp

all: $(TARGETS)

fuzz_manifest: fuzz_manifest.c $(HERALD)/manifest.c $(HERALD)/manifest.h
	$(CC) $(CFLAGS) -I$(HERALD) -o $@ fuzz_manifest.c $(HERALD)/manifest.c

# tar.c is built with its filesystem calls redirected into the harness, so
# extraction does no real I/O and the harness can assert containment on every
# path tar.c would create.
tar_pure.o: $(HERALD)/tar.c
	$(CC) $(CFLAGS) -I$(HERALD) \
	  -Dopen=fz_open -Dmkdir=fz_mkdir -Dwrite=fz_write -Dclose=fz_close \
	  -c -o $@ $<

fuzz_tar: fuzz_tar.c tar_pure.o
	$(CC) $(CFLAGS) -I$(HERALD) -o $@ $^

# repo.c and dhcp/main.c are #included by their harnesses (for the statics), so
# they MUST be listed as prerequisites — otherwise make reuses a binary built
# from stale source and a re-run silently proves nothing.
fuzz_repo: fuzz_repo.c trusted_key.h $(HERALD)/repo.c $(HERALD)/repo.h
	$(CC) $(CFLAGS) -I. -I$(HERALD) -o $@ fuzz_repo.c

trusted_key.h:
	printf 'static const unsigned char herald_trusted_key[65] = {0x04};\n' > $@

fuzz_dhcp: fuzz_dhcp.c $(DHCP)/main.c
	$(CC) $(CFLAGS) -I$(DHCP) -o $@ fuzz_dhcp.c

seeds:
	python3 seeds.py .

clean:
	rm -f $(TARGETS) tar_pure.o trusted_key.h crash-* leak-* timeout-* oom-*
	rm -rf corpus_tar corpus_manifest corpus_repo corpus_dhcp

.PHONY: all seeds clean
