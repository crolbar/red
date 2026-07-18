#pragma once

#include "backend.h"
#include <EGL/egl.h>
#include <stdint.h>

struct backend_wayland
{
    struct wayland_client* wc;
    EGLDisplay             egl_display;
    EGLContext             egl_context;
    struct gbm_device*     gbm_dev;
    struct redbuffer*      rb0;
    struct redbuffer*      rb1;
    uint32_t               used_rb; // indicates which buffer is displayed
    uint32_t               width, height;
};

extern struct backend backend_wayland;
