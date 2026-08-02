#pragma once

#include "red.h"

// #define RED_DEBUG_TRACK_CLIENT_CREATION

int
init_compositor(struct redstate* rs);

int
red_on_frame_done(struct redstate* rs);

int
red_send_pending_callback(struct redsurface* rsurf);

int
red_send_configure(struct redsurface* rsurf, int activated, int resizing);

int
red_keyboard_send_enter(struct redclient* rc, struct wl_resource* wl_surface);
int
red_keyboard_send_leave(struct redclient* rc, struct wl_resource* wl_surface);

int
red_pointer_send_enter(struct redclient* rc, struct wl_resource* wl_surface);
int
red_pointer_send_leave(struct redclient* rc, struct wl_resource* wl_surface);

struct dmabuf*
red_get_dmabuf(struct wl_resource* resource);

void
handle_wl_log(const char* _fmt, va_list args);

uint32_t
red_get_scale(struct redsurface* rsurf);
