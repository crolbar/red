#pragma once

#include "backend-wayland.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

struct wayland_client
{
    struct wl_display*          wl_display;
    struct wl_registry*         wl_registry;
    struct wl_compositor*       wl_compositor;
    struct wl_surface*          wl_surface;
    struct wl_seat*             wl_seat;
    struct wl_keyboard*         wl_keyboard;
    struct wl_pointer*          wl_pointer;
    struct xdg_wm_base*         xdg_wm_base;
    struct xdg_surface*         xdg_surface;
    struct xdg_toplevel*        xdg_toplevel;
    struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf;
};

void
free_wayland(struct wayland_client* cws);

struct wayland_client*
init_wayland();

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
wl_surface_enter(void*              data,
                 struct wl_surface* wl_surface,
                 struct wl_output*  output);
void
wl_surface_leave(void*              data,
                 struct wl_surface* wl_surface,
                 struct wl_output*  output);
void
wl_surface_preferred_buffer_scale(void*              data,
                                  struct wl_surface* wl_surface,
                                  int32_t            factor);
void
wl_surface_preferred_buffer_transform(void*              data,
                                      struct wl_surface* wl_surface,
                                      uint32_t           transform);

static const struct wl_surface_listener wl_surface_listener = {
    .enter                      = wl_surface_enter,
    .leave                      = wl_surface_leave,
    .preferred_buffer_scale     = wl_surface_preferred_buffer_scale,
    .preferred_buffer_transform = wl_surface_preferred_buffer_transform,
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
wl_frame_done(void*               data,
              struct wl_callback* wl_callback,
              uint32_t            callback_data);

static const struct wl_callback_listener wl_frame_listener = {
    .done = wl_frame_done,
};

void
wl_keyboard_keymap(void*               data,
                   struct wl_keyboard* wl_keyboard,
                   uint32_t            format,
                   int32_t             fd,
                   uint32_t            size);
void
wl_keyboard_enter(void*               data,
                  struct wl_keyboard* wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface*  surface,
                  struct wl_array*    keys);
void
wl_keyboard_leave(void*               data,
                  struct wl_keyboard* wl_keyboard,
                  uint32_t            serial,
                  struct wl_surface*  surface);

void
wl_keyboard_repeat_info(void*               data,
                        struct wl_keyboard* wl_keyboard,
                        int32_t             rate,
                        int32_t             delay);
void
wl_keyboard_key(void*               data,
                struct wl_keyboard* wl_keyboard,
                uint32_t            serial,
                uint32_t            time,
                uint32_t            key,
                uint32_t            state);

void
wl_keyboard_modifiers(void*               data,
                      struct wl_keyboard* wl_keyboard,
                      uint32_t            serial,
                      uint32_t            mods_depressed,
                      uint32_t            mods_latched,
                      uint32_t            mods_locked,
                      uint32_t            group);

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap      = wl_keyboard_keymap,
    .enter       = wl_keyboard_enter,
    .leave       = wl_keyboard_leave,
    .repeat_info = wl_keyboard_repeat_info,
    .key         = wl_keyboard_key,
    .modifiers   = wl_keyboard_modifiers,
};

void
wl_pointer_enter(void*              data,
                 struct wl_pointer* wl_pointer,
                 uint32_t           serial,
                 struct wl_surface* surface,
                 wl_fixed_t         surface_x,
                 wl_fixed_t         surface_y);
void
wl_pointer_leave(void*              data,
                 struct wl_pointer* wl_pointer,
                 uint32_t           serial,
                 struct wl_surface* surface);
void
wl_pointer_motion(void*              data,
                  struct wl_pointer* wl_pointer,
                  uint32_t           time,
                  wl_fixed_t         surface_x,
                  wl_fixed_t         surface_y);
void
wl_pointer_button(void*              data,
                  struct wl_pointer* wl_pointer,
                  uint32_t           serial,
                  uint32_t           time,
                  uint32_t           button,
                  uint32_t           state);
void
wl_pointer_axis(void*              data,
                struct wl_pointer* wl_pointer,
                uint32_t           time,
                uint32_t           axis,
                wl_fixed_t         value);
void
wl_pointer_frame(void* data, struct wl_pointer* wl_pointer);
void
wl_pointer_axis_source(void*              data,
                       struct wl_pointer* wl_pointer,
                       uint32_t           axis_source);
void
wl_pointer_axis_stop(void*              data,
                     struct wl_pointer* wl_pointer,
                     uint32_t           time,
                     uint32_t           axis);
void
wl_pointer_axis_discrete(void*              data,
                         struct wl_pointer* wl_pointer,
                         uint32_t           axis,
                         int32_t            discrete);
void
wl_pointer_axis_value120(void*              data,
                         struct wl_pointer* wl_pointer,
                         uint32_t           axis,
                         int32_t            value120);
void
wl_pointer_axis_relative_direction(void*              data,
                                   struct wl_pointer* wl_pointer,
                                   uint32_t           axis,
                                   uint32_t           direction);

static const struct wl_pointer_listener wl_pointer_listener = {
    .enter                   = wl_pointer_enter,
    .leave                   = wl_pointer_leave,
    .motion                  = wl_pointer_motion,
    .button                  = wl_pointer_button,
    .axis                    = wl_pointer_axis,
    .frame                   = wl_pointer_frame,
    .axis_source             = wl_pointer_axis_source,
    .axis_stop               = wl_pointer_axis_stop,
    .axis_discrete           = wl_pointer_axis_discrete,
    .axis_value120           = wl_pointer_axis_value120,
    .axis_relative_direction = wl_pointer_axis_relative_direction,
};
