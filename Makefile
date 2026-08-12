CC = gcc
CFLAGS = -O2 -std=gnu11

.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	$(CC) $(CFLAGS) -o code main.c buddy.c

clean:
	rm -f code test
