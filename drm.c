#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "input.h"
#include "log.h"

int
vt_set_mode(int fd, struct vt_mode mode)
{
    if (ioctl(fd, VT_SETMODE, &mode) == -1) {
        ROG_ERR("failed setting vt mode for %s: %s",
                mode.relsig != 0 ? "enable" : "disable",
                strerror(errno));
        return -1;
    }
    return 0;
}

int
vt_start(int fd)
{
    if (vt_set_mode(fd,
                    (struct vt_mode){
                      .mode = VT_PROCESS,
                      .waitv = 0,
                      .relsig = SIGUSR1,
                      .acqsig = SIGUSR2,
                      .frsig = 0,
                    }) == -1) {
        return -1;
    };

    if (ioctl(fd, KDSKBMODE, K_OFF) == -1) {
        ROG_ERR("failed settintg kd keyboard to off: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == -1) {
        ROG_ERR("failed setting kd mode to graphics: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
vt_stop(int fd)
{
    if (vt_set_mode(fd,
                    (struct vt_mode){
                      .mode = VT_AUTO,
                      .waitv = 0,
                      .relsig = 0,
                      .acqsig = 0,
                      .frsig = 0,
                    }) == -1) {
        return -1;
    };

    if (ioctl(fd, KDSKBMODE, K_UNICODE) == -1) {
        ROG_ERR("failed settintg kd keyboard to unicode: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, KDSETMODE, KD_TEXT) == -1) {
        ROG_ERR("failed setting kd mode to text: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
init_signals()
{
    sigset_t mask;

    sigemptyset(&mask);

    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        ROG_ERR("sigprocmask: %s", strerror(errno));
        return -1;
    }

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1) {
        ROG_ERR("signalfd: %s", strerror(errno));
        return -1;
    }

    return signal_fd;
}

// TODO remove active
int
handle_signal(int sfd, int tty_fd, int drm_fd, int* active)
{
    struct signalfd_siginfo si;
    ssize_t n = read(sfd, &si, sizeof(si));

    if (n != sizeof(si)) {
        perror("read signalfd");
        return -1;
    }

    switch (si.ssi_signo) {
        case SIGUSR1:
            ROG_INFO("Releasing drm_master and vt_display");

            if (drmDropMaster(drm_fd) == -1) {
                ROG_ERR("Could not drop master: %s", strerror(errno));
                return -1;
            }

            if (ioctl(tty_fd, VT_RELDISP, 1) == -1) {
                ROG_ERR("Could not ack VT release: %s", strerror(errno));
                return -1;
            }
            *active = false;
            break;

        case SIGUSR2:
            ROG_INFO("Acquiring drm_master and vt_display");

            if (drmSetMaster(drm_fd) == -1) {
                ROG_ERR("Could not set master: %s", strerror(errno));
                return -1;
            }

            if (ioctl(tty_fd, VT_RELDISP, VT_ACKACQ) == -1) {
                ROG_ERR("Could not ack VT acquire: %s", strerror(errno));
                return -1;
            }
            *active = true;
            break;

        case SIGINT:
            ROG_INFO("recived int");
            break;

        case SIGTERM:
            ROG_INFO("recived term");
            break;
    }

    return 0;
}

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = 0;

    int fd = -1; // drmfd
    int tty_fd = -1;
    struct libinput* li = NULL;
    int signal_fd = -1;
    int vt_enabled = 0;

    signal_fd = init_signals();
    if (signal_fd < 0) {
        ret = -1;
        goto end;
    }

    tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
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

    // libinput device
    li = init_input();

    // drm device
    ROG_INFO("Opening /dev/dri/card0, first connected connector..");
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        ret = 1;
        goto end;
    }

    {
        drmVersion* ver = drmGetVersion(fd);
        if (!ver) {
            ROG_ERR("drmGetVersion failed");
            ret = 1;
            goto end;
        }
        ROG_INFO("Using Driver: %s", ver->name);
        drmFreeVersion(ver);
    }

    int width;
    int height;
    int crtc_id;
    uint32_t conn_id;
    drmModeModeInfo mode;
    {
        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) {
            ret = 1;
            goto end;
        }

        drmModeConnector* conn = NULL;

        // get first found connected connector
        for (int i = 0; i < res->count_connectors; i++) {
            drmModeConnector* _conn =
              drmModeGetConnector(fd, res->connectors[i]);

            if (!_conn || _conn->connection != DRM_MODE_CONNECTED) {
                drmModeFreeConnector(_conn);
                continue;
            }

            conn = _conn;
            break;
        }

        drmModeEncoder* encoder = drmModeGetEncoder(fd, conn->encoder_id);
        crtc_id = encoder->crtc_id;
        drmModeFreeEncoder(encoder);

        conn_id = conn->connector_id;

        mode = conn->modes[0];

        width = conn->modes[0].hdisplay;
        height = conn->modes[0].vdisplay;
        ROG_INFO("Rendering at: %dx%d", width, height);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
    }

    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    {
        if (drmModeCreateDumbBuffer(
              fd, width, height, 32, 0, &handle, &pitch, &size) < 0) {
            ROG_ERR("failed create dumb buffer");
            ret = 1;
            goto end;
        }
    }

    uint32_t buf_id;
    {
        if (drmModeAddFB(fd, width, height, 24, 32, pitch, handle, &buf_id)) {
            ROG_ERR("failed mode add fb");
            ret = 1;
            goto end;
        }
    }

    uint64_t offset;
    if (drmModeMapDumbBuffer(fd, handle, &offset)) {
        ROG_ERR("failed map dumb buffer");
        ret = 1;
        goto end;
    }

    uint8_t* pixels;
    {
        pixels = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
        if (pixels == MAP_FAILED) {
            ret = 1;
            goto end;
        }
    }

    // draw something
    {
        for (int i = 0; i < height; i++) {
            uint8_t* row = pixels + i * pitch;

            for (int j = 0; j < (int)pitch; j += 4) {
                uint8_t* pixel = row + j;

                pixel[2] = 0x66;
                pixel[0] = 0x22;
                pixel[1] = 0x22;
                pixel[3] = 0;
            }
        }
        for (int y = 0; y < height - 40; y++) {
            for (int x = 0; x < width - 40; x++) {
                memset(pixels + ((y + 20) * pitch) + ((x + 20) * 4), 0x23, 4);
            }
        }
        // memset(pixels, 0x33, size);
    }

    if (drmModeSetCrtc(fd, crtc_id, buf_id, 0, 0, &conn_id, 1, &mode))
        ROG_ERR("failed set crtc: %s", strerror(errno));

    // loop
    {
        struct pollfd fds[2];

        fds[0].fd = libinput_get_fd(li);
        fds[0].events = POLLIN;
        fds[1].fd = signal_fd;
        fds[1].events = POLLIN;

        ROG_INFO("Starting loop...");

        int running = 1;
        int active = 1;
        while (running) {
            poll(fds, 2, -1);

            if (fds[0].revents & POLLIN) {
                if (input_check_close(li, tty_fd)) {
                    running = 0;
                }
            }

            if (fds[1].revents & POLLIN) {
                int prev_active = active;
                if (handle_signal(signal_fd, tty_fd, fd, &active) == -1) {
                    ret = 1;
                    goto end;
                }

                // redraw on aquire
                if (active && prev_active != active) {
                    if (drmModeSetCrtc(
                          fd, crtc_id, buf_id, 0, 0, &conn_id, 1, &mode))
                        ROG_ERR("failed re-set crtc: %s", strerror(errno));
                }
            }
        }
    }

end:
    ROG_WARN("Closing..");

    if (fd != -1)
        close(fd);
    if (tty_fd != -1 && vt_enabled)
        vt_stop(tty_fd);
    if (signal_fd != -1)
        close(signal_fd);
    if (li)
        libinput_unref(li);

    ROG_PRINT_CLOSE();
    return ret;
}
