CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
bindir ?= $(PREFIX)/bin
DESTDIR ?=
INSTALL ?= install
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags md4c-html lexbor)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs md4c-html lexbor)

.PHONY: all clean test install uninstall
all: m2t g2t

m2t: m2t.o

g2t: m2t
	ln -sf m2t g2t

test: all
	python3 tests/test_cli.py ./m2t

install: all
	$(INSTALL) -d "$(DESTDIR)$(bindir)"
	$(INSTALL) -m 755 m2t "$(DESTDIR)$(bindir)/m2t"
	ln -sf m2t "$(DESTDIR)$(bindir)/g2t"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/g2t" "$(DESTDIR)$(bindir)/m2t"

clean:
	rm -f m2t g2t m2t.o
