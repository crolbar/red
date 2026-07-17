#pragma once

#include "red.h"
#include "drmProps.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <xf86drmMode.h>

struct glProc
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLCREATEIMAGEKHRPROC        eglCreateImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
};

struct drmstate
{
    int fd;

    int                width;
    int                height;
    uint32_t           crtc_id;
    int                crtc_idx;
    uint32_t           conn_id;
    uint32_t           plane_id;
    drmModeModeInfo    mode;
    drmModeModeInfoPtr modes;

    struct drmprops* props;

    struct gbm_device* gbm_dev;

    bool page_flip_ready; // are we ready to render next frame
    bool stop_flipping;

    bool       gbm_has_modifier;
    EGLDisplay egl_display;
    EGLContext egl_context;
};

struct drmstate*
init_drm();

int
init_drm_render();

void
drm_handle_event(struct drmstate* drm);

int
drm_handle_render_trigger(struct redstate* rs, uint32_t buf_id, int r);

int
drm_set_crct(struct drmstate* drm, uint32_t buf_id);

int
drm_flip(struct drmstate* drm, uint32_t buf_id, struct redstate* rs);
