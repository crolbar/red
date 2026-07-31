#pragma once

#include "red.h"
#include <libinput.h>

int
red_focus_rt(struct redstate* rs, struct redtoplevel* rt);

int
red_destroy_rt(struct redstate* rs, struct redtoplevel* rt);

struct redtoplevel*
red_create_rt(struct redstate*   rs,
              struct redsurface* rsurf,
              struct wl_client*  wl_client);

int
red_kb_send_keys(struct redstate* rs,
                 uint32_t         time_msec,
                 uint32_t         key,
                 int              press,
                 int              mods_have_changed);

int
red_pointer_send_motion(struct redstate* rs, uint32_t time_msec);

int
red_pointer_send_button(struct redstate* rs,
                        uint32_t         time_msec,
                        uint32_t         button,
                        int              state);

int
red_pointer_send_scroll(struct redstate*                  rs,
                        uint32_t                          time_msec,
                        enum libinput_pointer_axis        axis,
                        enum libinput_pointer_axis_source source,
                        double                            value,
                        double                            value120);

int
red_pointer_send_frame(struct redstate* rs);

double
red_get_lc_x(struct redstate* rs);
double
red_get_lc_y(struct redstate* rs);
struct redclient*
red_get_client(struct redstate* rs, struct wl_client* wl_client);
struct redclient*
red_get_client_by_rsurf(struct redstate* rs, struct redsurface* rsurf);

int
red_is_client_valid(struct redstate* rs, struct redclient* rc);
