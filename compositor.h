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
red_pointer_send_relative_motion(struct redstate* rs,
                                 uint64_t         time_usec,
                                 double           dx,
                                 double           dy,
                                 double           udx,
                                 double           udy);

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
struct redsurface*
red_get_rsurf_by_wl_surf(struct redstate* rs, struct wl_resource* wl_surface);

int
red_is_client_valid(struct redstate* rs, struct redclient* rc);

int
red_rt_send_enter(struct redstate* rs, struct redtoplevel* rt);

int
red_is_rsurf_focused(struct redstate* rs, struct redsurface* rsurf);

double
red_get_rsurf_x(struct redsurface* rsurf);
double
red_get_rsurf_y(struct redsurface* rsurf);
int
red_on_tick(struct redstate* rs);

int
red_handle_animation_frame_done(struct redstate* rs);

int
red_autoscroll_handle_click(struct redstate* rs, uint32_t button, int pressed);
int
red_autoscroll_handle_motion(struct redstate* rs);
int
red_autoscroll_update_timer(struct redstate* rs, uint32_t delay);

int
red_capture_focused_toplevel(struct redstate* rs);
int
red_capture_rsurf_to(struct redsurface* rsurf,
                     char*              path,
                     uint32_t           w,
                     uint32_t           h);

int
red_rsurf_is_scaling(struct redsurface* rsurf);
