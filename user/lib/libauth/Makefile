CC         = musl-gcc
CFLAGS     = -O2 -fno-pie -Wall
TARGET     = libauth.a
SRCS       = auth.c
OBJS       = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	ar rcs $@ $^

%.o: %.c auth.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)
