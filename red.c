#include <EGL/egl.h>
#include <assert.h>
#include <errno.h> // IWYU pragma: keep
#include <gbm.h>
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
#include "wayland-backend-client.h"

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
    rs->drm               = NULL;
    rs->wl                = NULL;
    rs->active            = 1;
    rs->should_quit       = 0;
    rs->rect_x            = 0.0;
    rs->rect_y            = 0.0;
    rs->time_start        = time_get_now();
    rs->last_frame_time   = time_get_elapsed_sec(rs->time_start);
    rs->is_wayland_client = false;
    rs->used_rb           = 0;
    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (getenv("WAYLAND_DISPLAY") ||
            strcmp(getenv("XDG_SESSION_TYPE"), "wayland") == 0) {
            rs->is_wayland_client = true;
            ROG_INFO("Spawning as wl client");
        }

    // drm device
    // struct drmstate* drm;
    // if (!rs->is_wayland_client) //
    // {
    //     drm = init_drm();
    //     if (!drm) {
    //         ret = 1;
    //         goto end;
    //     }
    //     rs->drm = drm;
    // }

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

    {
        rs->glProc = init_gl_proc();
        if (!rs->glProc) {
            ret = 1;
            goto end;
        }
    }

    // gbm device
    // {
    //     drm->gbm_dev = init_gbm(drm->fd);
    //     if (!drm->gbm_dev) {
    //         ret = 1;
    //         goto end;
    //     }

    //     drm->glProc = init_gl_proc();
    //     if (!drm->glProc) {
    //         ret = 1;
    //         goto end;
    //     }
    // }

    // egl display/context
    // {
    //     if (init_egl(drm)) {
    //         ret = 1;
    //         goto end;
    //     }
    // }

    // drm->rb0 = init_drm_buffer(drm);
    // if (!drm->rb0) {
    //     ret = 1;
    //     goto end;
    // }
    // drm->rb1 = init_drm_buffer(drm);
    // if (!drm->rb1) {
    //     ret = 1;
    //     goto end;
    // }

    {
        struct client_wayland_state* cws = init_wayland(rs);
        if (!cws)
            goto end;
        rs->wl = cws;

        // TODO
        rs->wl->egl_display = NULL;
        rs->wl->egl_context = NULL;
        rs->wl->gbm_dev     = NULL;
    }

    {

        int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            ROG_ERR("failed oppening drm device: %s", strerror(errno));
            return 1;
        }

        struct gbm_device* gbm_dev = init_gbm(fd);
        if (!gbm_dev) {
            goto end;
        }
        rs->wl->gbm_dev = gbm_dev;

        if (init_egl(rs->wl->gbm_dev,
                     rs->glProc,
                     &rs->wl->egl_display,
                     &rs->wl->egl_context)) {
            ret = 1;
            goto end;
        }

        rs->rb0 = init_wl_buffer(rs->wl, rs->glProc);
        rs->rb1 = init_wl_buffer(rs->wl, rs->glProc);

        wl_buffer_add_listener(
          rs->rb0->wl_buffer, &wl_buffer_listener, rs->rb0);
        wl_buffer_add_listener(
          rs->rb1->wl_buffer, &wl_buffer_listener, rs->rb1);
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
        fds[0].fd     = li_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = rs->sig_fd;
        fds[1].events = POLLIN;
        fds[2].fd     = rs->rrender_fd;
        fds[2].events = POLLIN;

        // if (!rs->is_wayland_client) {
        //     fds[3].fd     = drm->fd;
        //     fds[3].events = POLLIN;
        // }
        if (rs->is_wayland_client) {
            int fd = wl_display_get_fd(rs->wl->wl_display);
            if (fd < 0) {
                ROG_ERR("falied to get wl display fd");
                ret = 1;
                goto end;
            }

            fds[3].fd     = fd;
            fds[3].events = POLLIN;
        }
        wl_display_roundtrip(rs->wl->wl_display);
        ROG_INFO("Starting loop...");

        while (!rs->should_quit) {
            while (wl_display_prepare_read(rs->wl->wl_display) != 0)
                wl_display_dispatch_pending(rs->wl->wl_display);
            wl_display_flush(rs->wl->wl_display);

            if (poll(fds, 4, -1) == -1) {
                ROG_ERR("poll fds error");
                ret = 1;
                goto end;
            }

            wl_display_read_events(rs->wl->wl_display);
            wl_display_dispatch_pending(rs->wl->wl_display);

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
                    ret = 1;
                    goto end;
                }
            }

            // page flip ready
            // if (!rs->is_wayland_client)
            //     if (fds[3].revents & POLLIN) {
            //         drm_handle_event(drm);
            //     }
            if (fds[3].revents & POLLIN) {
                // render_frame(rs, NULL);
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
                    double now          = time_get_elapsed_sec(rs->time_start);
                    double dt           = (now - rs->last_frame_time) * 1000;
                    rs->last_frame_time = now;
                    rs->frame_latency   = dt;
                }

                redbuffer* rb = get_buffer(rs);
                // this rerender should be triggered by frame done
                // which should happen a whole lot after wl_buffer.release
                assert(rb->free);

                if (rb->needs_resize) {
                    wl_buffer_destroy(rb->wl_buffer);
                    glDeleteFramebuffers(1, &rb->fbo);
                    glDeleteRenderbuffers(1, &rb->rbo);
                    eglDestroyImage(rs->wl->egl_display, rb->egl_image);
                    gbm_bo_destroy(rb->gbm_bo);

                    struct redbuffer* new_buf =
                      init_wl_buffer(rs->wl, rs->glProc);
                    if (!new_buf) {
                        ret = 1;
                        goto end;
                    }

                    *rb = *new_buf;
                    free(new_buf);
                    wl_buffer_add_listener(
                      rb->wl_buffer, &wl_buffer_listener, rb);
                }

                render_frame(rs, rb);

                commit_buffer_wayland(rs, rb);
                // if (!rs->is_wayland_client)
                //     if (drm_handle_render_trigger(rs, rb->buf_id, r))
                //         goto end;
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (rs->drm && rs->drm->fd != -1)
        close(rs->drm->fd);

    if (rs->tty_fd != -1)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    free(rs->time_start);

    // if (drm)
    //     free(drm);
    if (rs->wl)
        free(rs->wl);

    if (rs)
        free(rs);

    ROG_PRINT_CLOSE();
    return ret;
}
