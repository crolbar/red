#include "backend-drm.h"
#include "backend-wayland-client.h"
#include "backend-wayland.h"
#include "gbm.h"
#include "linux-dmabuf-protocol.h"
#include "log.h"
#include "red.h"
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <stdlib.h>
#include <wayland-client-protocol.h>

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

// NOTE: pass pointers to egl_display and context
int
init_egl(struct gbm_device* gbm_dev,
         EGLDisplay*        egl_display,
         EGLContext*        egl_context)
{
    EGLint major, minor;
    {
        *egl_display = gl_proc->eglGetPlatformDisplayEXT(
          EGL_PLATFORM_GBM_KHR, gbm_dev, NULL);
        if (*egl_display == EGL_NO_DISPLAY) {
            ROG_ERR("failed to get egl display: %x", eglGetError());
            return 1;
        }

        if (!eglInitialize(*egl_display, &major, &minor)) {
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

        *egl_context = eglCreateContext(
          *egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (*egl_context == EGL_NO_CONTEXT) {
            ROG_ERR("failed to create egl context: %x", eglGetError());
            return 1;
        }

        if (!eglMakeCurrent(
              *egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, *egl_context)) {
            ROG_ERR("eglMakeCurrent failed: %x", eglGetError());
            return 1;
        }
    }

    ROG_INFO("EGL version %d.%d", major, minor);
    ROG_INFO("GL version: %s", glGetString(GL_VERSION));
    return 0;
}

EGLImageKHR
_create_egl_image(EGLDisplay     egl_display,
                  struct gbm_bo* bo,
                  int            gbm_has_modifier)
{
    int      fd       = gbm_bo_get_fd(bo);
    uint32_t stride   = gbm_bo_get_stride(bo);
    uint32_t offset   = gbm_bo_get_offset(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t width    = gbm_bo_get_width(bo);
    uint32_t height   = gbm_bo_get_height(bo);
    uint32_t format   = gbm_bo_get_format(bo);

    // ROG("creating egl image w: %d, h: %d", width, height);

    int    i = 0;
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

        if (gbm_has_modifier) {
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            attribs[i++] = (EGLint)(modifier & 0xFFFFFFFF);
            attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            attribs[i++] = (EGLint)(modifier >> 32);
        }

        attribs[i++] = EGL_NONE;
    }

    EGLImageKHR image = gl_proc->eglCreateImageKHR(
      egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (image == EGL_NO_IMAGE_KHR) {
        ROG_ERR("failed to create egl image: %x", eglGetError());
        return NULL;
    }
    close(fd);

    return image;
}

struct redbuffer*
init_wl_buffer(struct backend_wayland* bw)
{
    EGLImageKHR       egl_image = EGL_NO_IMAGE_KHR;
    GLuint            fbo = 0, rbo = 0;
    struct wl_buffer* wl_buffer = NULL;

    struct gbm_bo* bo = gbm_bo_create(bw->gbm_dev,
                                      bw->width,
                                      bw->height,
                                      GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
        ROG_ERR("failed to create gbm_bo");
        goto fail;
    }

    // modifier
    int gbm_has_modifier = 0;
    {
        uint64_t modifier = gbm_bo_get_modifier(bo);
        gbm_has_modifier  = modifier != DRM_FORMAT_MOD_INVALID;
    }

    // connect buffer to opengl
    {
        egl_image = _create_egl_image(bw->egl_display, bo, gbm_has_modifier);
        if (!egl_image) {
            goto fail;
        }

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);

        // attaching egl image to glRenderbuffer
        gl_proc->glEGLImageTargetRenderbufferStorageOES(
          GL_RENDERBUFFER, (GLeglImageOES)egl_image);

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

    {
        int      bo_fd    = gbm_bo_get_fd(bo);
        uint32_t stride   = gbm_bo_get_stride(bo);
        uint32_t offset   = gbm_bo_get_offset(bo, 0);
        uint64_t modifier = gbm_bo_get_modifier(bo);
        uint32_t width    = gbm_bo_get_width(bo);
        uint32_t height   = gbm_bo_get_height(bo);
        uint32_t format   = gbm_bo_get_format(bo);

        struct zwp_linux_buffer_params_v1* params =
          zwp_linux_dmabuf_v1_create_params(bw->wc->zwp_linux_dmabuf);
        zwp_linux_buffer_params_v1_add(params,
                                       bo_fd,
                                       0,
                                       offset,
                                       stride,
                                       modifier >> 32,
                                       modifier & 0xffffffff);

        if (wl_display_get_error(bw->wc->wl_display) != 0) {
            ROG_ERR("wayland connection is dead, bailing out");
            return NULL;
        }
        wl_buffer = zwp_linux_buffer_params_v1_create_immed(
          params, width, height, format, 0);

        wl_display_flush(bw->wc->wl_display);

        zwp_linux_buffer_params_v1_destroy(params);
        close(bo_fd);

        if (!wl_buffer) {
            ROG_ERR("faled to create immed wl_buffer");
            goto fail;
        }
    }

    struct redbuffer* rb;
    rb               = malloc(sizeof(*rb));
    rb->fbo          = fbo;
    rb->rbo          = rbo;
    rb->gbm_bo       = bo;
    rb->egl_image    = egl_image;
    rb->wl_buffer    = wl_buffer;
    rb->free         = 1;
    rb->needs_resize = 0;

    return rb;
fail:
    // TODO make its own free wl_buffer functions
    if (wl_buffer)
        wl_buffer_destroy(wl_buffer);
    if (fbo)
        glDeleteFramebuffers(1, &fbo);
    if (rbo)
        glDeleteRenderbuffers(1, &rbo);
    if (egl_image)
        eglDestroyImage(bw->egl_display, egl_image);
    if (bo)
        gbm_bo_destroy(bo);
    return NULL;
}

// using drm->width, drm->height
struct redbuffer*
init_drm_buffer(struct backend_drm* bd)
{
    struct gbm_bo* bo =
      gbm_bo_create(bd->gbm_dev,
                    bd->width,
                    bd->height,
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
        bd->gbm_has_modifier =
          bd->gbm_has_modifier || modifier != DRM_FORMAT_MOD_INVALID;
    }

    // connect buffer to opengl
    EGLImageKHR egl_image = NULL;
    GLuint      fbo = 0, rbo = 0;
    {
        egl_image =
          _create_egl_image(bd->egl_display, bo, bd->gbm_has_modifier);
        if (!egl_image) {
            goto fail;
        }

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);

        // attaching egl image to glRenderbuffer
        gl_proc->glEGLImageTargetRenderbufferStorageOES(
          GL_RENDERBUFFER, (GLeglImageOES)egl_image);

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
        uint32_t handles[4]   = { 0 };
        uint32_t pitches[4]   = { 0 };
        uint32_t offsets[4]   = { 0 };
        uint64_t modifiers[4] = { 0 };

        handles[0]      = gbm_bo_get_handle(bo).u32;
        pitches[0]      = gbm_bo_get_stride(bo);
        offsets[0]      = gbm_bo_get_offset(bo, 0);
        modifiers[0]    = gbm_bo_get_modifier(bo);
        uint32_t format = gbm_bo_get_format(bo);

        if (drmModeAddFB2WithModifiers(
              bd->drm_fd,
              bd->width,
              bd->height,
              format,
              handles,
              pitches,
              offsets,
              (bd->gbm_has_modifier) ? modifiers : NULL,
              &buf_id,
              (bd->gbm_has_modifier) ? DRM_MODE_FB_MODIFIERS : 0)) {
            ROG_ERR("failed to submit buffer drmModeAddFB2");
            goto fail;
        }
    }

    struct redbuffer* rb;
    rb               = malloc(sizeof(*rb));
    rb->fbo          = fbo;
    rb->rbo          = rbo;
    rb->gbm_bo       = bo;
    rb->egl_image    = egl_image;
    rb->buf_id       = buf_id;
    rb->free         = 1;
    rb->needs_resize = 0;

    return rb;
fail:
    ROG("failed to init drm buffer");

    return NULL;
}

struct gl_proc*
init_gl_proc()
{
    struct gl_proc* glProc;
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
