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
    PFNEGLDESTROYIMAGEKHRPROC       eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    PFNEGLCREATESYNCKHRPROC           eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC          eglDestroySyncKHR;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;
};

int
init_gl_proc();

// TODO: disable in release build?
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
gl_add_fb(EGLImageKHR egl_image, GLuint* fbo, GLuint* rbo);

EGLImageKHR
init_egl_image(EGLDisplay           egl_display,
               uint32_t             width,
               uint32_t             height,
               uint32_t             format,
               uint32_t             planes_count,
               struct dmabuf_plane* planes);

int
gl_setup_cursor_program(struct redstate* rs);

int
gl_setup_program(struct redstate* rs);

int
gl_bind_texture_from_surface(struct redsurface* rsurf);

void
gl_destroy_surface_texture(struct redsurface* rsurf);

void
gl_destroy_egl_img(EGLDisplay egl_display, EGLImageKHR egl_img);

int
egl_create_sync_fd(EGLDisplay egl_display);

int
gl_read_tex_into(GLuint tex, uint8_t* buf, uint32_t w, uint32_t h);
