CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
bindir ?= $(PREFIX)/bin
DESTDIR ?=
INSTALL ?= install
PREFER_STATIC ?= 1
PACKAGES := md4c-html lexbor libcss
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

ifeq ($(PREFER_STATIC),0)
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES))
else
# --static supplies private dependencies, but does not select archive files.
# Prefer archives in pkg-config's library directories, including Lexbor's
# alternative libNAME_static.a naming, and retain -lNAME when none is present.
PACKAGE_LIBS := $(shell $(PKG_CONFIG) --libs --static $(PACKAGES))
LIBRARY_DIRS := $(patsubst -L%,%,$(filter -L%,$(PACKAGE_LIBS))) \
	$(foreach package,$(PACKAGES),$(shell $(PKG_CONFIG) --variable=libdir $(package)))
archive = $(firstword $(foreach dir,$(LIBRARY_DIRS),$(wildcard $(dir)/lib$(1).a $(dir)/lib$(1)_static.a)))
prefer_archive = $(if $(filter -l%,$(1)),$(or $(call archive,$(patsubst -l%,%,$(1))),$(1)),$(1))
LDLIBS += $(foreach flag,$(PACKAGE_LIBS),$(call prefer_archive,$(flag)))
endif

.PHONY: all clean test install uninstall
all: m2t g2t

m2t: m2t.o css.o Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ m2t.o css.o $(LDLIBS)

m2t.o css.o: css.h

g2t: m2t
	ln -sf m2t g2t

test: all
	python3 tests/test_cli.py ./m2t
	python3 tests/test_css.py ./m2t

install: all
	$(INSTALL) -d "$(DESTDIR)$(bindir)"
	$(INSTALL) -m 755 m2t "$(DESTDIR)$(bindir)/m2t"
	ln -sf m2t "$(DESTDIR)$(bindir)/g2t"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/g2t" "$(DESTDIR)$(bindir)/m2t"

clean:
	rm -f m2t g2t m2t.o css.o
