#include "compositor.h"
#include "dll.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include <wayland-server.h>

int
red_focus_trc(struct redstate* rs, struct redclient* trc)
{
    if (rs->focused_trc) {
        uint32_t serial = wl_display_next_serial(rs->wl_display);
        wl_keyboard_send_leave(rs->focused_trc->wl_keyboard,
                               serial,
                               rs->focused_trc->rsurf->wl_surface);
    }

    rs->focused_trc = trc;

    // trc can be null
    if (trc) {
        uint32_t        serial = wl_display_next_serial(rs->wl_display);
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(
          trc->wl_keyboard, serial, trc->rsurf->wl_surface, &keys);
        wl_array_release(&keys);
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
