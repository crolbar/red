#include "backend-wayland-client.h"
#include "backend-wayland.h"
#include "input.h"
#include "linux-dmabuf-client-protocol.h"
#include "log.h"
#include "render.h"
#include "time.h"
#include "wayland.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-server-protocol.h"
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-egl-core.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

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
wl_surface_enter(void*              data,
                 struct wl_surface* wl_surface,
                 struct wl_output*  output)
{
}
void
wl_surface_leave(void*              data,
                 struct wl_surface* wl_surface,
                 struct wl_output*  output)
{
}
void
wl_surface_preferred_buffer_scale(void*              data,
                                  struct wl_surface* wl_surface,
                                  int32_t            factor)
{
    struct redstate*        rs = data;
    struct backend_wayland* bw = rs->backend->d;
    bw->scale_factor           = factor;
}
void
wl_surface_preferred_buffer_transform(void*              data,
                                      struct wl_surface* wl_surface,
                                      uint32_t           transform)
{
}

void
wl_frame_done(void*               data,
              struct wl_callback* wl_callback,
              uint32_t            callback_data)
{

    (void)callback_data;
    struct redstate*        rs = data;
    struct backend_wayland* bw = rs->backend->d;

    bw->is_ready_for_frame = 1;
    red_on_frame_done(rs, time_get_now_msec());
    wl_callback_destroy(wl_callback);
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

    } else if (strcmp(interface, "wl_seat") == 0) {
        cws->wl_seat =
          wl_registry_bind(wl_registry, name, &wl_seat_interface, 5);

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
    struct redstate* rs = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    // just so server window is not stuck
    if (!rs->focused_rt)
        request_redraw(rs);
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

    if (bw->width != (uint32_t)width || bw->height != (uint32_t)height) {
        bw->rb0->needs_resize = 1;
        bw->rb1->needs_resize = 1;
    }

    bw->width  = width * bw->scale_factor;
    bw->height = height * bw->scale_factor;

    int       activated = 0;
    int       resizing  = 0;
    uint32_t* pos;
    wl_array_for_each(pos, states)
    {
        if (*pos == XDG_TOPLEVEL_STATE_ACTIVATED)
            activated = 1;
        if (*pos == XDG_TOPLEVEL_STATE_RESIZING)
            resizing = 1;
    }

    if (rs->focused_rt && rs->focused_rt->rsurf)
        red_send_configure(rs->focused_rt->rsurf, activated, resizing);
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

void
wl_keyboard_keymap(void*               data,
                   struct wl_keyboard* wl_keyboard,
                   uint32_t            format,
                   int32_t             fd,
                   uint32_t            size)
{
}
void
wl_keyboard_enter(void*               data,
                  struct wl_keyboard* wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface*  surface,
                  struct wl_array*    keys)
{
}
void
wl_keyboard_leave(void*               data,
                  struct wl_keyboard* wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface*  surface)
{
}

void
wl_keyboard_repeat_info(void*               data,
                        struct wl_keyboard* wl_keyboard,
                        int32_t             rate,
                        int32_t             delay)
{
}

void
wl_keyboard_key(void*               data,
                struct wl_keyboard* wl_keyboard,
                uint32_t            serial,
                uint32_t            time,
                uint32_t            key,
                uint32_t            state)
{
    struct redstate* rs = data;
    input_kb_key(rs, time, key, state);
}

void
wl_keyboard_modifiers(void*               data,
                      struct wl_keyboard* wl_keyboard,
                      uint32_t            serial,
                      uint32_t            mods_depressed,
                      uint32_t            mods_latched,
                      uint32_t            mods_locked,
                      uint32_t            group)
{
}

void
wl_pointer_enter(void*              data,
                 struct wl_pointer* wl_pointer,
                 uint32_t           serial,
                 struct wl_surface* surface,
                 wl_fixed_t         surface_x,
                 wl_fixed_t         surface_y)
{
}
void
wl_pointer_leave(void*              data,
                 struct wl_pointer* wl_pointer,
                 uint32_t           serial,
                 struct wl_surface* surface)
{
}
void
wl_pointer_motion(void*              data,
                  struct wl_pointer* wl_pointer,
                  uint32_t           time,
                  wl_fixed_t         surface_x,
                  wl_fixed_t         surface_y)
{
    struct redstate*        rs     = data;
    struct backend_wayland* bw     = rs->backend->d;
    uint32_t                width  = rs->backend->get_width(rs->backend->d);
    uint32_t                height = rs->backend->get_height(rs->backend->d);

    double x = wl_fixed_to_double(surface_x);
    double y = wl_fixed_to_double(surface_y);

    int32_t scale = 1;
    if (rs->focused_rt && rs->focused_rt->rsurf)
        scale = red_get_scale(rs->focused_rt->rsurf);
    if (scale == 1)
        scale = bw->scale_factor;
    else
        scale = 1;

    x *= scale;
    y *= scale;
    width *= scale;
    height *= scale;

    rs->cursor_x = max(min(x, (double)width), 0);
    rs->cursor_y = max(min(y, (double)height), 0);

    if (rs->focused_rt && rs->focused_rt->rc->wl_pointer)
        wl_pointer_send_motion(rs->focused_rt->rc->wl_pointer,
                               time,
                               wl_fixed_from_double(x),
                               wl_fixed_from_double(y));

    if (!rs->using_hardware_cursor)
        request_redraw(rs);
}
void
wl_pointer_button(void*              data,
                  struct wl_pointer* wl_pointer,
                  uint32_t           serial,
                  uint32_t           time,
                  uint32_t           button,
                  uint32_t           state)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    uint32_t _serial = wl_display_next_serial(rs->wl_display);
    wl_pointer_send_button(
      rs->focused_rt->rc->wl_pointer, _serial, time, button, state);
}
void
wl_pointer_axis(void*              data,
                struct wl_pointer* wl_pointer,
                uint32_t           time,
                uint32_t           axis,
                wl_fixed_t         value)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis(rs->focused_rt->rc->wl_pointer, time, axis, value);
}
void
wl_pointer_frame(void* data, struct wl_pointer* wl_pointer)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_frame(rs->focused_rt->rc->wl_pointer);
}
void
wl_pointer_axis_source(void*              data,
                       struct wl_pointer* wl_pointer,
                       uint32_t           axis_source)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis_source(rs->focused_rt->rc->wl_pointer, axis_source);
}
void
wl_pointer_axis_stop(void*              data,
                     struct wl_pointer* wl_pointer,
                     uint32_t           time,
                     uint32_t           axis)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis_stop(rs->focused_rt->rc->wl_pointer, time, axis);
}
void
wl_pointer_axis_discrete(void*              data,
                         struct wl_pointer* wl_pointer,
                         uint32_t           axis,
                         int32_t            discrete)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis_discrete(
      rs->focused_rt->rc->wl_pointer, axis, discrete);
}
void
wl_pointer_axis_value120(void*              data,
                         struct wl_pointer* wl_pointer,
                         uint32_t           axis,
                         int32_t            value120)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis_value120(
      rs->focused_rt->rc->wl_pointer, axis, value120);
}
void
wl_pointer_axis_relative_direction(void*              data,
                                   struct wl_pointer* wl_pointer,
                                   uint32_t           axis,
                                   uint32_t           direction)
{
    struct redstate* rs = data;
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return;
    wl_pointer_send_axis_relative_direction(
      rs->focused_rt->rc->wl_pointer, axis, direction);
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
    cws->wl_seat          = NULL;
    cws->xdg_toplevel     = NULL;
    cws->zwp_linux_dmabuf = NULL;
    cws->wl_keyboard      = NULL;
    cws->wl_pointer       = NULL;

    wl_log_set_handler_client(handle_wl_log);
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
    if (!cws->wl_seat) {
        ROG_ERR("failed to get wl_seat");
        goto fail;
    }
    cws->wl_keyboard = wl_seat_get_keyboard(cws->wl_seat);
    if (!cws->wl_keyboard) {
        ROG_ERR("failed to get wl_keyboard");
        goto fail;
    }
    cws->wl_pointer = wl_seat_get_pointer(cws->wl_seat);
    if (!cws->wl_pointer) {
        ROG_ERR("failed to get wl_pointer");
        goto fail;
    }

    xdg_wm_base_add_listener(cws->xdg_wm_base, &xdg_wm_base_listener, cws);
    cws->wl_surface = wl_compositor_create_surface(cws->wl_compositor);

    cws->xdg_surface =
      xdg_wm_base_get_xdg_surface(cws->xdg_wm_base, cws->wl_surface);

    cws->xdg_toplevel = xdg_surface_get_toplevel(cws->xdg_surface);

    return cws;
fail:
    free_wayland(cws);
    return NULL;
}
