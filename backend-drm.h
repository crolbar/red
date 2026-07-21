#pragma once

#include "backend.h"
#include <EGL/egl.h>
#include <stdint.h>
#include <xf86drmMode.h>

struct backend_drm
{
    int                drm_fd;
    uint32_t           crtc_id;
    int                crtc_idx;
    uint32_t           conn_id;
    uint32_t           plane_id;
    drmModeModeInfo    mode;
    int                gbm_has_modifier;
    EGLDisplay         egl_display;
    EGLContext         egl_context;
    struct drmprops*   props;
    struct gbm_device* gbm_dev;
    struct redbuffer*  rb0;
    struct redbuffer*  rb1;
    int                page_flip_ready; // are we ready to render next frame
    uint32_t           used_rb; // indicates which buffer is displayed
    uint32_t           width, height;
};

extern struct backend backend_drm;
