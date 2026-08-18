CC=g++
PREFIX ?= $(shell pwd)/../install
USE_TBB ?= 1
CFLAGS+=-Wall -std=c++11
INCLUDES=-I../ -I$(PREFIX)/include
HEADERS=$(wildcard *.h)
TBB_LDFLAGS=
ifeq ($(USE_TBB),1)
  TBB_LDFLAGS = -Wl,-Bdynamic -ltbb -Wl,-Bstatic
endif
LDFLAGS = -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib -Wl,-Bstatic -lpthread -lhiredis -lm $(TBB_LDFLAGS) -static-libstdc++ -static-libgcc

all: hiredis sample

sample:	sample.cc ../db/redis_db.o $(HEADERS)
	$(MAKE) -C ./hiredis
	$(MAKE) PREFIX=$(PREFIX) install -C ./hiredis
	$(MAKE) -C ../db USE_TBB=$(USE_TBB)
	$(CC) $(CFLAGS) $(INCLUDES) $< ../db/redis_db.o $(LDFLAGS) -o $@

clean:
	$(RM) sample
	$(MAKE) -C ./hiredis clean
