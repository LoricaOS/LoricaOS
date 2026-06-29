MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -static -fno-pie -no-pie -Wl,--build-id=none -Wall -I../../lib/glyph -I../../lib/libauth -I../../lib/audio
TARGET     = bastion.elf
GLYPH      = ../../lib/glyph/libglyph.a
AUTH       = ../../lib/libauth/libauth.a
AUDIO      = ../../lib/audio/libaudio.a

$(TARGET): main.c $(GLYPH) $(AUTH) $(AUDIO)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/glyph -lglyph -L../../lib/libauth -lauth -L../../lib/audio -laudio

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(AUTH):
	$(MAKE) -C ../../lib/libauth

$(AUDIO):
	$(MAKE) -C ../../lib/audio

clean:
	rm -f $(TARGET) *.o
