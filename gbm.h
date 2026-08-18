#pragma once

#include "backend-drm.h"
#include "backend-wayland.h"
#include <EGL/eglext.h>

struct gbm_device*
init_gbm(int drm_fd);

EGLImageKHR
create_egl_image_from_gbm(EGLDisplay egl_display, struct gbm_bo* bo);

struct redbuffer*
init_drm_buffer(struct backend_drm* bd);

int
init_drm_cursor_buffer(struct backend_drm* bd);

struct redbuffer*
init_wl_buffer(struct backend_wayland* bw);

void
free_redbuffer(struct redbuffer* rb, EGLDisplay egl_display);
