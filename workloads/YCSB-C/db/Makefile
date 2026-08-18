CC=g++
USE_TBB ?= 0
CFLAGS=-std=c++11 -c -g -Wall -fgnu-tm
ifeq ($(USE_TBB),1)
  CFLAGS += -DUSE_TBB
endif
INCLUDES=-I../
SOURCES=$(wildcard *.cc)
OBJECTS=$(SOURCES:.cc=.o)

all: $(SOURCES) $(OBJECTS)

.cc.o:
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

clean:
	$(RM) $(OBJECTS)
