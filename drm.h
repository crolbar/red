#pragma once

#include <stdint.h>
#include <xf86drmMode.h>

struct drmstate
{
    int fd;

    int width;
    int height;
    int crtc_id;
    uint32_t conn_id;
    drmModeModeInfo mode;

    uint32_t handle;
    uint32_t pitch;
    uint64_t size;

    uint32_t buf_id;

    uint64_t offset;

    uint8_t* pixels;
};

struct redstate
{
    struct drmstate* drm;
    struct libinput* li;
    int tty_fd;
    int sig_fd;

    int active; // VT is active

    int should_quit;
};
