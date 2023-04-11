VERSION = 0.1
OS != uname -s

-include Makefile.$(OS)

CFLAGS += -DVERSION=\"${VERSION}\"
CFLAGS += -DNAME=\"saeko\"
CFLAGS += -Wall -Wextra -std=c99 -pedantic -static

PREFIX ?= /usr/local
MANDIR ?= /share/man

LIBS += -lmagic -lz

all: saeko

config.h:
	cp config.def.h $@

saeko: config.h src/saeko.c
	${CC} ${CFLAGS} ${LDFLAGS} -L. -o $@ src/saeko.c ${LIBS}
	strip $@

install:
	install saeko ${DESTDIR}${PREFIX}/bin/saeko

clean:
	rm -f saeko

again: clean all

