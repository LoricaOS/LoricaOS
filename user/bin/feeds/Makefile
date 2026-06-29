MUSL_DIR = ../../../build/musl-dynamic
CC       = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS   = -O2 -fno-pie -no-pie -Wall -Wl,--build-id=none \
           -I../../lib/glyph
TARGET   = feeds.elf
GLYPH    = ../../lib/glyph/libglyph.a
SRCS     = main.c feeds_net.c feeds_parse.c feeds_store.c feeds_text.c
HDRS     = feeds.h

$(TARGET): $(SRCS) $(HDRS) $(GLYPH)
	$(CC) $(CFLAGS) -o $@ $(SRCS) \
	    -L../../lib/glyph -lglyph

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

clean:
	rm -f $(TARGET) *.o
