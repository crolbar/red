#pragma once

#include "red.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

struct gl_proc
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLCREATEIMAGEKHRPROC        eglCreateImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
};

int
init_gl_proc();

#define CALL(_CALL)                                                            \
    do {                                                                       \
        (_CALL);                                                               \
        GLenum err = glGetError();                                             \
        if (err != GL_NO_ERROR) {                                              \
            ROG_ERR("gl err: 0x%x", err);                                      \
            goto fail;                                                         \
        }                                                                      \
    } while (0)

int
init_egl(struct gbm_device* gbm_dev,
         EGLDisplay*        egl_display,
         EGLContext*        egl_context);

int
gl_add_fb(struct gbm_bo* bo, EGLImageKHR egl_image, GLuint* fbo, GLuint* rbo);

EGLImageKHR
init_egl_image(EGLDisplay           egl_display,
               uint32_t             width,
               uint32_t             height,
               uint32_t             format,
               uint32_t             planes_count,
               struct dmabuf_plane* planes);

int
gl_setup_cursor_program(struct redstate* rs);
