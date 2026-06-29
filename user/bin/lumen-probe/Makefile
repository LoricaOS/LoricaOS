MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wall -I../../lib/glyph
GLYPH = ../../lib/glyph/libglyph.a

lumen-probe.elf: main.c $(GLYPH)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/glyph -lglyph

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

clean:
	rm -f *.elf *.o
