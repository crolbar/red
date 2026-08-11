#pragma once

#include "red.h"

// #define RED_DEBUG_TRACK_CLIENT_CREATION
// #define RED_DEBUG_TRACK_SURFACE_BUFS

int
init_compositor(struct redstate* rs);

int
red_on_frame_done(struct redstate* rs, uint32_t time_msec);

int
red_send_pending_callbacks(struct redsurface* rsurf, uint32_t time_msec);

int
red_send_toplevel_configure(struct redsurface* rsurf,
                            int                activated,
                            int                resizing);
int
red_send_popup_configure(struct redsurface* rsurf);
int
red_send_zwlr_layer_configure(struct redsurface* rsurf);

int
red_keyboard_send_enter(struct redsurface* rsurf);
int
red_keyboard_send_leave(struct redsurface* rsurf);
int
red_keyboard_send_leave_and_find_new(struct redsurface* rsurf);

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

int
red_current_buffer_deref(struct redsurface* rsurf);

int
red_current_buffer_release(struct redsurface* rsurf);

void
red_data_device_offer_selection(struct data_device* device,
                                struct data_source* source);
