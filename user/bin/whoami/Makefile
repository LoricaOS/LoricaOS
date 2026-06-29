MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

whoami.elf: whoami.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f whoami.elf
