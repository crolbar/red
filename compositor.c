#include "compositor.h"
#include "dll.h"
#include "log.h"
#include "red.h"
#include "render.h"

// on window close
int
red_destroy_trc(struct redstate* rs, struct redsurface* rsurf)
{
    struct redclient* rc             = NULL;
    int               is_rsurf_focus = 0;
    dll_for_each(rs->trcs, v)
    {
        if (v->val->rsurf == rsurf) {
            rc             = v->val;
            is_rsurf_focus = rc == rs->focused_trc;

            // change focused trc only if the destroyed one is on focus
            if (!is_rsurf_focus)
                break;

            if (v->prev)
                rs->focused_trc = v->prev->val;
            else if (v->next)
                rs->focused_trc = v->next->val;
            else
                rs->focused_trc = NULL;
            break;
        }
    }

    if (!rc)
        ROG_ERR("toplevel destroy: did not find trc in trcs list");
    else
        dll_remove_val(rs->trcs, rc);

    if (is_rsurf_focus)
        request_redraw(rs);

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
        ROG_ERR("toplevel create: did not find trc in rcs list");
        return 1;
    }

    // NOTE: maybe move this on creation of wl_surface
    // not needed for now
    rc->rsurf = rsurf;
    dll_push_tail(rs->trcs, rc);

    // instantly focusing new toplevel
    rs->focused_trc = rc;
    // focus change requires redraw
    request_redraw(rs);

    // TODO
    uint32_t        serial = wl_display_next_serial(rs->wl_display);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(rc->wl_keyboard, serial, rsurf->wl_surface, &keys);
    wl_array_release(&keys);

    return 0;
}
