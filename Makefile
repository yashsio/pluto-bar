# pluto-bar - see LICENSE for copyright and license details.
.POSIX:

PREFIX  = /usr/local
CC     ?= cc

PKGS    = wayland-client cairo pangocairo fontconfig
CFLAGS  = -std=c11 -pedantic -Wall -Wextra -O2 -Iprotocols \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS  = $(shell pkg-config --libs $(PKGS))

# protocol XML locations
WLR_PROTOCOLS     ?= /usr/share/wlr-protocols
WAYLAND_PROTOCOLS ?= /usr/share/wayland-protocols

PROTO = protocols/wlr-layer-shell-unstable-v1
XDG   = protocols/xdg-shell
SRC   = pluto-bar.c drw.c
OBJ   = $(SRC:.c=.o) $(PROTO)-protocol.o $(XDG)-protocol.o

all: pluto-bar

pluto-bar: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

pluto-bar.o: config.h drw.h $(PROTO)-client-protocol.h
drw.o: drw.h $(PROTO)-client-protocol.h
$(PROTO)-protocol.o: $(PROTO)-protocol.c

$(PROTO)-client-protocol.h: $(PROTO).xml
	wayland-scanner client-header $(PROTO).xml $@

$(PROTO)-protocol.c: $(PROTO).xml
	wayland-scanner private-code $(PROTO).xml $@

$(XDG)-protocol.o: $(XDG)-protocol.c

$(XDG)-protocol.c: $(XDG).xml
	wayland-scanner private-code $(XDG).xml $@

$(PROTO).xml:
	mkdir -p protocols
	cp $(WLR_PROTOCOLS)/unstable/wlr-layer-shell-unstable-v1.xml $@

$(XDG).xml:
	mkdir -p protocols
	cp $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

clean:
	rm -f pluto-bar $(OBJ)
	rm -rf protocols

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f pluto-bar $(DESTDIR)$(PREFIX)/bin
	cp -f status.sh $(DESTDIR)$(PREFIX)/bin/pluto-status

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pluto-bar
	rm -f $(DESTDIR)$(PREFIX)/bin/pluto-status

.PHONY: all clean install uninstall
