MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
AEGIS_VERSION ?= untracked
CFLAGS = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -DAEGIS_VERSION=\"$(AEGIS_VERSION)\" -I../../lib/glyph -I../../lib/citadel

SRCS = main.c cursor.c compositor.c terminal.c about.c \
       lumen_server.c
OBJS = $(SRCS:.c=.o)
GLYPH = ../../lib/glyph/libglyph.a
CITADEL = ../../lib/citadel/libcitadel.a

all: lumen.elf

$(GLYPH):
	$(MAKE) -C ../../lib/glyph

$(CITADEL):
	$(MAKE) -C ../../lib/citadel

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lumen.elf: $(OBJS) $(GLYPH) $(CITADEL)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -L../../lib/citadel -lcitadel -L../../lib/glyph -lglyph

clean:
	rm -f *.elf *.o
