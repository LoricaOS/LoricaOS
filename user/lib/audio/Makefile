CC         = musl-gcc
# minimp3 is third-party; build it without -Wall noise but keep our own code warned.
CFLAGS     = -O2 -fno-pie -Wall
TARGET     = libaudio.a
SRCS       = audio.c
OBJS       = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	ar rcs $@ $^

audio.o: audio.c audio.h minimp3.h
	$(CC) $(CFLAGS) -c -o $@ audio.c

# Host self-check (uses the native cc, NOT musl — it runs on the build host).
selftest: selftest.c audio.c audio.h minimp3.h
	cc -O2 -Wall -o selftest selftest.c audio.c

clean:
	rm -f $(TARGET) $(OBJS) selftest
