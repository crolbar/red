SRC = red.c \
		drm.c \
		input.c \
		log.c \
		signals.c \
		vt.c \
		config.c \
		gbm.c \
		render.c \
		time.c

DEPS = wayland-server \
		libdrm \
		libudev \
		libinput \
		gbm \
		egl \
		glesv2

CC      ?= gcc
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -g \
           $(shell pkg-config --cflags $(DEPS))
LDLIBS  += $(shell pkg-config --libs $(DEPS))

WAYLAND_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

all: red

xdg-shell-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner server-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

red: xdg-shell-protocol.c xdg-shell-protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f tinycompositor xdg-shell-protocol.c xdg-shell-protocol.h *.o

.PHONY: all clean red
