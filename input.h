#pragma once

#include "red.h"

struct libinput*
init_input(struct redstate* rs);

int
input_dispatch(struct redstate* rs);

int
init_xkb_keyboard(struct redstate* rs);

int
destroy_xkb(struct redstate* rs);

int
input_kb_key(struct redstate* rs,
             uint32_t         time_msec,
             uint32_t         evdev_key,
             int              evdev_press);

int
input_pointer_scroll(struct redstate*                  rs,
                     uint32_t                          time_msec,
                     enum libinput_pointer_axis        axis,
                     enum libinput_pointer_axis_source source,
                     double                            value,
                     double                            value120);
