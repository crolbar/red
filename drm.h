#pragma once

#include "red.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <xf86drmMode.h>

typedef struct redbuffer
{
    uint32_t buf_id;
    struct gbm_bo* gbm_bo;
    EGLImageKHR egl_image;
    GLuint rbo, fbo;
} redbuffer;

struct glProc
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
};

struct drmstate
{
    int fd;

    int width;
    int height;
    int crtc_id;
    uint32_t conn_id;
    drmModeModeInfo mode;
	drmModeModeInfoPtr modes;

    struct glProc* glProc;

    struct gbm_device* gbm_dev;
    struct redbuffer* rb0;
    struct redbuffer* rb1;
    int used_rb; // indicates which buffer is displayed

    bool page_flip_ready; // are we ready to render next frame

    bool gbm_has_modifier;
    EGLDisplay egl_display;
    EGLContext egl_context;
};

struct drmstate*
init_drm();

void
drm_handle_event(struct drmstate* drm);

int
drm_set_crct(struct drmstate* drm, uint32_t buf_id);

int
drm_flip(struct drmstate* drm, uint32_t buf_id, struct redstate* rs);
