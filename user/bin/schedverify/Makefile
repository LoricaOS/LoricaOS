MUSL_DIR = ../../../build/musl-dynamic
CC = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS = -O2 -s -fno-pie -no-pie -Wl,--build-id=none -Wall -Wextra

schedverify.elf: main.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f schedverify.elf
