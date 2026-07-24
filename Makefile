OS=$(shell uname)
LUA_INCDIR?=/usr/include/lua5.4
OBJECTS=bestline.o bestlinelib.o

ifeq ($(OS),Darwin)
bestline.so: $(OBJECTS)
	$(CC) -o $@ -bundle -undefined dynamic_lookup $^ $(OPT_LIB)
else
CFLAGS?=-O2
CFLAGS+=-fPIC -I$(LUA_INCDIR)
bestline.so: $(OBJECTS)
	$(CC) -o $@ -shared $^ $(OPT_LIB)
endif

bestline-test: test.c bestline.so
	$(CC) -Wall -W -Os -g -o bestline-test test.c -lutil

test: bestline-test
	./bestline-test

test-unit: bestline.so
	LUA_CPATH="./?.so;;" lua test/test_bestline.lua

clean:
	rm -f *.o *.so *.dylib bestline-test
