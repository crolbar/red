#include "compositor.h"
#include "dll.h"
#include "drm.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "wayland.h"
#include <libinput.h>
#include <wayland-server.h>

// local cursor to the focused surface
// one surface means absolute cursor loc is the local one too!
double
red_get_lc_x(struct redstate* rs)
{
    // add decorations in account
    if (rs->focused_trc && rs->focused_trc->rsurf) {
        if (rs->focused_trc->rsurf->geom_configured)
            return rs->cursor_x + rs->focused_trc->rsurf->geom_x;
    }
    return rs->cursor_x;
}
double
red_get_lc_y(struct redstate* rs)
{
    if (rs->focused_trc && rs->focused_trc->rsurf) {
        if (rs->focused_trc->rsurf->geom_configured)
            return rs->cursor_y + rs->focused_trc->rsurf->geom_y;
    }
    return rs->cursor_y;
}

int
red_focus_trc(struct redstate* rs, struct redclient* trc)
{
    if (rs->focused_trc) {
        // pointer leave old trc
        if (rs->focused_trc->wl_pointer) {
            uint32_t serial = wl_display_next_serial(rs->wl_display);
            wl_pointer_send_leave(rs->focused_trc->wl_pointer,
                                  serial,
                                  rs->focused_trc->rsurf->wl_surface);
            wl_pointer_send_frame(rs->focused_trc->wl_pointer);
        }
        // keyboard leave old trc
        if (rs->focused_trc->wl_keyboard) {
            uint32_t serial = wl_display_next_serial(rs->wl_display);
            wl_keyboard_send_leave(rs->focused_trc->wl_keyboard,
                                   serial,
                                   rs->focused_trc->rsurf->wl_surface);
        }

        if (rs->focused_trc->rsurf)
            red_send_configure(rs->focused_trc->rsurf, 0, 0);
    }

    rs->focused_trc = trc;

    // trc can be null
    if (trc) {
        // pointer enter new trc
        if (trc->wl_pointer) {
            uint32_t serial = wl_display_next_serial(rs->wl_display);
            wl_pointer_send_enter(rs->focused_trc->wl_pointer,
                                  serial,
                                  trc->rsurf->wl_surface,
                                  red_get_lc_x(rs),
                                  red_get_lc_y(rs));
            wl_pointer_send_frame(trc->wl_pointer);
        }
        // keyboard enter new trc
        if (trc->wl_keyboard && trc->rsurf->wl_surface) {
            uint32_t        serial = wl_display_next_serial(rs->wl_display);
            struct wl_array keys;
            wl_array_init(&keys);
            wl_keyboard_send_enter(
              trc->wl_keyboard, serial, trc->rsurf->wl_surface, &keys);
            wl_array_release(&keys);
        }

        if (trc->rsurf)
            red_send_configure(trc->rsurf, 1, 0);
    }

    request_redraw(rs);
    return 0;
}

// on window close
int
red_destroy_trc(struct redstate* rs, struct redsurface* rsurf)
{
    struct redclient* rc = NULL;
    dll_for_each(rs->trcs, v)
    {
        // find our rc
        if (v->val->rsurf != rsurf)
            continue;
        rc = v->val;

        // change focused trc only if the destroyed one is on focus
        if (v->val != rs->focused_trc)
            break;

        struct redclient* new_focus = NULL;
        if (v->prev)
            new_focus = v->prev->val;
        else if (v->next)
            new_focus = v->next->val;

        red_focus_trc(rs, new_focus);
        break;
    }

    if (!rc)
        ROG_ERR("toplevel destroy: did not find trc in trcs list");
    else
        dll_remove_val(rs->trcs, rc);

    return 0;
}

int
red_create_trc(struct redstate*   rs,
               struct redsurface* rsurf,
               struct wl_client*  wl_client)
{

    struct redclient* rc = NULL;
    dll_for_each(rs->rcs, v)
    {
        if (v->val->wl_client == wl_client)
            rc = v->val;
    }
    if (!rc) {
        ROG_ERR("toplevel create: did not find rc in rcs list");
        return 1;
    }

    // NOTE: maybe move this on creation of wl_surface
    // not needed for now
    rc->rsurf = rsurf;
    dll_push_tail(rs->trcs, rc);

    // instantly focusing new toplevel
    red_focus_trc(rs, rc);

    return 0;
}

int
red_kb_send_keys(struct redstate* rs,
                 uint32_t         time_msec,
                 uint32_t         key,
                 int              press,
                 int              mods_have_changed)
{

    if (!rs->focused_trc)
        return 0;

    if (!rs->focused_trc->wl_keyboard)
        return 0;

    uint32_t serial = wl_display_next_serial(rs->wl_display);
    wl_keyboard_send_key(
      rs->focused_trc->wl_keyboard, serial, time_msec, key, press);

    if (mods_have_changed) {
        serial = wl_display_next_serial(rs->wl_display);
        wl_keyboard_send_modifiers(rs->focused_trc->wl_keyboard,
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
                        double           y,
                        int              delta)
{

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);
    if (delta) {
        x = rs->cursor_x + x * 0.4;
        y = rs->cursor_y + y * 0.4;
    }
    rs->cursor_x = max(min(x, (double)width), 0);
    rs->cursor_y = max(min(y, (double)height), 0);

    if (rs->using_hardware_cursor)
        if (drm_update_cursor_plane(rs))
            return 1;

    if (rs->focused_trc && rs->focused_trc->wl_pointer) {
        wl_pointer_send_motion(rs->focused_trc->wl_pointer,
                               time_msec,
                               wl_fixed_from_double(red_get_lc_x(rs)),
                               wl_fixed_from_double(red_get_lc_y(rs)));
        wl_pointer_send_frame(rs->focused_trc->wl_pointer);
    }

    // need to redraw the whole frame on software cursor
    if (!rs->is_wayland_client && !rs->using_hardware_cursor)
        request_redraw(rs);

    return 0;
}

int
red_pointer_send_button(struct redstate* rs,
                        uint32_t         time_msec,
                        uint32_t         button,
                        int              state)
{
    if (!rs->focused_trc || !rs->focused_trc->wl_pointer)
        return 0;

    uint32_t serial = wl_display_next_serial(rs->wl_display);
    wl_pointer_send_button(
      rs->focused_trc->wl_pointer, serial, time_msec, button, state);
    wl_pointer_send_frame(rs->focused_trc->wl_pointer);

    return 0;
}
int
red_pointer_send_scroll(struct redstate* rs,
                        uint32_t         time_msec,
                        double           val,
                        int              is_vertical_scroll,
                        int              is_finger)
{
    if (!rs->focused_trc || !rs->focused_trc->wl_pointer)
        return 0;

    if (is_finger)
        wl_pointer_send_axis_source(rs->focused_trc->wl_pointer,
                                    WL_POINTER_AXIS_SOURCE_FINGER);

    wl_pointer_send_axis(rs->focused_trc->wl_pointer,
                         time_msec,
                         (is_vertical_scroll)
                           ? WL_POINTER_AXIS_VERTICAL_SCROLL
                           : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                         wl_fixed_from_double(val));
    wl_pointer_send_frame(rs->focused_trc->wl_pointer);

    return 0;
}
