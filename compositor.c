#include "compositor.h"
#include "dll.h"
#include "drm.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "wayland.h"
#include <libinput.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

// local cursor to the focused surface
// one surface means absolute cursor loc is the local one too!
double
red_get_lc_x(struct redstate* rs)
{
    // add decorations in account
    if (rs->focused_rt && rs->focused_rt->rsurf) {
        if (rs->focused_rt->rsurf->geom_configured)
            return rs->cursor_x + rs->focused_rt->rsurf->geom_x;
    }
    return rs->cursor_x;
}
double
red_get_lc_y(struct redstate* rs)
{
    if (rs->focused_rt && rs->focused_rt->rsurf) {
        if (rs->focused_rt->rsurf->geom_configured)
            return rs->cursor_y + rs->focused_rt->rsurf->geom_y;
    }
    return rs->cursor_y;
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
    if (rt) {
        if (rt->rc->wl_pointer)
            red_pointer_send_enter(rt->rc, rt->rsurf->wl_surface);

        if (rt->rc->wl_keyboard)
            red_keyboard_send_enter(rt->rc, rt->rsurf->wl_surface);

        if (rt->rsurf)
            red_send_configure(rt->rsurf, 1, 0);

        if (rs->backend->is_ready_for_frame(rs->backend->d))
            red_send_pending_callback(rt->rsurf);
    }

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
red_pointer_send_motion(struct redstate* rs,
                        uint32_t         time_msec,
                        double           x,
                        double           y)
{
    assert(!rs->is_wayland_client);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);
    x               = rs->cursor_x + x * 0.4;
    y               = rs->cursor_y + y * 0.4;
    rs->cursor_x    = max(min(x, (double)width), 0);
    rs->cursor_y    = max(min(y, (double)height), 0);

    if (rs->using_hardware_cursor) {
        if (drm_update_cursor_plane(rs))
            return 1;
    }
    // need to redraw the whole frame on software cursor
    else
        request_redraw(rs);

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
                        double                            value,
                        double                            value120,
                        enum libinput_pointer_axis_source source,
                        enum libinput_pointer_axis        axis)
{
    if (!rs->focused_rt || !rs->focused_rt->rc->wl_pointer)
        return 0;

    uint32_t axis_source;
    switch (source) {
        case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT:
            axis_source = WL_POINTER_AXIS_SOURCE_WHEEL_TILT;
            break;
        case LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS:
            axis_source = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
            break;
        case LIBINPUT_POINTER_AXIS_SOURCE_FINGER:
            axis_source = WL_POINTER_AXIS_SOURCE_FINGER;
            break;
        default:
        case LIBINPUT_POINTER_AXIS_SOURCE_WHEEL:
            axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
            break;
    };

    uint32_t wl_axis;
    if (axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
        wl_axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
    else
        wl_axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;

    wl_pointer_send_axis_source(rs->focused_rt->rc->wl_pointer, axis_source);

    wl_pointer_send_axis(rs->focused_rt->rc->wl_pointer,
                         time_msec,
                         wl_axis,
                         wl_fixed_from_double(value));

    if (value120 != 0)
        wl_pointer_send_axis_value120(
          rs->focused_rt->rc->wl_pointer, wl_axis, (int32_t)value120);

    return 0;
}
