MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -s -fno-pie -no-pie -Wl,--build-id=none -Wall -Wextra

SRCS = main.c editor.c history.c complete.c parser.c exec.c caps.c env.c
OBJS = $(SRCS:.c=.o)

stsh.elf: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c stsh.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.elf *.o
