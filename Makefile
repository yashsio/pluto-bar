# pluto-bar - see LICENSE for copyright and license details.
.POSIX:

PREFIX  = /usr/local
CC      = cc

PKGS    = wayland-client cairo pangocairo fontconfig
CFLAGS  = -std=c11 -pedantic -Wall -Wextra -O2
BAR_CFLAGS = -Iprotocols $(shell pkg-config --cflags $(PKGS))
BAR_LDLIBS = $(shell pkg-config --libs $(PKGS))

# libpulse is optional: without it pluto-status falls back to wpctl polling.
STATUS_PULSE = $(shell pkg-config --exists libpulse 2>/dev/null && echo 1)
STATUS_CFLAGS = -pthread $(shell if [ "$(STATUS_PULSE)" = 1 ]; then \
    echo -DHAVE_PULSE; pkg-config --cflags libpulse; fi)
STATUS_LDLIBS = -pthread $(shell if [ "$(STATUS_PULSE)" = 1 ]; then \
    pkg-config --libs libpulse; fi)

# protocol XML locations
WLR_PROTOCOLS     ?= /usr/share/wlr-protocols
WAYLAND_PROTOCOLS ?= /usr/share/wayland-protocols

PROTO = protocols/wlr-layer-shell-unstable-v1
XDG   = protocols/xdg-shell
SRC   = pluto-bar.c drw.c backend.c backend-hyprland.c backend-river.c
OBJ   = $(SRC:.c=.o) $(PROTO)-protocol.o $(XDG)-protocol.o
STATUS_SRC = pluto-status.c

all: pluto-bar pluto-status

pluto-bar: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(BAR_LDLIBS)

pluto-status: $(STATUS_SRC:.c=.o)
	$(CC) -o $@ $(STATUS_SRC:.c=.o) $(LDFLAGS) $(STATUS_LDLIBS)

pluto-bar.o drw.o backend.o backend-hyprland.o backend-river.o \
$(PROTO)-protocol.o $(XDG)-protocol.o: CFLAGS += $(BAR_CFLAGS)
pluto-bar.o: config.h bar.h backend.h drw.h $(PROTO)-client-protocol.h
backend.o: backend.h bar.h drw.h
backend-hyprland.o backend-river.o: backend.h bar.h drw.h
pluto-status.o: CFLAGS += $(STATUS_CFLAGS)
pluto-status.o: status-config.h
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
	rm -f pluto-bar pluto-status $(OBJ) pluto-status.o
	rm -rf protocols

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 pluto-bar $(DESTDIR)$(PREFIX)/bin/pluto-bar
	install -m 755 pluto-status $(DESTDIR)$(PREFIX)/bin/pluto-status

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/pluto-bar
	rm -f $(DESTDIR)$(PREFIX)/bin/pluto-status

.PHONY: all clean install uninstall
