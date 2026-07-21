#pragma once

#include "red.h"

int
init_compositor(struct redstate* rs);

int
wl_send_pending_callback(struct redsurface* rsurf);
