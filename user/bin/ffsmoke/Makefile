MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
FFI        = ../../../build/ffmpeg-install
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none -Wall -I$(FFI)/include
LIBS       = -L$(FFI)/lib -lavformat -lavcodec -lswscale -lswresample -lavutil

ffsmoke.elf: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f *.elf
