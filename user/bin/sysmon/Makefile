MUSL_DIR = ../../../build/musl-dynamic
CC       = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS   = -O2 -fno-pie -no-pie -Wall -Wl,--build-id=none \
           -I../../lib/glyph
TARGET   = sysmon.elf
GLYPH    = ../../lib/glyph/libglyph.a

$(TARGET): main.c $(GLYPH)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/glyph -lglyph

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

clean:
	rm -f $(TARGET) *.o
