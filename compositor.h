#pragma once

#include "red.h"
#include <libinput.h>

int
red_focus_trc(struct redstate* rs, struct redclient* trc);

int
red_destroy_trc(struct redstate* rs, struct redsurface* rsurf);

int
red_create_trc(struct redstate* rs,
               struct redsurface* rsurf,
               struct wl_client* wl_client);

int
red_kb_send_keys(struct redstate* rs,
                 struct libinput_event_keyboard* kbe,
                 uint32_t key,
                 int press,
                 int mods_have_changed);

int
red_pointer_send_motion(struct redstate* rs, struct libinput_event_pointer* pe);

int
red_pointer_send_button(struct redstate* rs, struct libinput_event_pointer* pe);

int
red_pointer_send_scroll(struct redstate* rs,
                        struct libinput_event_pointer* pe,
                        int is_finger);

double
red_get_lc_x(struct redstate* rs);
double
red_get_lc_y(struct redstate* rs);
