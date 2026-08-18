CROSS  := aarch64-linux-gnu-
CC     := $(CROSS)gcc
CFLAGS := -O2 -Wall -Wextra

all: ltram-inspect cache-probe

ltram-inspect: ltram-inspect.c
	$(CC) $(CFLAGS) -o $@ $<
cache-probe: cache-probe.c
	$(CC) $(CFLAGS) -o $@ $<

native: ltram-inspect.c cache-probe.c
	gcc $(CFLAGS) -o ltram-inspect.native ltram-inspect.c
	gcc $(CFLAGS) -o cache-probe.native   cache-probe.c

clean:
	rm -f ltram-inspect cache-probe *.native
.PHONY: all native clean
