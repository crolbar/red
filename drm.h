#pragma once

#include <EGL/egl.h>
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

    struct gbm_device* gbm_dev;
    struct gbm_bo* gbm_bo; // current front buffer object

    EGLDisplay egl_display;
    EGLContext egl_context;
    uint32_t fb_id; // replaces buf_id name if you like
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
