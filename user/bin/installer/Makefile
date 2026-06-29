MUSL_DIR    = ../../../build/musl-dynamic
CC          = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS      = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall \
              -I../../lib/libinstall
TARGET      = installer.elf
LIBINSTALL  = ../../lib/libinstall/libinstall.a

all: $(TARGET)

$(TARGET): main.c $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/libinstall -linstall -lcrypt

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

clean:
	rm -f $(TARGET)
