#include "compositor.h"
#include "log.h"
#include "red.h"
#include "render.h"

// on window close
int
red_destroy_toplevel(struct redstate* rs, struct redsurface* rsurf)
{
    if (rs->focused_toplevel == rsurf) {
        dll_for_each(rs->toplevels, v)
        {
            if (v->val == rsurf) {
                if (v->prev)
                    rs->focused_toplevel = v->prev->val;
                else if (v->next)
                    rs->focused_toplevel = v->next->val;
                else
                    rs->focused_toplevel = NULL;
            }
        }
    }

    dll_remove_val(rs->toplevels, rsurf);

    request_redraw(rs);
    return 0;
}

int
red_create_toplevel(struct redstate* rs, struct redsurface* rsurf)
{
    dll_push_tail(rs->toplevels, rsurf);
    rs->focused_toplevel = rsurf;

    // focus change requires redraw
    request_redraw(rs);
    return 0;
}
