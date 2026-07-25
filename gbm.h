#pragma once

#include "backend-drm.h"
#include "backend-wayland.h"

struct gbm_device*
init_gbm(int drm_fd);

int
init_egl(struct gbm_device* gbm_dev,
         EGLDisplay*        egl_display,
         EGLContext*        egl_context);

struct gl_proc*
init_gl_proc();

struct redbuffer*
init_drm_buffer(struct backend_drm* bd);

int
init_drm_cursor_buffer(struct backend_drm* bd);

struct redbuffer*
init_wl_buffer(struct backend_wayland* bw);
