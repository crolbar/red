#pragma once

#include "red.h"

int
init_compositor(struct redstate* rs);

int
red_on_frame_done(struct redstate* rs);

int
red_send_pending_callback(struct redsurface* rsurf);

int
red_send_configure(struct redsurface* rsurf, int activated, int resizing);

struct dmabuf*
red_get_dmabuf(struct wl_resource* resource);

void
handle_wl_log(const char* _fmt, va_list args);
