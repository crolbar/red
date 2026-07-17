#pragma once

#include "red.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-egl-core.h>

struct client_wayland_state
{
    struct wl_display*          wl_display;
    struct wl_registry*         wl_registry;
    struct wl_compositor*       wl_compositor;
    struct wl_surface*          wl_surface;
    struct xdg_wm_base*         xdg_wm_base;
    struct xdg_surface*         xdg_surface;
    struct xdg_toplevel*        xdg_toplevel;
    struct wl_egl_window*       wl_egl_window;
    struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf;

    int width, height;

    EGLDisplay         egl_display;
    EGLContext         egl_context;
    struct gbm_device* gbm_dev;
};

void
free_wayland(struct client_wayland_state* cws);

struct client_wayland_state*
init_wayland(struct redstate* rs);

void
commit_buffer_wayland(struct redstate* rs, struct redbuffer* rb);

void
wl_registry_global(void*               data,
                   struct wl_registry* wl_registry,
                   uint32_t            name,
                   const char*         interface,
                   uint32_t            version);

void
wl_registry_global_remove(void*               data,
                          struct wl_registry* wl_registry,
                          uint32_t            name);

static const struct wl_registry_listener wl_registry_listener = {
    .global        = wl_registry_global,
    .global_remove = wl_registry_global_remove,
};

void
xdg_wm_base_listener_ping(void*               data,
                          struct xdg_wm_base* xdg_wm_base,
                          uint32_t            serial);

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_listener_ping,
};

void
xdg_surface_configure(void*               data,
                      struct xdg_surface* xdg_surface,
                      uint32_t            serial);

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

void
xdg_toplevel_configure(void*                data,
                       struct xdg_toplevel* xdg_toplevel,
                       int32_t              width,
                       int32_t              height,
                       struct wl_array*     states);

void
xdg_toplevel_close(void* data, struct xdg_toplevel* xdg_toplevel);

void
xdg_toplevel_configure_bounds(void*                data,
                              struct xdg_toplevel* xdg_toplevel,
                              int32_t              width,
                              int32_t              height);

void
xdg_toplevel_wm_capabilities(void*                data,
                             struct xdg_toplevel* xdg_toplevel,
                             struct wl_array*     capabilities);

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure        = xdg_toplevel_configure,
    .close            = xdg_toplevel_close,
    .configure_bounds = xdg_toplevel_configure_bounds,
    .wm_capabilities  = xdg_toplevel_wm_capabilities,
};

void
wl_buffer_release(void* data, struct wl_buffer* wl_buffer);

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

void
wl_frame_done(void* data, struct wl_callback* wl_callback, uint32_t callback_data);

static const struct wl_callback_listener wl_frame_listener = {
    .done = wl_frame_done,
};
