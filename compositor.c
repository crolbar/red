#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "log.h"
#include "red.h"
#include "relative-pointer-server-protocol.h"
#include "render.h"
#include "time.h"
#include "wayland.h"
#include <libinput.h>
#include <sys/timerfd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

// local cursor to the focused surface
double
red_get_lc_x(struct redstate* rs)
{
    double x = rs->cursor_x;

    // add decorations in account
    if (rs->focused_rt && rs->focused_rt->rsurf) {
        // add geom of surface, because we remove it
        if (rs->focused_rt->rsurf->geom_configured)
            x += rs->focused_rt->rsurf->geom_x;

        x /= red_get_scale(rs->focused_rt->rsurf);
    } else {
        x /= cfg.screen_scale;
    }

    return x;
}
double
red_get_lc_y(struct redstate* rs)
{
    double y = rs->cursor_y;

    if (rs->focused_rt && rs->focused_rt->rsurf) {
        if (rs->focused_rt->rsurf->geom_configured)
            y += rs->focused_rt->rsurf->geom_y;

        y /= red_get_scale(rs->focused_rt->rsurf);
    } else {
        y /= cfg.screen_scale;
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

int
red_is_rsurf_focused(struct redstate* rs, struct redsurface* rsurf)
{
    if (!rs->focused_rt)
        return 0;
    if (!rs->focused_rt->rsurf)
        return 0;

    if (!rsurf)
        return 0;

    return rs->focused_rt->rsurf == rsurf;
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
        red_keyboard_send_enter(rt->rc, rt->rsurf->wl_surface);

    if (rt->rsurf)
        red_send_configure(rt->rsurf, 1, 0);

    if (rs->backend->is_ready_for_frame(rs->backend->d))
        red_send_pending_callbacks(rt->rsurf, time_get_now_msec());

    return 0;
}

int
red_focus_rt(struct redstate* rs, struct redtoplevel* rt)
{
    // send leave on keyboard, pointer and surface to old focus
    struct redtoplevel* frt = rs->focused_rt;
    if (frt && red_is_client_valid(rs, frt->rc)) {
        if (frt->rc->wl_pointer)
            red_pointer_send_leave(frt->rc, frt->rsurf->wl_surface);

        if (frt->rc->wl_keyboard)
            red_keyboard_send_leave(frt->rc, frt->rsurf->wl_surface);

        if (frt->rsurf)
            red_send_configure(frt->rsurf, 0, 0);
    }
    frt = NULL;

    // send enter + frame callback on new focus
    red_rt_send_enter(rs, rt);

    rs->focused_rt = rt;

    if (!rt)
        request_redraw(rs);
    return 0;
}

// on window close
int
red_destroy_rt(struct redstate* rs, struct redtoplevel* rt)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("destroy rt %d(%s) %d", rt, rt->app_id, rt->rc->wl_client);
#endif
    // move focus to prev or next for now.
    // later we should do prev focus
    if (rs->focused_rt == rt) {
        dll_for_each(rs->rts, v)
        {
            if (v->val != rt)
                continue;

            if (v->prev)
                red_focus_rt(rs, v->prev->val);
            else if (v->next)
                red_focus_rt(rs, v->next->val);
            else
                red_focus_rt(rs, NULL);
            break;
        }
    }

    dll_remove_val(rs->rts, rt);

    free(rt->app_id);
    free(rt);
    return 0;
}

struct redtoplevel*
red_create_rt(struct redstate*   rs,
              struct redsurface* rsurf,
              struct wl_client*  wl_client)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating rt client: %d, rsurf: %d", wl_client, rsurf);
#endif

    struct redtoplevel* rt;
    rt = calloc(1, sizeof(*rt));
    assert(rt);

    struct redclient* rc = red_get_client(rs, wl_client);
    assert(rc);

    rt->rs     = rs;
    rt->rc     = rc;
    rt->rsurf  = rsurf;
    rt->app_id = NULL;

    dll_push_tail(rs->rts, rt);

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
    if (!rs->focused_rt)
        return 0;

    if (!rs->focused_rt->rc->wl_keyboard)
        return 0;

    uint32_t serial = wl_display_next_serial(rs->wl_display);
    wl_keyboard_send_key(
      rs->focused_rt->rc->wl_keyboard, serial, time_msec, key, press);

    if (mods_have_changed) {
        serial = wl_display_next_serial(rs->wl_display);
        wl_keyboard_send_modifiers(rs->focused_rt->rc->wl_keyboard,
                                   serial,
                                   rs->xkb_mods_depressed,
                                   rs->xkb_mods_latched,
                                   rs->xkb_mods_locked,
                                   rs->xkb_group);
    }

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
    if (rs->relative_pointers.size == 0)
        return 0;

    uint32_t time_hi = time_usec >> 32;
    uint32_t time_lo = time_usec & 0xffffffff;
    dll_for_each(rs->relative_pointers, v)
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
red_pointer_send_motion(struct redstate* rs, uint32_t time_msec)
{
    assert(!rs->is_wayland_client);

    if (!rs->cursor_hidden) {
        struct itimerspec its = {
            .it_value    = { .tv_sec = cfg.cursor_autohide_time / 1000,
                             .tv_nsec =
                               (cfg.cursor_autohide_time % 1000) * 1000 * 1000 },
            .it_interval = { 0, 0 },
        };
        timerfd_settime(rs->cursor_hide_timer, 0, &its, NULL);

        if (rs->using_hardware_cursor) {
            if (drm_update_cursor_plane(rs))
                return 1;
        }
        // need to redraw the whole frame on software cursor
        else
            request_redraw(rs);
    }

    // give some time between scroll and motion events to stop starvation
    if (time_msec - rs->cursor_last_scroll_time < 30)
        return 0;

    if (rs->focused_rt && rs->focused_rt->rc->wl_pointer) {
        wl_pointer_send_motion(rs->focused_rt->rc->wl_pointer,
                               time_msec,
                               wl_fixed_from_double(red_get_lc_x(rs)),
                               wl_fixed_from_double(red_get_lc_y(rs)));
    }
    return 0;
}

int
red_pointer_send_button(struct redstate* rs,
                        uint32_t         time_msec,
                        uint32_t         button,
                        int              state)
{
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return 0;

    uint32_t serial = wl_display_next_serial(rs->wl_display);
    wl_pointer_send_button(
      rs->focused_rt->rc->wl_pointer, serial, time_msec, button, state);

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
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return 0;
    struct wl_resource* pointer = rs->focused_rt->rc->wl_pointer;
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
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return 0;

    struct wl_resource* pointer = rs->focused_rt->rc->wl_pointer;
    int                 version = wl_resource_get_version(pointer);

    if (version < 5)
        return 0;

    wl_pointer_send_frame(pointer);
    return 0;
}
