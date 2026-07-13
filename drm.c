#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#include "input.c"

#include <drm.h>
#include <libinput.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

int
main(int argc, char** argv)
{
    // libinput device
    struct libinput* li = init_input();

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

    if (drmModeSetCrtc(fd, crtc_id, buf_id, 0, 0, &conn_id, 1, &mode)) {
        fprintf(stderr, "failed set crtc\n");
        return 1;
    }

    int running = 1;
    struct pollfd fds = { .fd = libinput_get_fd(li), .events = POLLIN };
    while (running) {
        poll(&fds, 1, -1);
        if (input_check_close(li)) {
            running = 0;
        }
    }

    printf("Closing section...\n");

    libinput_unref(li);
    return 0;
}
