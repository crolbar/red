#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#include <linux/kd.h>
#include <sys/ioctl.h>

#include "input.c"

#include <drm.h>
#include <libinput.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libseat.h>
#include <syslog.h>

struct cdrm_data
{
    int active;
    int drm_fd;
    int tty_fd;
};

static void
handle_enable(struct libseat* backend, void* data)
{
    (void)backend;
    struct cdrm_data* cdrm_data = (struct cdrm_data*)data;
    cdrm_data->active++;
    openlog("red", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "running enable");

    if (drmSetMaster(cdrm_data->drm_fd)) {
        fprintf(stderr, "drmSetMaster failed: %s\n", strerror(errno));
        syslog(LOG_INFO, "drmSetMaster failed: %s\n", strerror(errno));
    }

    closelog();
}

static void
handle_disable(struct libseat* backend, void* data)
{
    openlog("red", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "running disable");
    closelog();

    (void)backend;
    struct cdrm_data* cdrm_data = (struct cdrm_data*)data;
    cdrm_data->active--;

    drmDropMaster(cdrm_data->drm_fd);

    libseat_disable_seat(backend);
}

int
main(int argc, char** argv)
{
    // libinput device
    struct libinput* li = init_input();

    openlog("red", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "startnig");

    // drm device
    if (argc < 2) {
        printf("no gpu selected\n");
        return 0;
    }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    // int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    {
        drmVersion* ver = drmGetVersion(fd);
        if (!ver) {
            fprintf(stderr, "drmGetVersion failed\n");
            close(fd);
            return 1;
        }
        printf("Using Driver: %s\n", ver->name);
        drmFreeVersion(ver);
    }

    // libseat
    struct libseat* ls;
    struct cdrm_data data = { .active = 0, .drm_fd = fd };
    struct libseat_seat_listener listener = {
        .enable_seat = handle_enable,
        .disable_seat = handle_disable,
    };
    {
        libseat_set_log_level(LIBSEAT_LOG_LEVEL_DEBUG);

        ls = libseat_open_seat(&listener, &data);
        fprintf(stderr,
                "libseat_open_seat(listener: %p, userdata: %p) = %p\n",
                (void*)&listener,
                (void*)&data,
                (void*)ls);
        if (ls == NULL) {
            fprintf(
              stderr, "libseat_open_seat() failed: %s\n", strerror(errno));
            return -1;
        }

        while (data.active == 0) {
            fprintf(stderr, "waiting for activation...\n");
            if (libseat_dispatch(ls, -1) == -1) {
                libseat_close_seat(ls);
                fprintf(
                  stderr, "libseat_dispatch() failed: %s\n", strerror(errno));
                return -1;
            }
        }
        fprintf(stderr, "active!\n");
    }

    int width;
    int height;
    int crtc_id;
    uint32_t conn_id;
    drmModeModeInfo mode;
    {
        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) {
            return 1;
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
        printf("Rendering at: %dx%d\n", width, height);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
    }

    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    {
        if (drmModeCreateDumbBuffer(
              fd, width, height, 32, 0, &handle, &pitch, &size) < 0) {
            fprintf(stderr, "failed create dumb buffer\n");
            return 1;
        }
    }

    uint32_t buf_id;
    {
        if (drmModeAddFB(fd, width, height, 24, 32, pitch, handle, &buf_id)) {
            fprintf(stderr, "failed mode add fb\n");
            return 1;
        }
    }

    uint64_t offset;
    if (drmModeMapDumbBuffer(fd, handle, &offset)) {
        fprintf(stderr, "failed map dumb buffer\n");
        return 1;
    }

    uint8_t* pixels;
    {
        pixels = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
        if (pixels == MAP_FAILED) {
            return 1;
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

    struct pollfd fds[2];

    fds[0].fd = libinput_get_fd(li);
    fds[0].events = POLLIN;

    fds[1].fd = libseat_get_fd(ls);
    fds[1].events = POLLIN;

    int was_active = 0;
    int running = 1;
    while (running) {
        if (data.active && !was_active) {
            // just came back from disabled -> enabled
            if (drmModeSetCrtc(fd, crtc_id, buf_id, 0, 0, &conn_id, 1, &mode))
                fprintf(stderr, "failed re-set crtc: %s\n", strerror(errno));

            // setting back to xlate to detect ctrl+alt+f*. libseat is setting
            // this to K_OFF disableing our keyboard
            {
                int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
                if (tty_fd < 0) {
                    perror("open");
                }

                if (ioctl(tty_fd, KDSKBMODE, K_XLATE) < 0) {
                    perror("KDSKBMODE K_XLATE");
                    return -1;
                }
            }
        }
        was_active = data.active;

        poll(fds, 2, -1);

        if (fds[1].revents & POLLIN) {
            while (libseat_dispatch(ls, 0) > 0)
                ;
        }

        if (fds[0].revents & POLLIN) {
            if (input_check_close(li)) {
                running = 0;
            }
        }
    }

    printf("Closing section...\n");

    close(fd);
    libinput_unref(li);
    libseat_close_seat(ls);

    closelog();
    return 0;
}
