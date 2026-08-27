#include "backend-drm.h"
#include "backend-wayland.h"
#include "backend.h"
#include "input.h"
#include "log.h"
#include "red.h"
#include "vt.h"
#include <stdlib.h>
#include <wayland-client-core.h>
#include <xf86drm.h>

int
init_backend(struct redstate* rs)
{
    char* wayland_display = NULL;
    if ((wayland_display = getenv("WAYLAND_DISPLAY")) != NULL) {
        // probe if connection can be made
        struct wl_display* d = wl_display_connect(wayland_display);
        // if connection fails try drm backend
        if (d == NULL)
            goto drm_backend;
        wl_display_disconnect(d);

        rs->backend                = &backend_wayland;
        struct backend_wayland* bw = backend_wayland_init_data();
        if (bw == NULL)
            goto fail;
        rs->backend->d = bw;

        if (backend_wayland_init(rs, bw)) {
            free(bw);
            goto fail;
        }
        setenv("RED_PARENT_WAYLAND_DISPLAY", wayland_display, 1);
    } else {
    drm_backend:
        rs->backend            = &backend_drm;
        struct backend_drm* bd = backend_drm_init_data();
        if (bd == NULL)
            goto fail;
        rs->backend->d = bd;

        if (!getenv("RED_DONT_SPAWN_CLIENT"))
            if (init_vt(rs)) {
                ROG_ERR("failed to initialize vt");
                free(bd);
                goto fail;
            }

        if (backend_drm_init(rs, bd)) {
            free(bd);
            goto fail;
        }

        if (!(rs->li = init_input(rs))) {
            ROG_ERR("failed to initialize libinput");
            free(bd);
            goto fail;
        }
    }

    rs->is_wayland_client = rs->backend == &backend_wayland;
    ROG_INFO("Using backend: %s", (rs->is_wayland_client) ? "wayland" : "drm");
    return 0;
fail:
    return 1;
}
