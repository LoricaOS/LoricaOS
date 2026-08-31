MUSL_DIR    = ../../../build/musl-dynamic
CC          = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS      = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall \
              -I../../lib/libinstall
TARGET      = configure.elf
LIBINSTALL  = ../../lib/libinstall/libinstall.a
BEARSSL     = ../../../build/bearssl-install

all: $(TARGET)

$(TARGET): main.c $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c \
	    -L../../lib/libinstall -linstall -L$(BEARSSL)/lib -lbearssl -lcrypt

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

clean:
	rm -f $(TARGET)
