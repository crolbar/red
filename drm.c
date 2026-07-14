#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm.h"
#include "input.h"
#include "log.h"
#include "signals.h"
#include "vt.h"


int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    struct drmstate* drm;
    drm = malloc(sizeof(*drm));
    drm->fd = -1;

    struct redstate* rs;
    rs = malloc(sizeof(*rs));
    rs->drm = drm;
    rs->sig_fd = -1;
    rs->tty_fd = -1;
    rs->li = NULL;
    rs->active = 1;
    rs->should_quit = 0;

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
    int vt_enabled;
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
    ROG_INFO("Opening /dev/dri/card0, first connected connector..");
    {
        int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
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

    {
        if (drmModeCreateDumbBuffer(drm->fd,
                                    drm->width,
                                    drm->height,
                                    32,
                                    0,
                                    &drm->handle,
                                    &drm->pitch,
                                    &drm->size) < 0) {
            ROG_ERR("failed create dumb buffer");
            ret = 1;
            goto end;
        }
    }

    {
        if (drmModeAddFB(drm->fd,
                         drm->width,
                         drm->height,
                         24,
                         32,
                         drm->pitch,
                         drm->handle,
                         &drm->buf_id)) {
            ROG_ERR("failed mode add fb");
            ret = 1;
            goto end;
        }
    }

    if (drmModeMapDumbBuffer(drm->fd, drm->handle, &drm->offset)) {
        ROG_ERR("failed map dumb buffer");
        ret = 1;
        goto end;
    }

    {
        drm->pixels = mmap(0,
                           drm->size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           drm->fd,
                           drm->offset);
        if (drm->pixels == MAP_FAILED) {
            ROG_ERR("failed mmmap");
            ret = 1;
            goto end;
        }
    }

    // draw something
    {
        for (int i = 0; i < drm->height; i++) {
            uint8_t* row = drm->pixels + i * drm->pitch;

            for (int j = 0; j < (int)drm->pitch; j += 4) {
                uint8_t* pixel = row + j;

                pixel[2] = 0x66;
                pixel[0] = 0x22;
                pixel[1] = 0x22;
                pixel[3] = 0;
            }
        }
        for (int y = 0; y < drm->height - 40; y++) {
            for (int x = 0; x < drm->width - 40; x++) {
                memset(drm->pixels + ((y + 20) * drm->pitch) + ((x + 20) * 4),
                       0x23,
                       4);
            }
        }
        // memset(pixels, 0x33, size);
    }

    if (drmModeSetCrtc(drm->fd,
                       drm->crtc_id,
                       drm->buf_id,
                       0,
                       0,
                       &drm->conn_id,
                       1,
                       &drm->mode))
        ROG_ERR("failed set crtc: %s", strerror(errno));

    // loop
    {
        struct pollfd fds[2];

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

        ROG_INFO("Starting loop...");

        while (!rs->should_quit) {
            if (poll(fds, 2, -1) == -1) {
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
                    if (drmModeSetCrtc(drm->fd,
                                       drm->crtc_id,
                                       drm->buf_id,
                                       0,
                                       0,
                                       &drm->conn_id,
                                       1,
                                       &drm->mode))
                        ROG_ERR("failed re-set crtc: %s", strerror(errno));
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (rs->drm->fd != -1)
        close(rs->drm->fd);

    if (rs->tty_fd != -1 && vt_enabled)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    ROG_PRINT_CLOSE();
    return ret;
}
