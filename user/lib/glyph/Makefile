MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -fno-pie -no-pie -Wall
AR = ar

# Source set mirrors canonical glyph's libglyph object list (see
# ~/Developer/glyph Makefile GLYPH_OBJS) so this vendored copy — built into the
# base image for the gui-installer — stays a faithful, current snapshot of the
# toolkit the apps fetch, rather than drifting behind it. glyph_cursor.c +
# layout.c were added in glyph 1.1.x.
SRCS = draw.c theme.c window.c \
       font.c lumen_client.c glyph_term.c apps.c icons.c image_load.c \
       glyph_cursor.c layout.c
OBJS = $(SRCS:.c=.o)

all: libglyph.a

HOST_CC ?= cc
.PHONY: test
test:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	$(HOST_CC) -O1 -g -Wall -Wextra -fsanitize=address,undefined \
	    -fno-omit-frame-pointer text_elide_test.c draw.c font.c theme.c \
	    -lm -o "$$tmp/test"; \
	ASAN_OPTIONS=detect_leaks=0 "$$tmp/test" "$(TEST_FONT)"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

libglyph.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f libglyph.a *.o
