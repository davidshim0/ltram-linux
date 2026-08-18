CROSS  := aarch64-linux-gnu-
CC     := $(CROSS)gcc
CFLAGS := -O2 -Wall -Wextra
ltram-inspect: ltram-inspect.c
	$(CC) $(CFLAGS) -o $@ $<
native: ltram-inspect.c
	gcc $(CFLAGS) -o ltram-inspect.native $<
clean:
	rm -f ltram-inspect ltram-inspect.native
.PHONY: native clean
