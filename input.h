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

int
input_kb_key(struct redstate* rs,
             uint32_t         time_msec,
             uint32_t         evdev_key,
             int              evdev_press);
