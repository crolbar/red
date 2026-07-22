#pragma once

#include "red.h"

struct libinput*
init_input();

int
input_dispatch(struct redstate* rs);

int
xkb_init_keyboard(struct redstate* rs);

int
xkb_destroy(struct redstate* rs);
