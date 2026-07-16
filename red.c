#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "drm.h"
#include "gbm.h"
#include "input.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "signals.h"
#include "time.h"
#include "vt.h"

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    struct redstate* rs;
    rs = malloc(sizeof(*rs));
    rs->sig_fd = -1;
    rs->tty_fd = -1;
    rs->li = NULL;
    rs->active = 1;
    rs->should_quit = 0;
    rs->rect_x = 0.0;
    rs->rect_y = 0.0;
    rs->time_start = time_get_now();
    rs->last_frame_time = time_get_elapsed_sec(rs->time_start);
    rs->is_wayland_client = false;
    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (getenv("WAYLAND_DISPLAY") ||
            strcmp(getenv("XDG_SESSION_TYPE"), "wayland") == 0) {
            rs->is_wayland_client = true;
            ROG_INFO("Spawning as wl client");
        }

    // drm device
    struct drmstate* drm;
    {
        drm = init_drm();
        if (!drm) {
            ret = 1;
            goto end;
        }
        rs->drm = drm;
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

    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (!rs->is_wayland_client) // setup VT
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

    // gbm device
    {
        drm->gbm_dev = init_gbm(drm->fd);
        if (!drm->gbm_dev) {
            ret = 1;
            goto end;
        }

        drm->glProc = init_gl_proc();
        if (!drm->glProc) {
            ret = 1;
            goto end;
        }
    }

    // egl display/context
    {
        if (init_egl(drm)) {
            ret = 1;
            goto end;
        }
    }

    drm->rb0 = init_drm_buffer(drm);
    if (!drm->rb0) {
        ret = 1;
        goto end;
    }
    drm->rb1 = init_drm_buffer(drm);
    if (!drm->rb1) {
        ret = 1;
        goto end;
    }

    // loop
    {
        {
            int pipefd[2];

            if (pipe(pipefd) == -1) {
                ROG_ERR("failed to create pipe: %s", strerror(errno));
                ret = 1;
                goto end;
            }

            rs->rrender_fd = pipefd[0];
            rs->wrender_fd = pipefd[1];
        }
        render_triggerI(rs->wrender_fd);

        struct pollfd fds[4];

        int li_fd = libinput_get_fd(rs->li);
        if (li_fd < 0) {
            ROG_ERR("failed get libinput fd: %s", strerror(errno));
            ret = 1;
            goto end;
        }
        fds[0].fd = li_fd;
        fds[0].events = POLLIN;
        fds[1].fd = rs->sig_fd;
        fds[1].events = POLLIN;
        fds[2].fd = rs->rrender_fd;
        fds[2].events = POLLIN;
        fds[3].fd = drm->fd;
        fds[3].events = POLLIN;

        ROG_INFO("Starting loop...");

        while (!rs->should_quit) {
            if (poll(fds, 4, -1) == -1) {
                ROG_ERR("poll fds error");
                ret = 1;
                goto end;
            }

            // page flip ready
            if (fds[3].revents & POLLIN) {
                drm_handle_event(drm);
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
                    render_triggerI(rs->wrender_fd);
                }
            }

            // input event
            if (fds[0].revents & POLLIN) {
                if (input_check_close(rs)) {
                    goto end;
                }
            }

            // render
            if (fds[2].revents & POLLIN) {
                if (!rs->active)
                    continue;

                int r = should_render_trigger(rs->rrender_fd);
                if (!r) {
                    continue;
                }

                {
                    double now = time_get_elapsed_sec(rs->time_start);
                    double dt = (now - rs->last_frame_time) * 1000;
                    rs->last_frame_time = now;
                    rs->frame_latency = dt;
                }

                redbuffer* rb = get_buffer(drm);

                render_frame(rs, rb);

                if (r == RENDER_TRIGGER_FLIP) {
                    if (drm_flip(drm, rb->buf_id, rs))
                        goto end;

                } else if (r == RENDER_TRIGGER_INIT) {
                    if (drm_set_crct(drm, rb->buf_id))
                        goto end;
                    render_trigger(rs->wrender_fd);
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (rs->drm->fd != -1)
        close(rs->drm->fd);

    if (rs->tty_fd != -1)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    free(rs->time_start);

    if (drm)
        free(drm);

    if (rs)
        free(rs);

    ROG_PRINT_CLOSE();
    return ret;
}
