MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wall -Wl,--build-id=none \
             -I../../lib/glyph \
             -I../../lib/libinstall
TARGET     = gui-installer.elf
GLYPH      = ../../lib/glyph/libglyph.a
LIBINSTALL = ../../lib/libinstall/libinstall.a

$(TARGET): main.c $(GLYPH) $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/libinstall -linstall \
	    -L../../lib/glyph -lglyph \
	    -lcrypt

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

clean:
	rm -f $(TARGET) *.o
