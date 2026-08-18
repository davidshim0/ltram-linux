CC=g++
PREFIX ?= $(shell pwd)/install
# By default, don't use TBB so everything can just be statically linked
USE_TBB ?= 0
CFLAGS=-std=c++11 -g -Wall -pthread -I./ -I$(PREFIX)/include
TBB_LDFLAGS=
ifeq ($(USE_TBB),1)
  CFLAGS += -DUSE_TBB
  TBB_LDFLAGS = -Wl,-Bdynamic -ltbb -Wl,-Bstatic
endif
LDFLAGS = -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib -Wl,-Bstatic -lpthread -lhiredis -lm $(TBB_LDFLAGS) -static-libstdc++ -static-libgcc
SUBDIRS=core db redis
SUBSRCS=$(wildcard core/*.cc) $(wildcard db/*.cc)
OBJECTS=$(SUBSRCS:.cc=.o)
EXEC=ycsbc

all: $(SUBDIRS) $(EXEC)

$(SUBDIRS):
	$(MAKE) -C $@ USE_TBB=$(USE_TBB) PREFIX=$(PREFIX)

$(EXEC): $(wildcard *.cc) $(OBJECTS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir $@; \
	done
	$(RM) $(EXEC)
	rm -rf install

.PHONY: $(SUBDIRS) $(EXEC)
