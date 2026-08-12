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
		ipc.c \
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
CFLAGS  += -Wall -Wextra -Wno-unused-parameter \
           $(shell pkg-config --cflags $(DEPS))
CFLAGS_DEBUG = -g
CFLAGS_RELEASE = -O2 -DNDEBUG -flto
LDLIBS  += $(shell pkg-config --libs $(DEPS))

PREFIX ?= /usr/local
BINS ?= red

WAYLAND_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)
WLR_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wlr-protocols)

XDG_SHELL_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION_XML = $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
LINUX_DMABUF_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/linux-dmabuf/linux-dmabuf-v1.xml
VIEWPORTER_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/viewporter/viewporter.xml
RELATIVE_POINTER_XML = $(WAYLAND_PROTOCOLS_DIR)/unstable/relative-pointer/relative-pointer-unstable-v1.xml
POINTER_CONSTRAINTS_XML = $(WAYLAND_PROTOCOLS_DIR)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
PRESENTATION_TIME_XML = $(WAYLAND_PROTOCOLS_DIR)/stable/presentation-time/presentation-time.xml
LAYER_SHELL_XML = $(WLR_PROTOCOLS_DIR)/unstable/wlr-layer-shell-unstable-v1.xml

all: $(BINS)

layer-shell-server-protocol.h: $(LAYER_SHELL_XML)
	wayland-scanner server-header $< $@
layer-shell-protocol.c: $(LAYER_SHELL_XML)
	wayland-scanner private-code $< $@

presentation-time-server-protocol.h: $(PRESENTATION_TIME_XML)
	wayland-scanner server-header $< $@
presentation-time-protocol.c: $(PRESENTATION_TIME_XML)
	wayland-scanner private-code $< $@

pointer-constraints-server-protocol.h: $(POINTER_CONSTRAINTS_XML)
	wayland-scanner server-header $< $@
pointer-constraints-protocol.c: $(POINTER_CONSTRAINTS_XML)
	wayland-scanner private-code $< $@

relative-pointer-server-protocol.h: $(RELATIVE_POINTER_XML)
	wayland-scanner server-header $< $@
relative-pointer-protocol.c: $(RELATIVE_POINTER_XML)
	wayland-scanner private-code $< $@

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
		 layer-shell-server-protocol.h \
		 layer-shell-protocol.c \
		 presentation-time-protocol.c \
		 presentation-time-server-protocol.h \
		 pointer-constraints-server-protocol.h \
		 pointer-constraints-protocol.c \
		 relative-pointer-server-protocol.h \
		 relative-pointer-protocol.c \
		 viewporter-server-protocol.h \
		 viewporter-protocol.c \
		 xdg-shell-protocol.c \
		 xdg-shell-client-protocol.h \
		 xdg-shell-server-protocol.h \
		 xdg-decoration-server-protocol.h \
		 xdg-decoration-protocol.c

pro: $(PRO_SRC)

$(BINS): $(SRC) $(PRO_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) -o $@ $(SRC) $(PRO_SRC) $(LDLIBS)

release: $(SRC) $(PRO_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_RELEASE) -o $(BINS) $(SRC) $(PRO_SRC) $(LDLIBS)

run: all
	./run.sh

prof: $(SRC) $(PRO_SRC)
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) -fno-omit-frame-pointer -o $(BINS) $(SRC) $(PRO_SRC) $(LDLIBS) -lprofiler
	./run_prof.sh

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin $(BINS)

clean:
	rm -f red *protocol.c *protocol.h *.o

.PHONY: all clean red release prof
