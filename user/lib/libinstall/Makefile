MUSL_DIR = ../../../build/musl-dynamic
CC       = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS   = -O2 -fno-pie -no-pie -Wall -Wextra \
           -fno-stack-protector -Wl,--build-id=none \
           -I../../../build/bearssl-install/include

TARGET = libinstall.a
SRCS   = gpt.c copy.c cryptroot_crypto.c ext2_grow.c ext2_repair.c config.c credentials.c run.c install_elevate.c
OBJS   = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	ar rcs $@ $^

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
