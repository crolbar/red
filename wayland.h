#pragma once

#include "red.h"

int
init_compositor(struct redstate* rs);

int
red_send_pending_callback(struct redsurface* rsurf);

int
red_send_configure(struct redsurface* rsurf, int activated, int resizing);
