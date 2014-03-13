CC = gcc
CFLAGS = -Wall -O2 -std=c11 -D_GNU_SOURCE -pthread -fPIC
LDFLAGS = -shared -Wl,-soname,libclrunon.so -lpthread -ldl
TARGETS = libclrunon.so

all: $(TARGETS)

libclrunon.so: libclrunon.o Makefile
	$(CC) -o libclrunon.so libclrunon.o $(LDFLAGS)

libclrunon.o: libclrunon.c Makefile
	$(CC) -c libclrunon.c $(CFLAGS)

clean:
	rm -f $(TARGETS) *.o
