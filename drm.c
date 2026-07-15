#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <poll.h>
#include <string.h>
#include <sys/signalfd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "config.h"
#include "drm.h"
#include "gbm.h"
#include "input.h"
#include "log.h"
#include "render.h"
#include "signals.h"
#include "vt.h"

// #define NO_VT

static void
page_flip_handler(int fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void* user_data)
{
    struct redstate* rs = user_data;
    rs->drm->page_flip_ready = true;
}

static drmEventContext drmevctx = {
    .version = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = page_flip_handler,
};

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    struct drmstate* drm;
    drm = malloc(sizeof(*drm));
    drm->fd = -1;
    drm->used_rb = 0;
    drm->gbm_has_modifier = false;

    struct redstate* rs;
    rs = malloc(sizeof(*rs));
    rs->drm = drm;
    rs->sig_fd = -1;
    rs->tty_fd = -1;
    rs->li = NULL;
    rs->active = 1;
    rs->should_quit = 0;
    rs->drm->page_flip_ready = true;
    rs->rect_x = 0.0;
    rs->rect_y = 0.0;

    // signals
    {
        int signal_fd = init_signals();
        if (signal_fd < 0) {
            ret = -1;
            goto end;
        }
        rs->sig_fd = signal_fd;
    }

    // setup VT
    // TODO: make init_vt
    int vt_enabled;
#ifdef NO_VT
    if (false)
#endif
    {
        int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty_fd < 0) {
            ROG_ERR("open tty: %s", strerror(errno));
            ret = 1;
            goto end;
        }

        if (vt_start(tty_fd) == -1) {
            ret = 1;
            goto end;
        }
        vt_enabled = 1;
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

    // drm device
    // TODO: init_drm
    ROG_INFO("Opening %s, first connected connector..", cfg.dri_dev);
    {
        int fd = open(cfg.dri_dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            ROG_ERR("failed oppening drm device: %s", strerror(errno));
            ret = 1;
            goto end;
        }
        rs->drm->fd = fd;
    }

    {
        drmVersion* ver = drmGetVersion(drm->fd);
        if (!ver) {
            ROG_ERR("drmGetVersion failed");
            ret = 1;
            goto end;
        }
        ROG_INFO("Using Driver: %s", ver->name);
        drmFreeVersion(ver);
    }

    {
        drmModeResPtr res = drmModeGetResources(drm->fd);
        if (!res) {
            ret = 1;
            goto end;
        }

        drmModeConnector* conn = NULL;

        // get first found connected connector
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector* _conn =
              drmModeGetConnector(drm->fd, res->connectors[i]);

            if (!_conn || _conn->connection != DRM_MODE_CONNECTED) {
                drmModeFreeConnector(_conn);
                continue;
            }

            conn = _conn;
            break;
        }

        drmModeEncoder* encoder = drmModeGetEncoder(drm->fd, conn->encoder_id);
        drm->crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);

        drm->conn_id = conn->connector_id;

        drm->mode = conn->modes[0];

        drm->width = conn->modes[0].hdisplay;
        drm->height = conn->modes[0].vdisplay;
        ROG_INFO("Rendering at: %dx%d", drm->width, drm->height);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
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

    drm->rb0 = init_buffer(drm);
    if (!drm->rb0) {
        ret = 1;
        goto end;
    }
    drm->rb1 = init_buffer(drm);
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

            // input event
            if (fds[0].revents & POLLIN) {
                if (input_check_close(rs)) {
                    goto end;
                }
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
                    if (drmModeSetCrtc(drm->fd,
                                       drm->crtc_id,
                                       (drm->used_rb) ? drm->rb1->buf_id
                                                      : drm->rb0->buf_id,
                                       0,
                                       0,
                                       &drm->conn_id,
                                       1,
                                       &drm->mode))
                        ROG_ERR("failed re-set crtc: %s", strerror(errno));
                }
            }

            if (fds[3].revents & POLLIN) {
                drmHandleEvent(drm->fd, &drmevctx);
            }

            // render
            if (fds[2].revents & POLLIN) {
                // TODO better
                if (!rs->drm->page_flip_ready)
                    continue;

                int r = should_render_trigger(rs->rrender_fd);
                if (!r) {
                    continue;
                }

                redbuffer* rb = get_buffer(drm);
                // ROG("rendering to buf %d, %d", rs->drm->used_rb, r)

                render_frame(rs, rb);

                if (r == RENDER_TRIGGER_FLIP) {
                    rs->drm->page_flip_ready = false;
                    if (drmModePageFlip(drm->fd,
                                        drm->crtc_id,
                                        rb->buf_id,
                                        DRM_MODE_PAGE_FLIP_EVENT,
                                        rs)) {
                        ROG_ERR("page flip failed: %s", strerror(errno));
                        ret = 1;
                        goto end;
                    }

                } else if (r == RENDER_TRIGGER_INIT) {
                    if (drmModeSetCrtc(drm->fd,
                                       drm->crtc_id,
                                       rb->buf_id,
                                       0,
                                       0,
                                       &drm->conn_id,
                                       1,
                                       &drm->mode))
                        ROG_ERR("failed set crtc: %s", strerror(errno));
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (rs->drm->fd != -1)
        close(rs->drm->fd);

#ifdef NO_VT
    if (false)
#endif
        if (rs->tty_fd != -1 && vt_enabled)
            vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    ROG_PRINT_CLOSE();
    return ret;
}
