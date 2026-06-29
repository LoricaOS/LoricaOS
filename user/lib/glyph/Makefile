MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wall
AR = ar

SRCS = draw.c theme.c window.c \
       font.c lumen_client.c glyph_term.c apps.c icons.c image_load.c
OBJS = $(SRCS:.c=.o)

all: libglyph.a

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

libglyph.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f libglyph.a *.o
