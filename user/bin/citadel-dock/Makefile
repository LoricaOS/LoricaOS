MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -I../../lib/glyph -I../../lib/citadel
GLYPH = ../../lib/glyph/libglyph.a

citadel-dock.elf: main.c $(GLYPH)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/glyph -lglyph

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

clean:
	rm -f *.elf *.o
