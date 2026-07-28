SRC = red.c \
		wayland.c \
		compositor.c \
		actions.c \
		drm.c \
		backend-drm.c \
		backend-wayland.c \
		backend-wayland-client.c \
		red_cursor.c \
		drmProps.c \
		opengl.c \
		input.c \
		log.c \
		signals.c \
		vt.c \
		config.c \
		gbm.c \
		render.c \
		time.c

DEPS = wayland-server \
		wayland-client \
		libdrm \
		libudev \
		libinput \
		xkbcommon \
		gbm \
		egl \
		glesv2

CC      ?= gcc
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -g \
           $(shell pkg-config --cflags $(DEPS))
LDLIBS  += $(shell pkg-config --libs $(DEPS))

PREFIX ?= /usr/local
BINS ?= red

WAYLAND_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION_XML = $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
LINUX_DMABUF_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml
VIEWPORTER_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/viewporter.xml

all: $(BINS)

viewporter-server-protocol.h: $(VIEWPORTER_XML)
	wayland-scanner server-header $< $@
viewporter-protocol.c: $(VIEWPORTER_XML)
	wayland-scanner private-code $< $@

xdg-decoration-server-protocol.h: $(XDG_DECORATION_XML)
	wayland-scanner server-header $< $@
xdg-decoration-protocol.c: $(XDG_DECORATION_XML)
	wayland-scanner private-code $< $@

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@
xdg-shell-server-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner server-header $< $@
xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

linux-dmabuf-client-protocol.h: $(LINUX_DMABUF_XML)
	wayland-scanner client-header $< $@
linux-dmabuf-server-protocol.h: $(LINUX_DMABUF_XML)
	wayland-scanner server-header $< $@
linux-dmabuf-protocol.c: $(LINUX_DMABUF_XML)
	wayland-scanner private-code $< $@

PRO_SRC=linux-dmabuf-protocol.c \
		 linux-dmabuf-client-protocol.h \
		 linux-dmabuf-server-protocol.h \
		 viewporter-server-protocol.h \
		 viewporter-protocol.c \
		 xdg-shell-protocol.c \
		 xdg-shell-client-protocol.h \
		 xdg-shell-server-protocol.h \
		 xdg-decoration-server-protocol.h \
		 xdg-decoration-protocol.c

pro: $(PRO_SRC)

$(BINS): $(SRC) $(PRO_SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(PRO_SRC) $(LDLIBS)

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin $(BINS)

clean:
	rm -f red *protocol.c *protocol.h *.o

.PHONY: all clean red
