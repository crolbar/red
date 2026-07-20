#include "xdg-shell-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

struct state
{
    struct wl_compositor* comp;
    struct xdg_wm_base*   wm_base;
};

void
xdg_surface_configure(void*               data,
                      struct xdg_surface* xdg_surface,
                      uint32_t            serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
registry_global(void*               data,
                struct wl_registry* wl_registry,
                uint32_t            name,
                const char*         interface,
                uint32_t            version)
{
    struct state* state = data;
    printf("%s %d\n", interface, version);

    if (strcmp(interface, "wl_compositor") == 0) {
        state->comp =
          wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        state->wm_base =
          wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 5);
    }
}

static void
registry_global_remove(void* data, struct wl_registry* registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

int
main()
{
    struct state* state;
    state       = malloc(sizeof(*state));
    state->comp = NULL;

    struct wl_display* d = wl_display_connect("wayland-0");

    struct wl_registry* registry = wl_display_get_registry(d);
    wl_registry_add_listener(registry, &registry_listener, state);

    wl_display_roundtrip(d);
    if (!state->comp) {
        printf("no comp\n");
    }

    struct wl_surface* s = wl_compositor_create_surface(state->comp);
    if (!s) {
        printf("no s\n");
    }

    struct xdg_surface* xdg_surface =
      xdg_wm_base_get_xdg_surface(state->wm_base, s);
    xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
    if (!xdg_surface) {
        printf("no xdg surf\n");
    }

    struct xdg_toplevel* xdg_toplevel = xdg_surface_get_toplevel(xdg_surface);
    if (!xdg_toplevel) {
        printf("no xdg top\n");
    }

    wl_surface_commit(s);

    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);

    printf("hi");
    return 0;
}
