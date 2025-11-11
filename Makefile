VERSION = 0.5
OS != uname -s

-include Makefile.$(OS)

CFLAGS += -DVERSION=\"${VERSION}\"
CFLAGS += -DNAME=\"saeko\"
CFLAGS += -Wall -Wextra -std=c99 -pedantic -Wformat=2
CFLAGS += -fstack-protector-strong -D_FORTIFY_SOURCE=2

PREFIX ?= /usr/local

.PHONY: all install lint doc push clean again release

all: saeko

saeko: src/spartan.h src/spartan.c src/saeko.c
	${CC} ${CFLAGS} ${LDFLAGS} -L. -o $@ src/spartan.c src/saeko.c ${LIBS}

install: saeko
	install -d ${DESTDIR}${PREFIX}/bin
	install -d ${DESTDIR}/etc/rc.d
	install -m 755 saeko ${DESTDIR}${PREFIX}/bin/saeko
	install -m 555 saeko.rc ${DESTDIR}/etc/rc.d/saeko

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/saeko
	rm -f ${DESTDIR}/etc/rc.d/saeko

lint:
	cppcheck --enable=all --suppress=missingIncludeSystem src/*.c

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
