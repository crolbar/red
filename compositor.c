#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "log.h" // IWYU pragma: keep
#include "opengl.h"
#include "red.h"
#include "relative-pointer-server-protocol.h"
#include "render.h"
#include "time.h"
#include "wayland.h"
#include <libinput.h>
#include <linux/input.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

// local cursor to the focused surface
double
red_get_lc_x(struct redstate* rs)
{
    double x = rs->cursor_x;

    if (rs->pointer_focused_rsurf) {
        uint32_t scale = 1;
        // TODO: better?
        if (rs->pointer_focused_rsurf->parent)
            scale = red_get_scale(rs->pointer_focused_rsurf->parent);
        else
            scale = red_get_scale(rs->pointer_focused_rsurf);

        // add geom of surface, because we remove it
        if (rs->pointer_focused_rsurf->xdg_toplevel &&
            rs->pointer_focused_rsurf->geom_configured)
            x += rs->pointer_focused_rsurf->geom_x;

        if (rs->pointer_focused_rsurf)
            x -= red_get_rsurf_x(rs->pointer_focused_rsurf);

        x /= scale;

    } else {
        x /= cfg.screen_scale;
    }

    return x;
}
double
red_get_lc_y(struct redstate* rs)
{
    double y = rs->cursor_y;

    if (rs->pointer_focused_rsurf) {
        uint32_t scale = 1;
        if (rs->pointer_focused_rsurf->parent)
            scale = red_get_scale(rs->pointer_focused_rsurf->parent);
        else
            scale = red_get_scale(rs->pointer_focused_rsurf);

        if (rs->pointer_focused_rsurf->xdg_toplevel &&
            rs->pointer_focused_rsurf->geom_configured)
            y += rs->pointer_focused_rsurf->geom_y;

        if (rs->pointer_focused_rsurf)
            y -= red_get_rsurf_y(rs->pointer_focused_rsurf);

        y /= scale;
    } else {
        y /= cfg.screen_scale;
    }
    return y;
}

double
red_get_rsurf_x(struct redsurface* rsurf)
{
    double   x     = rsurf->x;
    uint32_t scale = red_get_scale(rsurf);
    x *= scale;
    if (rsurf->geom_configured) {
        x -= rsurf->geom_x;
    }
    return x;
}
double
red_get_rsurf_y(struct redsurface* rsurf)
{
    double   y     = rsurf->y;
    uint32_t scale = red_get_scale(rsurf);
    y *= scale;
    if (rsurf->geom_configured) {
        y -= rsurf->geom_y;
    }
    return y;
}

struct redclient*
red_get_client(struct redstate* rs, struct wl_client* wl_client)
{
    dll_for_each(rs->rcs, v)
    {
        if (v->val->wl_client == wl_client)
            return v->val;
    }
    return NULL;
}
struct redclient*
red_get_client_by_rsurf(struct redstate* rs, struct redsurface* rsurf)
{
    dll_for_each(rs->rcs, v_rc)
    {
        dll_for_each(v_rc->val->rsurfs, v)
        {
            if (v->val == rsurf)
                return v_rc->val;
        }
    }
    return NULL;
}
struct redsurface*
red_get_rsurf_by_wl_surf(struct redstate* rs, struct wl_resource* wl_surface)
{
    dll_for_each(rs->rcs, v_rc)
    {
        dll_for_each(v_rc->val->rsurfs, v)
        {
            if (v->val->wl_surface == wl_surface)
                return v->val;
        }
    }
    return NULL;
}

// check if point at x,y is on rsurf, using x,y,w,h as dimentions
struct redsurface*
red_hit_test(struct redsurface* rsurf, double x, double y, uint32_t scale)
{
    double sx = rsurf->x * (int32_t)scale;
    double sy = rsurf->y * (int32_t)scale;
    double sw = rsurf->w;
    double sh = rsurf->h;

    if (x < sx)
        return NULL;
    if (y < sy)
        return NULL;
    if (x > sx + sw)
        return NULL;
    if (y > sy + sh)
        return NULL;

    return rsurf;
}

struct redsurface*
red_hit_test_r(struct redsurface* rsurf, double x, double y, uint32_t scale)
{
    // first go through subsurfs as they should be above
    dll_rfor_each(rsurf->subsurfs, v)
    {
        struct redsurface* r;
        if ((r = red_hit_test_r(v->val, x, y, scale)))
            return r;
    }

    return red_hit_test(rsurf, x, y, scale);
}

struct redsurface*
red_get_pointer_focused_rsurf(struct redstate* rs)
{
    if (!rs->focused_rt || !rs->focused_rt->rsurf)
        return NULL;

    double x = rs->cursor_x;
    double y = rs->cursor_y;

    // check other layers first, as all they should be above toplevel
    struct redsurface* ret = NULL;
    dll_rfor_each(rs->layer_rsurfs, v)
    {
        uint32_t scale = red_get_scale(v->val);
        if ((ret = red_hit_test_r(v->val, x, y, scale)))
            return ret;
    }

    uint32_t scale = red_get_scale(rs->focused_rt->rsurf);
    return red_hit_test_r(rs->focused_rt->rsurf, x, y, scale);
}

int
red_update_pointer_focused_rsurf(struct redstate* rs)
{
    struct redsurface* ht_rsurf = red_get_pointer_focused_rsurf(rs);
    if (ht_rsurf == NULL) {
        rs->pointer_focused_rsurf = NULL;
        return 0;
    }

    // focus has not changed, do nothing
    if (ht_rsurf == rs->pointer_focused_rsurf)
        return 0;

    // pointer focus should be on a surface with a registered wl_pointer
    if (ht_rsurf->rc->wl_pointer == NULL)
        return 0;

    if (rs->pointer_focused_rsurf)
        red_pointer_send_leave(rs->pointer_focused_rsurf->rc,
                               rs->pointer_focused_rsurf->wl_surface);
    red_pointer_send_enter(ht_rsurf->rc, ht_rsurf->wl_surface);

    rs->pointer_focused_rsurf = ht_rsurf;

    return 0;
}

int
red_is_click_on_overlay(struct redstate* rs)
{
    if (!rs->overlay_rt)
        return 0;
    if (!rs->overlay_rt->rsurf)
        return 0;

    return red_hit_test(rs->overlay_rt->rsurf,
                        rs->cursor_x,
                        rs->cursor_y,
                        red_get_scale(rs->overlay_rt->rsurf)) != NULL;
}

// return: 0 -> nothing happened, 1 -> should skip sending event
int
red_update_overlay_on_button(struct redstate* rs, uint32_t button, int state)
{
    if (!state) {
        // on release when in move mode exit mode
        if (rs->overlay_move_mode) {
            rs->overlay_move_mode = 0;
            return 1;
        }
        return 0;
    }

    if (rs->overlay_rt == rs->focused_rt)
        return 0;

    if (!red_is_click_on_overlay(rs))
        return 0;

    // we have click on the overlay

    // on click on overlay enter move mode
    if (button == BTN_LEFT)
        rs->overlay_move_mode = 1;
    else if (button == BTN_RIGHT)
        rs->overlay_move_mode = 2;
    else
        return 0;

    if (!rs->overlay_rt || !rs->overlay_rt->rsurf)
        return 1;

    // move
    if (rs->overlay_move_mode == 1) {
        rs->overlay_move_mode_diff_x =
          (rs->cursor_x / cfg.screen_scale) - rs->overlay_rt->rsurf->x;

        rs->overlay_move_mode_diff_y =
          (rs->cursor_y / cfg.screen_scale) - rs->overlay_rt->rsurf->y;
    }

    // TODO: fucked up when surf x or y is < 0
    // resize
    else if (rs->overlay_move_mode == 2) {
        rs->overlay_move_mode_diff_x =
          rs->overlay_rt->rsurf->w -
          (rs->cursor_x - rs->overlay_rt->rsurf->x * cfg.screen_scale);

        rs->overlay_move_mode_diff_y =
          rs->overlay_rt->rsurf->h -
          (rs->cursor_y - rs->overlay_rt->rsurf->y * cfg.screen_scale);
    }

    return 1;
}

int
red_update_overlay_on_motion(struct redstate* rs)
{
    // move
    if (rs->overlay_move_mode == 1) {
        rs->overlay_rt_x =
          (rs->cursor_x / cfg.screen_scale) - rs->overlay_move_mode_diff_x;

        rs->overlay_rt_y =
          (rs->cursor_y / cfg.screen_scale) - rs->overlay_move_mode_diff_y;

        if (rs->overlay_rt->rsurf) {
            rs->overlay_rt->rsurf->x = rs->overlay_rt_x;
            rs->overlay_rt->rsurf->y = rs->overlay_rt_y;
        }

        request_redraw(rs);
    }

    // resize
    else if (rs->overlay_move_mode == 2) {
        rs->overlay_rt_w =
          (rs->cursor_x - rs->overlay_rt_x * cfg.screen_scale) +
          rs->overlay_move_mode_diff_x;
        rs->overlay_rt_w = max(rs->overlay_rt_w, 0);

        rs->overlay_rt_h =
          (rs->cursor_y - rs->overlay_rt_y * cfg.screen_scale) +
          rs->overlay_move_mode_diff_y;
        rs->overlay_rt_h = max(rs->overlay_rt_h, 0);

        if (rs->overlay_rt->rsurf) {
            rs->overlay_rt->rsurf->w = rs->overlay_rt_w;
            rs->overlay_rt->rsurf->h = rs->overlay_rt_h;
            red_send_toplevel_configure(rs->overlay_rt->rsurf, 0, 1);
        }
    }

    return 0;
}

int
red_is_rsurf_focused(struct redstate* rs, struct redsurface* rsurf)
{
    if (!rs->focused_rt)
        return 0;
    if (!rs->focused_rt->rsurf)
        return 0;
    if (!rsurf)
        return 0;

    if (rsurf->parent)
        return red_is_rsurf_focused(rs, rsurf->parent);

    return rs->focused_rt->rsurf == rsurf ||
           (rs->overlay_rt && rsurf == rs->overlay_rt->rsurf);
}

// check if rc is not freed.
// we have some states where we could do a use after free on redclient.
int
red_is_client_valid(struct redstate* rs, struct redclient* rc)
{
    dll_for_each(rs->rcs, v)
    {
        if (v->val != rc)
            continue;

        return 1;
    }
    return 0;
}

inline double
red_get_animation_step(uint64_t frame_latency)
{
    return (double)1.0 / ((double)cfg.animation_focus_change_duration /
                          (double)frame_latency);
}

// 0 -> not animating
// > 1 -> request redraw, animating
int
red_handle_animation_frame_done(struct redstate* rs)
{
    if (rs->animation_value == 0) {
        return 0;
    }

    if (rs->animation_value == 1) {
        rs->animation_value = 0;
        return 0;
    }

    rs->animation_value += red_get_animation_step(rs->frame_latency);
    if (rs->animation_value > 1)
        rs->animation_value = 1;
    return 1;
}

int
red_rt_send_enter(struct redstate* rs, struct redtoplevel* rt)
{
    if (!rt)
        return 0;
    assert(rt->rc);
    assert(rt->rsurf);

    if (rt->rc->wl_pointer)
        red_pointer_send_enter(rt->rc, rt->rsurf->wl_surface);

    if (rt->rc->wl_keyboard)
        red_keyboard_send_enter(rt->rsurf);

    if (rt->rsurf && rt->rsurf->configured)
        red_send_toplevel_configure(rt->rsurf, 1, 0);

    if (rs->backend->is_ready_for_frame(rs->backend->d))
        red_send_pending_callbacks(rt->rsurf, time_get_now_msec());

    return 0;
}

int
red_focus_rt(struct redstate* rs, struct redtoplevel* rt)
{
    // did focus change
    if (rs->focused_rt != rt) {
        // send leave on keyboard, pointer and surface to old focus
        struct redtoplevel* frt = rs->focused_rt;
        if (frt && red_is_client_valid(rs, frt->rc)) {
            if (frt->rc->wl_keyboard)
                red_keyboard_send_leave(frt->rsurf);

            if (frt->rsurf && frt->rsurf->xdg_toplevel)
                red_send_toplevel_configure(frt->rsurf, 0, 0);
        }
        frt = NULL;

        if (rs->overlay_rt && rs->overlay_rt->rsurf) {
            // send resize configure to overlay
            if (rs->overlay_rt != rt) {
                rs->overlay_rt->rsurf->x = rs->overlay_rt_x;
                rs->overlay_rt->rsurf->y = rs->overlay_rt_y;
                rs->overlay_rt->rsurf->w = rs->overlay_rt_w;
                rs->overlay_rt->rsurf->h = rs->overlay_rt_h;
                red_send_toplevel_configure(rs->overlay_rt->rsurf, 0, 1);
            } else
            // if we are the overlay keep size normal
            {
                uint32_t w = rs->backend->get_width(rs->backend->d);
                uint32_t h = rs->backend->get_height(rs->backend->d);
                rs->overlay_rt->rsurf->x = 0;
                rs->overlay_rt->rsurf->y = 0;
                rs->overlay_rt->rsurf->w = w;
                rs->overlay_rt->rsurf->h = h;
            }
        }

        // send enter + frame callback on new focus
        red_rt_send_enter(rs, rt);

        rs->ipc_red_state_changes |= RED_STATE_RT_CHANGE_FOCUS;
    }
    rs->last_focused_rt = rs->focused_rt;
    rs->focused_rt      = rt;

    // (re)set val to step to start animating
    if (cfg.animations) {
        if (rs->last_focused_rt)
            rs->animation_value = red_get_animation_step(rs->frame_latency);
    }

    request_redraw(rs);
    return 0;
}

// on window close
int
red_destroy_rt(struct redstate* rs, struct redtoplevel* rt)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("destroy rt %d(%s), rsurf: %d, client: %d",
        rt,
        rt->app_id,
        rt->rsurf,
        rt->rc->wl_client);
#endif

    if (rt->rsurf)
        rt->rsurf->xdg_toplevel = NULL;

    if (rs->overlay_rt && rs->overlay_rt == rt)
        rs->overlay_rt = NULL;

    if (rs->last_focused_rt == rt)
        rs->last_focused_rt = NULL;

    if (rs->focused_rt == rt) {
        if (!rs->keyboard_focused_rsurf_exclusive)
            rs->keyboard_focused_rsurf = NULL;
        rs->pointer_focused_rsurf = NULL;
        rs->focused_rt            = NULL;

        // first if there is last focus, focus it
        if (rs->last_focused_rt) {
            red_focus_rt(rs, rs->last_focused_rt);
        }
        // if no last, focus prev neighbour
        else if (rs->rts.tail) {
            dll_for_each(rs->rts, v)
            {
                if (v->val == rt) {
                    if (v->prev)
                        red_focus_rt(rs, v->prev->val);
                    else
                        red_focus_rt(rs, NULL);
                    break;
                }
            }
        } else
            red_focus_rt(rs, NULL);
    }

    dll_remove_val(rs->rts, rt);
    rs->ipc_red_state_changes |= RED_STATE_RT_DESTROY;

    free(rt->app_id);
    free(rt->title);
    free(rt);
    return 0;
}

struct redtoplevel*
red_create_rt(struct redstate*   rs,
              struct redsurface* rsurf,
              struct wl_client*  wl_client)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating toplevel rsurf: %d, client: %d", rsurf, wl_client);
#endif

    struct redtoplevel* rt;
    rt = calloc(1, sizeof(*rt));
    assert(rt);

    uint32_t          w  = rs->backend->get_width(rs->backend->d);
    uint32_t          h  = rs->backend->get_height(rs->backend->d);
    struct redclient* rc = red_get_client(rs, wl_client);
    assert(rc);

    rt->rs       = rs;
    rt->rc       = rc;
    rt->rsurf    = rsurf;
    rt->app_id   = NULL;
    rt->title    = NULL;
    rt->rsurf->w = w;
    rt->rsurf->h = h;

    dll_push_tail(rs->rts, rt);
    rs->ipc_red_state_changes |= RED_STATE_RT_CREATE;

    // instantly focusing new toplevel
    red_focus_rt(rs, rt);

    return rt;
}

int
red_kb_send_keys(struct redstate* rs,
                 uint32_t         time_msec,
                 uint32_t         key,
                 int              press,
                 int              mods_have_changed)
{
    if (!rs->keyboard_focused_rsurf)
        return 0;
    assert(rs->keyboard_focused_rsurf->rc);

    if (!rs->keyboard_focused_rsurf->rc->wl_keyboard)
        return 0;

    wl_keyboard_send_key(rs->keyboard_focused_rsurf->rc->wl_keyboard,
                         wl_display_next_serial(rs->wl_display),
                         time_msec,
                         key,
                         press);

    if (mods_have_changed)
        wl_keyboard_send_modifiers(rs->keyboard_focused_rsurf->rc->wl_keyboard,
                                   wl_display_next_serial(rs->wl_display),
                                   rs->xkb_mods_depressed,
                                   rs->xkb_mods_latched,
                                   rs->xkb_mods_locked,
                                   rs->xkb_group);

    return 0;
}

int
red_pointer_send_relative_motion(struct redstate* rs,
                                 uint64_t         time_usec,
                                 double           dx,
                                 double           dy,
                                 double           udx,
                                 double           udy)
{
    if (rs->rel_pointers.size == 0)
        return 0;

    uint32_t time_hi = time_usec >> 32;
    uint32_t time_lo = time_usec & 0xffffffff;
    dll_for_each(rs->rel_pointers, v)
    {
        zwp_relative_pointer_v1_send_relative_motion(v->val,
                                                     time_hi,
                                                     time_lo,
                                                     wl_fixed_from_double(dx),
                                                     wl_fixed_from_double(dy),
                                                     wl_fixed_from_double(udx),
                                                     wl_fixed_from_double(udy));
    }
    red_pointer_send_frame(rs);
    return 0;
}

int
red_pointer_update_visibility(struct redstate* rs)
{
    struct itimerspec its = {
        .it_value    = { .tv_sec = cfg.cursor_autohide_time / 1000,
                         .tv_nsec =
                           (cfg.cursor_autohide_time % 1000) * 1000 * 1000 },
        .it_interval = { 0, 0 },
    };
    timerfd_settime(rs->cursor_hide_timer_fd, 0, &its, NULL);

    if (rs->using_hardware_cursor) {
        if (drm_update_cursor_plane(rs))
            return 1;
    }
    // need to redraw the whole frame on software cursor
    else
        request_redraw(rs);

    return 0;
}

int
red_pointer_send_motion(struct redstate* rs, uint32_t time_msec)
{
    if (!rs->is_wayland_client && !rs->cursor_hidden && rs->vt_active)
        if (red_pointer_update_visibility(rs))
            return 1;

    if (rs->overlay_move_mode) {
        red_update_overlay_on_motion(rs);
        return 0;
    }

    // give some time between scroll and motion events to stop starvation
    if (time_msec - rs->cursor_last_scroll_time < 30)
        return 0;

    if (red_update_pointer_focused_rsurf(rs))
        return 1;
    if (!rs->pointer_focused_rsurf)
        return 0;

    wl_pointer_send_motion(rs->pointer_focused_rsurf->rc->wl_pointer,
                           time_msec,
                           wl_fixed_from_double(red_get_lc_x(rs)),
                           wl_fixed_from_double(red_get_lc_y(rs)));

    return 0;
}

int
red_pointer_send_button(struct redstate* rs,
                        uint32_t         time_msec,
                        uint32_t         button,
                        int              state)
{
    if (red_update_overlay_on_button(rs, button, state))
        return 0;

    if (!rs->pointer_focused_rsurf)
        return 0;

    wl_pointer_send_button(rs->pointer_focused_rsurf->rc->wl_pointer,
                           wl_display_next_serial(rs->wl_display),
                           time_msec,
                           button,
                           state);
    return 0;
}

int
red_pointer_send_scroll(struct redstate*                  rs,
                        uint32_t                          time_msec,
                        enum libinput_pointer_axis        axis,
                        enum libinput_pointer_axis_source source,
                        double                            value,
                        double                            value120)
{
    if (!rs->pointer_focused_rsurf)
        return 0;

    struct wl_resource* pointer = rs->pointer_focused_rsurf->rc->wl_pointer;
    int                 version = wl_resource_get_version(pointer);

    uint32_t axis_source;
    switch (source) {
        case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
            axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
            break;
        case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
            axis_source = WL_POINTER_AXIS_SOURCE_FINGER;
            break;
        case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
            axis_source = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
            break;

        default:
        case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT:
            axis_source = WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
            break;
    };

    uint32_t wl_axis;
    if (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        wl_axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
    else
        wl_axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;

    wl_pointer_send_axis(
      pointer, time_msec, wl_axis, wl_fixed_from_double(value));

    if (version >= 5)
        wl_pointer_send_axis_source(pointer, axis_source);

    if (version >= 9)
        wl_pointer_send_axis_relative_direction(
          pointer, wl_axis, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);

    if (axis_source == WL_POINTER_AXIS_SOURCE_WHEEL) {
        if (value120 != 0) {
            if (version >= 8)
                wl_pointer_send_axis_value120(
                  pointer, wl_axis, (int32_t)value120);
        }
    } else if (value == 0)
        if (version >= 5)
            wl_pointer_send_axis_stop(pointer, time_msec, wl_axis);

    return 0;
}

int
red_pointer_send_frame(struct redstate* rs)
{
    if (!rs->pointer_focused_rsurf)
        return 0;

    struct wl_resource* pointer = rs->pointer_focused_rsurf->rc->wl_pointer;
    int                 version = wl_resource_get_version(pointer);

    if (version < 5)
        return 0;

    wl_pointer_send_frame(pointer);
    return 0;
}

int
red_on_tick(struct redstate* rs)
{
    uint64_t time_msec = time_get_now_msec();
    dll_for_each(rs->rts, rt)
    {
        if (rs->focused_rt == rt->val)
            continue;

        red_send_pending_callbacks(rt->val->rsurf, time_msec);
        if (rt->val->rsurf->current_buffer)
            wl_buffer_send_release(rt->val->rsurf->current_buffer);
        if (rt->val->rsurf->pending_buffer)
            wl_buffer_send_release(rt->val->rsurf->pending_buffer);

        dll_for_each(rt->val->rsurf->subsurfs, v)
        {
            red_send_pending_callbacks(v->val, time_msec);

            if (v->val->current_buffer)
                wl_buffer_send_release(v->val->current_buffer);
            if (v->val->pending_buffer)
                wl_buffer_send_release(v->val->pending_buffer);
        }
    }
    return 0;
}

int
red_autoscroll_update_timer(struct redstate* rs, uint32_t delay)
{
    struct timespec   ts  = { .tv_sec  = delay / 1000,
                              .tv_nsec = (delay % 1000) * 1000 * 1000 };
    struct itimerspec its = {
        .it_value    = ts,
        .it_interval = ts,
    };
    timerfd_settime(rs->autoscroll_fd, 0, &its, NULL);
    return 0;
}

// the biggest delay we can have. used when the littlest mouse
// movement happened between the start point and the current one.
#define AUTOSCROLL_BASE_DELAY       300.0f
#define AUTOSCROLL_NEEDED_INIT_DIST 20.0

int
red_autoscroll_handle_motion(struct redstate* rs)
{
    if (rs->autoscroll_fd == -1)
        return 0;
    assert(rs->autoscroll_point_y >= 0);

    int32_t  diff     = rs->cursor_y - rs->autoscroll_point_y;
    uint32_t abs_diff = abs(diff);

    // no need to update on such small changes
    if (abs_diff < 10)
        return 0;

    uint32_t height        = rs->backend->get_height(rs->backend->d);
    double   display_ratio = (double)abs_diff / (double)height;
    double   delay_scaler  = (1.0f - display_ratio);
    uint32_t delay =
      AUTOSCROLL_BASE_DELAY * pow(delay_scaler, cfg.autoscroll_expo);

    delay *= cfg.autoscroll_scale;
    delay = max(delay, 1);

    // only update timer initially, then updates
    // should be handled on the next timer hit
    if (rs->autoscroll_delay == 0)
        red_autoscroll_update_timer(rs, delay);
    rs->autoscroll_delay_changed = 1;
    rs->autoscroll_delay         = delay;
    rs->autoscroll_direction     = (diff > 0) ? 0 : 1;

    return 0;
}

int
red_autoscroll_handle_click(struct redstate* rs, uint32_t button, int pressed)
{
    if (BTN_MIDDLE == button && !pressed && rs->autoscroll_point_y != -1) {
        if (fabs(rs->cursor_y - rs->autoscroll_point_y) <=
            AUTOSCROLL_NEEDED_INIT_DIST)
            return 0;

        if (rs->autoscroll_fd != -1)
            close(rs->autoscroll_fd);

        rs->autoscroll_fd                = timerfd_create(CLOCK_MONOTONIC, 0);
        rs->pfds[RFD_AUTOSCROLL].fd      = rs->autoscroll_fd;
        rs->pfds[RFD_AUTOSCROLL].revents = 0;
        red_autoscroll_handle_motion(rs);
        return 0;
    }

    if (rs->autoscroll_fd != -1) {
        close(rs->autoscroll_fd);
        rs->autoscroll_fd                = -1;
        rs->autoscroll_point_y           = -1;
        rs->autoscroll_delay             = 0;
        rs->pfds[RFD_AUTOSCROLL].fd      = -1;
        rs->pfds[RFD_AUTOSCROLL].revents = 0;
        return 0;
    }

    if (button == BTN_MIDDLE && pressed)
        rs->autoscroll_point_y = rs->cursor_y;

    return 0;
}

// writes rgba `buf` - with dimentions `w` and `h - to `path` in ppm format.
int
red_write_rgba_buf_to_ppm(char* path, uint8_t* buf, uint32_t w, uint32_t h)
{
    uint32_t stride = w * 4;
    FILE*    f      = fopen(path, "wb");
    if (!f)
        return 1;

    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t i = 0; i < h; i++) {
        uint8_t out_buf[w * 3];
        int     out_j = 0;
        for (uint32_t j = 0; j < stride; j += 4) {
            out_buf[out_j++] = buf[i * stride + j + 0];
            out_buf[out_j++] = buf[i * stride + j + 1];
            out_buf[out_j++] = buf[i * stride + j + 2];
        }
        fwrite(out_buf, 3, w, f);
    }
    fputc('\n', f);
    fclose(f);
    return 0;
}

// creates a ppm image of `rsurf` in `path`
int
red_capture_rsurf_to(struct redsurface* rsurf, char* path)
{
    if (!rsurf)
        return 0;
    if (!rsurf->gl_tex)
        return 0;

    uint32_t w   = rsurf->w;
    uint32_t h   = rsurf->h;
    uint8_t* buf = calloc(h * w * 4, sizeof(*buf));
    if (gl_read_tex_into(rsurf->gl_tex, buf, w, h)) {
        ROG_ERR("error while reading gl texture into buf");
        return 1;
    }
    if (red_write_rgba_buf_to_ppm(path, buf, w, h))
        return 1;
    return 0;
}

int
red_capture_focused_toplevel(struct redstate* rs)
{
    if (!rs->focused_rt)
        return 0;

    if (red_capture_rsurf_to(rs->focused_rt->rsurf, "./red-capture.ppm"))
        return 1;

    return 0;
}
