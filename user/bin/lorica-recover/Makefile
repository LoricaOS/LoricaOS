MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -Os -fno-pie -no-pie -static -Wl,--build-id=none -Wall -Wextra \
             -I../../lib/libinstall -I../../../build/bearssl-install/include
TARGET     = lorica-recover.elf
LIBINSTALL = ../../lib/libinstall/libinstall.a
BEARSSL    = ../../../build/bearssl-install

all: $(TARGET)

$(TARGET): main.c $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/libinstall -linstall -L$(BEARSSL)/lib -lbearssl

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

clean:
	rm -f $(TARGET)
