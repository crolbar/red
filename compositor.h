#pragma once
#include "red.h"

int
red_destroy_trc(struct redstate* rs, struct redsurface* rsurf);

int
red_create_trc(struct redstate* rs, struct redsurface* rsurf, struct wl_client* wl_client);
