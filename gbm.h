#pragma once

#include "drm.h"

struct gbm_device*
init_gbm(int drm_fd);

int
init_egl(struct drmstate* drm);

struct glProc*
init_gl_proc();

struct redbuffer*
init_drm_buffer(struct drmstate* drm);

struct redbuffer*
get_buffer(struct drmstate* drm);
