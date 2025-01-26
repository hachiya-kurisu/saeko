VERSION = 0.1
OS != uname -s

-include Makefile.$(OS)

CFLAGS += -DVERSION=\"${VERSION}\"
CFLAGS += -DNAME=\"saeko\"
CFLAGS += -Wall -Wextra -std=c99 -pedantic -static

PREFIX ?= /usr/local
MANDIR ?= /share/man

all: saeko

config.h:
	cp config.def.h $@

saeko: config.h src/saeko.c
	${CC} ${CFLAGS} ${LDFLAGS} -L. -o $@ src/saeko.c ${LIBS}
	strip $@

install:
	install saeko ${DESTDIR}${PREFIX}/bin/saeko
	install saeko.rc /etc/rc.d/saeko

README.md: README.gmi
	sisyphus -f markdown <README.gmi >README.md

doc: README.md

push:
	got send
	git push github
 
clean:
	rm -f saeko

again: clean all

release: push
	git push github --tags
