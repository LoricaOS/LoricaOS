MUSL_DIR = ../../../build/musl-dynamic
CC       = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS   = -O2 -fno-pie -no-pie -Wall -Wl,--build-id=none \
           -I../../lib/glyph -I../../lib/audio
TARGET   = tunes.elf
GLYPH    = ../../lib/glyph/libglyph.a
AUDIO    = ../../lib/audio/libaudio.a

$(TARGET): main.c $(GLYPH) $(AUDIO)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/glyph -lglyph -L../../lib/audio -laudio

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(AUDIO):
	$(MAKE) -C ../../lib/audio

clean:
	rm -f $(TARGET) *.o
