CC      ?= gcc
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -g \
           $(shell pkg-config --cflags wayland-server libdrm libinput gbm egl glesv2)
LDLIBS  += -ludev \
	$(shell pkg-config --libs wayland-server libdrm libinput gbm egl glesv2)

WAYLAND_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

all: red

xdg-shell-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner server-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@


red: main.c xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) -o $@ main.c xdg-shell-protocol.c $(LDLIBS)

drm: drm.c
	$(CC) $(CFLAGS) -o drm drm.c input.c log.c signals.c vt.c $(LDLIBS)

clean:
	rm -f tinycompositor xdg-shell-protocol.c xdg-shell-protocol.h *.o

.PHONY: all clean drm
