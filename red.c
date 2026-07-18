#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <poll.h>
#include <string.h>

#include "backend-drm.h"
#include "backend-wayland.h"
#include "gbm.h"
#include "input.h"
#include "log.h"
#include "red.h"
#include "signals.h"
#include "time.h"
#include "vt.h"

struct gl_proc* gl_proc = NULL;

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    struct redstate* rs;
    rs                    = malloc(sizeof(*rs));
    rs->sig_fd            = -1;
    rs->tty_fd            = -1;
    rs->li                = NULL;
    rs->active            = 1;
    rs->should_quit       = 0;
    rs->rect_x            = 0.0;
    rs->rect_y            = 0.0;
    rs->time_start        = time_get_now();
    rs->last_frame_time   = time_get_elapsed_sec(rs->time_start);
    rs->is_wayland_client = false;
    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (getenv("WAYLAND_DISPLAY") ||
            strcmp(getenv("XDG_SESSION_TYPE"), "wayland") == 0) {
            rs->is_wayland_client = true;
            ROG_INFO("Spawning as wl client");
        }
    rs->backend    = (rs->is_wayland_client) ? &backend_wayland : &backend_drm;
    rs->backend->d = rs->backend->init_data();

    {
        gl_proc = init_gl_proc();
        if (!gl_proc) {
            ret = 1;
            goto end;
        }
    }

    // backend
    if (rs->backend->init(rs)) {
        ret = 1;
        goto end;
    }

    // signals
    {
        int signal_fd = init_signals();
        if (signal_fd < 0) {
            ret = -1;
            goto end;
        }
        rs->sig_fd = signal_fd;
    }

    // VT
    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (!rs->is_wayland_client) //
        {
            int tty_fd = init_vt();
            if (tty_fd == -1) {
                ret = 1;
                goto end;
            }
            rs->tty_fd = tty_fd;
        }

    // libinput
    {
        struct libinput* li = init_input();
        if (!li) {
            ROG_ERR("failed to init libinput");
            ret = 1;
            goto end;
        }
        rs->li = li;
    }

    rs->backend->push_init_buffer(rs, rs->backend->pull_buffer(rs->backend->d));

    // loop
    {
        struct pollfd fds[3];

        int li_fd = libinput_get_fd(rs->li);
        if (li_fd < 0) {
            ROG_ERR("failed get libinput fd: %s", strerror(errno));
            ret = 1;
            goto end;
        }

        int backend_fd = rs->backend->get_fd(rs->backend->d);
        if (backend_fd < 0) {
            ret = 1;
            goto end;
        }

        fds[0].fd     = li_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = rs->sig_fd;
        fds[1].events = POLLIN;
        fds[2].fd     = backend_fd;
        fds[2].events = POLLIN;

        ROG_INFO("Starting loop...");
        while (!rs->should_quit) {
            rs->backend->flush_events(rs->backend->d);

            if (poll(fds, 3, -1) == -1) {
                ROG_ERR("poll fds error");
                ret = 1;
                goto end;
            }

            if (fds[2].revents & POLLIN || rs->is_wayland_client) {
                rs->backend->handle_events(rs->backend->d);
            }

            // signal
            if (fds[1].revents & POLLIN) {
                int prev_active = rs->active;

                if (handle_signal(rs) == -1) {
                    ret = 1;
                    goto end;
                }

                // redraw on aquire
                if (rs->active && prev_active != rs->active) {
                    rs->backend->push_init_buffer(
                      rs, rs->backend->pull_buffer(rs->backend->d));
                }
            }

            // input event
            if (fds[0].revents & POLLIN) {
                if (input_check_close(rs)) {
                    ret = 1;
                    goto end;
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    // if (rs->drm && rs->drm->fd != -1)
    //     close(rs->drm->fd);

    if (rs->tty_fd != -1)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    free(rs->time_start);

    // if (drm)
    //     free(drm);
    // if (rs->wl)
    //     free(rs->wl);

    if (rs)
        free(rs);

    ROG_PRINT_CLOSE();
    return ret;
}
