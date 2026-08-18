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
    uint32_t               scale_factor;
    uint32_t               width, height;
    int                    is_ready_for_frame;
};

struct backend_wayland*
backend_wayland_init_data();

int
backend_wayland_init(struct redstate* rs, struct backend_wayland* bw);

extern struct backend backend_wayland;
