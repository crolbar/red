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
        uint32_t scale = red_get_scale(rs->focused_rt->rsurf);

        // add geom of surface, because we remove it
        if (rs->focused_rt->rsurf->geom_configured)
            x += rs->focused_rt->rsurf->geom_x;

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

    if (rs->focused_rt && rs->focused_rt->rsurf) {
        uint32_t scale = red_get_scale(rs->focused_rt->rsurf);

        if (rs->focused_rt->rsurf->geom_configured)
            y += rs->focused_rt->rsurf->geom_y;

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
    double sx = rsurf->x * scale;
    double sy = rsurf->y * scale;
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
        red_keyboard_send_enter(rt->rsurf);

    if (rt->rsurf)
        red_send_toplevel_configure(rt->rsurf, 1, 0);

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
        if (frt->rc->wl_keyboard)
            red_keyboard_send_leave(frt->rsurf);

        if (frt->rsurf)
            red_send_toplevel_configure(frt->rsurf, 0, 0);
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
        rs->keyboard_focused_rsurf = NULL;
        rs->pointer_focused_rsurf  = NULL;
        rs->focused_rt             = NULL;
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
    ROG("creating toplevel rsurf: %d, client: %d", rsurf, wl_client);
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
    if (!rs->keyboard_focused_rsurf)
        return 0;
    assert(rs->keyboard_focused_rsurf->rc &&
           rs->keyboard_focused_rsurf->rc->wl_keyboard);

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
    if (!rs->is_wayland_client && !rs->cursor_hidden && rs->active)
        if (red_pointer_update_visibility(rs))
            return 1;

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
