VERSION = 0.2.0
CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c99 -pedantic -DVERSION="\"$(VERSION)\""
LDLIBS = -lImlib2

all: palc

palc: palc.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ palc.c $(LDLIBS)

install: palc
	install -Dm755 palc $(DESTDIR)$(PREFIX)/bin/palc

clean:
	rm -f palc

.PHONY: all install clean
