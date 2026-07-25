#pragma once

#include "backend.h"
#include <EGL/egl.h>
#include <stdint.h>

struct backend_wayland
{
    struct wayland_client* wc;
    int                    drm_fd;
    EGLDisplay             egl_display;
    EGLContext             egl_context;
    struct gbm_device*     gbm_dev;
    struct redbuffer*      rb0;
    struct redbuffer*      rb1;
    uint32_t               used_rb; // indicates which buffer is displayed
    uint32_t               width, height;
    int                    is_ready_for_frame;
};

extern struct backend backend_wayland;
