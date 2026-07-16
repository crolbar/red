#include "drm.h"
#include "gbm.h"
#include "log.h"
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <stdlib.h>

struct gbm_device*
init_gbm(int drm_fd)
{
    struct gbm_device* gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        ROG_ERR("failed to create gbm device");
        return NULL;
    }
    return gbm_dev;
}

int
init_egl(struct drmstate* drm)
{
    struct glProc* p = drm->glProc;

    EGLint major, minor;
    {
        drm->egl_display =
          p->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, drm->gbm_dev, NULL);
        if (drm->egl_display == EGL_NO_DISPLAY) {
            ROG_ERR("failed to get egl display: %x", eglGetError());
            return 1;
        }

        if (!eglInitialize(drm->egl_display, &major, &minor)) {
            ROG_ERR("failed to init egl: %x", eglGetError());
            return 1;
        }
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        ROG_ERR("failed to bind opengl es api: %x", eglGetError());
        return 1;
    };

    {
        EGLint attrs[] = {
            EGL_CONTEXT_MAJOR_VERSION,
            3,
            EGL_CONTEXT_MINOR_VERSION,
            2,
            EGL_NONE,
        };

        drm->egl_context = eglCreateContext(
          drm->egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (drm->egl_context == EGL_NO_CONTEXT) {
            ROG_ERR("failed to create egl context: %x", eglGetError());
            return 1;
        }

        if (!eglMakeCurrent(drm->egl_display,
                            EGL_NO_SURFACE,
                            EGL_NO_SURFACE,
                            drm->egl_context)) {
            ROG_ERR("eglMakeCurrent failed: %x", eglGetError());
            return 1;
        }
    }

    ROG_INFO("EGL version %d.%d", major, minor);
    ROG_INFO("GL version: %s", glGetString(GL_VERSION));
    return 0;
}

EGLImageKHR
_create_egl_image(struct drmstate* drm, struct gbm_bo* bo)
{
    int fd = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t offset = gbm_bo_get_offset(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t format = gbm_bo_get_format(bo);

    ROG("creating egl image w: %d, h: %d", width, height);

    int i = 0;
    EGLint attribs[32];

    {
        attribs[i++] = EGL_WIDTH;
        attribs[i++] = width;
        attribs[i++] = EGL_HEIGHT;
        attribs[i++] = height;
        attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
        attribs[i++] = format;

        attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        attribs[i++] = fd;
        attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        attribs[i++] = offset;
        attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        attribs[i++] = stride;

        if (drm->gbm_has_modifier) {
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribs[i++] = (EGLint)(modifier >> 32);
        }

        attribs[i++] = EGL_NONE;
    }

    EGLImageKHR image = drm->glProc->eglCreateImageKHR(
      drm->egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        ROG_ERR("failed to create egl image: %x", eglGetError());
        return NULL;
    }

    return image;
}

// using drm->width, drm->height
struct redbuffer*
init_drm_buffer(struct drmstate* drm)
{
    struct glProc* p = drm->glProc;

    struct gbm_bo* bo =
      gbm_bo_create(drm->gbm_dev,
                    drm->width,
                    drm->height,
                    GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!bo) {
        ROG_ERR("failed to create gbm_bo");
        goto fail;
    }

    // modifier
    {
        uint64_t modifier = gbm_bo_get_modifier(bo);
        if (!modifier) {
            ROG_ERR("failed to bo get modifier");
            goto fail;
        }
        drm->gbm_has_modifier =
          drm->gbm_has_modifier || modifier != DRM_FORMAT_MOD_INVALID;
    }

    // connect buffer to opengl
    EGLImageKHR egl_image = NULL;
    GLuint fbo = 0, rbo = 0;
    {
        egl_image = _create_egl_image(drm, bo);
        if (!egl_image) {
            goto fail;
        }

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);

        // attaching egl image to glRenderbuffer
        p->glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
                                                  (GLeglImageOES)egl_image);

        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        // attach render buffer to framebuffer object
        glFramebufferRenderbuffer(
          GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            ROG_ERR("glCheckFramebufferStatus failed: %x, status: %x",
                    glGetError(),
                    status);
            goto fail;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // connect buffer to drm
    uint32_t buf_id;
    {
        uint32_t handles[4] = { 0 };
        uint32_t pitches[4] = { 0 };
        uint32_t offsets[4] = { 0 };
        uint64_t modifiers[4] = { 0 };

        handles[0] = gbm_bo_get_handle(bo).u32;
        pitches[0] = gbm_bo_get_stride(bo);
        offsets[0] = gbm_bo_get_offset(bo, 0);
        modifiers[0] = gbm_bo_get_modifier(bo);
        uint32_t format = gbm_bo_get_format(bo);

        if (drmModeAddFB2WithModifiers(
              drm->fd,
              drm->width,
              drm->height,
              format,
              handles,
              pitches,
              offsets,
              (drm->gbm_has_modifier) ? modifiers : NULL,
              &buf_id,
              (drm->gbm_has_modifier) ? DRM_MODE_FB_MODIFIERS : 0)) {
            ROG_ERR("failed to submit buffer drmModeAddFB2");
            goto fail;
        }
    }

    struct redbuffer* rb;
    rb = malloc(sizeof(*rb));
    rb->fbo = fbo;
    rb->rbo = rbo;
    rb->gbm_bo = bo;
    rb->egl_image = egl_image;
    rb->buf_id = buf_id;

    return rb;
fail:

    return NULL;
}

// returns the unused buffer
struct redbuffer*
get_buffer(struct drmstate* drm)
{
    drm->used_rb = 1 - drm->used_rb;
    if (drm->used_rb)
        return drm->rb1;
    return drm->rb0;
}

struct glProc*
init_gl_proc()
{
    struct glProc* glProc;
    glProc = malloc(sizeof(*glProc));

    // get procs
    {
        glProc->eglGetPlatformDisplayEXT =
          (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
        if (!glProc->eglGetPlatformDisplayEXT) {
            ROG_ERR("did not found proc address of getPlatformDisplay");
            goto fail;
        }

        glProc->eglCreateImageKHR =
          (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        if (!glProc->eglCreateImageKHR) {
            ROG_ERR("did not found proc address of eglCreateImageKHR");
            goto fail;
        }

        glProc->glEGLImageTargetRenderbufferStorageOES =
          (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress(
            "glEGLImageTargetRenderbufferStorageOES");

        if (!glProc->glEGLImageTargetRenderbufferStorageOES) {
            ROG_ERR("did not found proc address of "
                    "glEGLImageTargetRenderbufferStorageOES");
            goto fail;
        }
    }

    return glProc;
fail:
    return NULL;
}
