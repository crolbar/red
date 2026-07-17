SRC = red.c \
		drm.c \
		wayland-backend-client.c \
		drmProps.c \
		input.c \
		log.c \
		signals.c \
		vt.c \
		config.c \
		gbm.c \
		render.c \
		time.c

#REMOVE wayland egl
DEPS = wayland-server \
		wayland-client \
		wayland-egl \
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
LINUX_DMABUF_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml

all: red

xdg-shell-client-protocol.h: $(XDG_SHELL_XML)
	wayland-scanner client-header $< $@
xdg-shell-protocol.c: $(XDG_SHELL_XML)
	wayland-scanner private-code $< $@

linux-dmabuf-protocol.h: $(LINUX_DMABUF_XML)
	wayland-scanner client-header $< $@
linux-dmabuf-protocol.c: $(LINUX_DMABUF_XML)
	wayland-scanner private-code $< $@

PRO_SRC=linux-dmabuf-protocol.c  xdg-shell-protocol.c

red: $(SRC) $(PRO_SRC) xdg-shell-client-protocol.h linux-dmabuf-protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(PRO_SRC) $(LDLIBS)

clean:
	rm -f red *protocol.c *protocol.h *.o

.PHONY: all clean red
