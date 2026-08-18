# Cross-build on ba8 (x86) for zuestoll08 (arm64), or `make native` for a local check.
#
# NOTE: use ':=' not '?=' for CC. make has a BUILT-IN default for CC, which counts
# as "already set", so '?=' silently leaves you cross-compiling with the host gcc.
CROSS  := aarch64-linux-gnu-
CC     := $(CROSS)gcc
CFLAGS := -O2 -Wall -Wextra
LDLIBS := -lm

matmul: matmul.c sha256.h
	$(CC) $(CFLAGS) -o $@ matmul.c $(LDLIBS)

native: matmul.c sha256.h
	gcc $(CFLAGS) -o matmul.native matmul.c $(LDLIBS)

clean:
	rm -f matmul matmul.native
.PHONY: native clean
