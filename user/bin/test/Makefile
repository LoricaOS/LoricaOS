MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none

# test and `[` are the same binary; bracket.elf is a copy installed at /bin/[.
all: test.elf bracket.elf

test.elf: main.c
	$(CC) $(CFLAGS) -o $@ $<

bracket.elf: test.elf
	cp test.elf bracket.elf

clean:
	rm -f *.elf
