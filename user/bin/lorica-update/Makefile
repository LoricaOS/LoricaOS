MUSL_DIR   = ../../../build/musl-dynamic
CC         = $(MUSL_DIR)/usr/bin/musl-gcc
CFLAGS     = -O2 -fno-pie -no-pie -Wl,--build-id=none -Wall -Wextra \
             -I../../lib/libinstall
TARGET     = lorica-update.elf
LIBINSTALL = ../../lib/libinstall/libinstall.a
BEARSSL    = ../../../build/bearssl-install

all: $(TARGET)

$(TARGET): main.c $(LIBINSTALL)
	$(CC) $(CFLAGS) -o $@ main.c -L../../lib/libinstall -linstall \
	    -L$(BEARSSL)/lib -lbearssl

$(LIBINSTALL):
	$(MAKE) -C ../../lib/libinstall

selftest: main.c
	cc -O2 -Wall -Wextra -DHOST_SELFTEST -o .selftest main.c
	@test -n "$(ESP_IMAGE)" || { echo 'set ESP_IMAGE to a disk image with the ESP at 1 MiB'; exit 2; }
	./.selftest $(ESP_IMAGE)

clean:
	rm -f $(TARGET) .selftest

.PHONY: all clean selftest
