#pragma once

#include "drm.h"

struct gbm_device*
init_gbm(int drm_fd);

int
init_egl(struct gbm_device* gbm_dev,
         struct glProc*     p,
         EGLDisplay*        egl_display,
         EGLContext*        egl_context);

struct glProc*
init_gl_proc();

struct redbuffer*
init_drm_buffer(struct drmstate* drm, struct glProc* p);

struct redbuffer*
init_wl_buffer(struct client_wayland_state* cws, struct glProc* p);

struct redbuffer*
get_buffer(struct redstate* drm);
