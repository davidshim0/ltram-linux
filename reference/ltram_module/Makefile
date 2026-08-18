# Out-of-tree build for the ltram smoke test and (later) main module.
# Points at the kernel tree in linux/ submodule.

KDIR ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../linux)
PWD  := $(shell pwd)

obj-m += ltram_smoketest.o

# ltram.o (the COW handler scaffold) is not yet a complete module;
# leave it out until we wire up mmap + .fault.
# obj-m += ltram.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
