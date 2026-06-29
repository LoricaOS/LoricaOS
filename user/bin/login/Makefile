CC      = musl-gcc
CFLAGS  = -O2 -static -Wall -Wextra -I../../lib/libauth

login.elf: main.c ../../lib/libauth/libauth.a
	$(CC) $(CFLAGS) -o login.elf main.c -L../../lib/libauth -lauth

../../lib/libauth/libauth.a:
	$(MAKE) -C ../../lib/libauth

clean:
	rm -f login.elf *.o
