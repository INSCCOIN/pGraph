CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -lm
PREFIX ?= /usr/local

pGraph: pGraph.c
	$(CC) -O2 -Wall -Wextra -o pGraph pGraph.c -lm

install: pGraph
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 pGraph $(DESTDIR)$(PREFIX)/bin/pGraph

clean:
	rm -f pGraph
