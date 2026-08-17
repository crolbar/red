#include "backend-drm.h"
#include "log.h"
#include "red.h"
#include <errno.h> // IWYU pragma: keep
#include <libseat.h>
#include <string.h>
#include <xf86drm.h>

static void
enable_seat(struct libseat* seat, void* data)
{
    struct redstate* rs          = data;
    int              prev_active = rs->vt_active;
    rs->vt_active                = true;

    ROG_INFO("Acquiring drm_master and vt_display");
    struct backend_drm* bd = rs->backend->d;

    if (libinput_resume(rs->li)) {
        ROG_ERR("Could not resume libinput context");
        return;
    }

    // redraw on aquire
    if (rs->vt_active && prev_active != rs->vt_active) {
        rs->backend->push_init_buffer(rs);
        ROG("redraw on quire")
    }
}

static void
disable_seat(struct libseat* seat, void* data)
{
    struct redstate* rs = data;
    rs->vt_active       = false;
    ROG_INFO("Releasing drm_master and vt_display!!!!");

    struct backend_drm* bd = rs->backend->d;
    rs->vt_active          = 0;

    while (!bd->page_flip_ready) {
        rs->backend->handle_events(bd);
    }

    struct libinput_event* ev;
    while ((ev = libinput_get_event(rs->li))) {
        libinput_event_destroy(ev);
    }
    libinput_suspend(rs->li);

    libseat_disable_seat(rs->ls);
}

static const struct libseat_seat_listener listener = {
    .disable_seat = disable_seat,
    .enable_seat  = enable_seat,
};

int
vt_switch(struct redstate* rs, int n)
{
    if (libseat_switch_session(rs->ls, n) == -1) {
        ROG_ERR("failed switching vt: %s", strerror(errno));
        return 1;
    }
    return 0;
}

int
vt_stop(struct redstate* rs)
{
    libseat_close_seat(rs->ls);
    rs->seat_name = NULL;
    return 0;
}
static void
log_func(enum libseat_log_level level, const char* format, va_list args)
{
    ROG_ERR_VARGS(format, args);
}

int
init_vt(struct redstate* rs)
{
    if ((rs->ls = libseat_open_seat(&listener, rs)) == NULL) {
        ROG_ERR("failed to open seat with err: %s", strerror(errno));
        goto fail;
    }

    if ((rs->ls_fd = libseat_get_fd(rs->ls)) == -1) {
        ROG_ERR("failed to open get seat fd with err: %s", strerror(errno));
        goto fail;
    }

    rs->seat_name = libseat_seat_name(rs->ls);

    libseat_set_log_handler(log_func);
    return 0;
fail:
    return 1;
}
