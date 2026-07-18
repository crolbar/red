#include "backend-wayland-client.h"
#include "backend-wayland.h"
#include "linux-dmabuf-protocol.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "xdg-shell-client-protocol.h"
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-egl-core.h>

void
free_wayland(struct wayland_client* cws)
{
    if (cws->zwp_linux_dmabuf)
        zwp_linux_dmabuf_v1_destroy(cws->zwp_linux_dmabuf);
    if (cws->xdg_toplevel)
        xdg_toplevel_destroy(cws->xdg_toplevel);
    if (cws->xdg_surface)
        xdg_surface_destroy(cws->xdg_surface);
    if (cws->wl_surface)
        wl_surface_destroy(cws->wl_surface);
    if (cws->xdg_wm_base)
        xdg_wm_base_destroy(cws->xdg_wm_base);
    if (cws->wl_compositor)
        wl_compositor_destroy(cws->wl_compositor);
    if (cws->wl_registry)
        wl_registry_destroy(cws->wl_registry);
    if (cws->wl_display)
        wl_display_disconnect(cws->wl_display);
    if (cws)
        free(cws);
}

/* ======== wl_surface======== */

void
wl_frame_done(void*               data,
              struct wl_callback* wl_callback,
              uint32_t            callback_data)
{

    (void)callback_data;
    struct redstate* rs = data;

    wl_callback_destroy(wl_callback);
    redraw(rs);
}

/* ======== wl_buffer ======== */

void
wl_buffer_release(void* data, struct wl_buffer* wl_buffer)
{
    struct redbuffer* rb = data;
    rb->free             = 1;
}

/* ======== wl_registry ======== */

void
wl_registry_global(void*               data,
                   struct wl_registry* wl_registry,
                   uint32_t            name,
                   const char*         interface,
                   uint32_t            version)
{
    (void)version;
    struct wayland_client* cws = data;
    if (strcmp(interface, "wl_compositor") == 0) {
        cws->wl_compositor =
          wl_registry_bind(wl_registry, name, &wl_compositor_interface, 6);

    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        cws->xdg_wm_base =
          wl_registry_bind(wl_registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        cws->zwp_linux_dmabuf = wl_registry_bind(
          wl_registry, name, &zwp_linux_dmabuf_v1_interface, 4);
    }
}

void
wl_registry_global_remove(void*               data,
                          struct wl_registry* wl_registry,
                          uint32_t            name)
{
    (void)data;
    (void)wl_registry;
    (void)name;
}

/* ======== xdg_wm_base  ======== */

void
xdg_wm_base_listener_ping(void*               data,
                          struct xdg_wm_base* xdg_wm_base,
                          uint32_t            serial)
{
    (void)data;
    xdg_wm_base_pong(xdg_wm_base, serial);
}

/* ======== xdg_surface  ======== */

void
xdg_surface_configure(void*               data,
                      struct xdg_surface* xdg_surface,
                      uint32_t            serial)
{
    (void)data;
    xdg_surface_ack_configure(xdg_surface, serial);
}

/* ======== xdg_toplevel  ======== */

void
xdg_toplevel_configure(void*                data,
                       struct xdg_toplevel* xdg_toplevel,
                       int32_t              width,
                       int32_t              height,
                       struct wl_array*     states)
{
    (void)states;
    (void)xdg_toplevel;
    struct redstate*        rs = data;
    struct backend_wayland* bw = rs->backend->d;

    if (width <= 0 || height <= 0)
        return;

    if (bw->width != (uint32_t)width || bw->width != (uint32_t)height) {
        bw->rb0->needs_resize = 1;
        bw->rb1->needs_resize = 1;
    }
    bw->width  = width;
    bw->height = height;
}

void
xdg_toplevel_close(void* data, struct xdg_toplevel* xdg_toplevel)
{
    (void)xdg_toplevel;
    struct redstate* rs = data;
    rs->should_quit     = 1;
}

void
xdg_toplevel_configure_bounds(void*                data,
                              struct xdg_toplevel* xdg_toplevel,
                              int32_t              width,
                              int32_t              height)
{
    (void)data, (void)xdg_toplevel, (void)width, (void)height;
}

void
xdg_toplevel_wm_capabilities(void*                data,
                             struct xdg_toplevel* xdg_toplevel,
                             struct wl_array*     capabilities)
{
    (void)data, (void)xdg_toplevel, (void)capabilities;
}

struct wayland_client*
init_wayland()
{
    struct wayland_client* cws = NULL;
    cws                        = malloc(sizeof(*cws));
    if (!cws)
        goto fail;
    cws->wl_display       = NULL;
    cws->wl_compositor    = NULL;
    cws->wl_registry      = NULL;
    cws->wl_compositor    = NULL;
    cws->xdg_wm_base      = NULL;
    cws->xdg_surface      = NULL;
    cws->xdg_toplevel     = NULL;
    cws->zwp_linux_dmabuf = NULL;

    cws->wl_display = wl_display_connect(NULL);
    if (!cws->wl_display) {
        ROG_ERR("failed connecting to display");
        goto fail;
    }

    cws->wl_registry = wl_display_get_registry(cws->wl_display);
    if (!cws->wl_registry) {
        ROG_ERR("failed to get wl_registry");
        goto fail;
    }
    wl_registry_add_listener(cws->wl_registry, &wl_registry_listener, cws);
    wl_display_roundtrip(cws->wl_display);

    if (!cws->wl_compositor) {
        ROG_ERR("failed to get wl_compositor");
        goto fail;
    }
    if (!cws->xdg_wm_base) {
        ROG_ERR("failed to get xdg_wm_base");
        goto fail;
    }
    if (!cws->zwp_linux_dmabuf) {
        ROG_ERR("failed to get zwp_linux_dmabuf");
        goto fail;
    }
    xdg_wm_base_add_listener(cws->xdg_wm_base, &xdg_wm_base_listener, cws);
    cws->wl_surface = wl_compositor_create_surface(cws->wl_compositor);

    cws->xdg_surface =
      xdg_wm_base_get_xdg_surface(cws->xdg_wm_base, cws->wl_surface);
    xdg_surface_add_listener(cws->xdg_surface, &xdg_surface_listener, cws);

    cws->xdg_toplevel = xdg_surface_get_toplevel(cws->xdg_surface);

    wl_surface_commit(cws->wl_surface);

    return cws;
fail:
    free_wayland(cws);
    return NULL;
}
