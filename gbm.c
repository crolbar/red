#include "backend-drm.h"
#include "backend-wayland-client.h"
#include "backend-wayland.h"
#include "gbm.h"
#include "linux-dmabuf-client-protocol.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <gbm.h>
#include <string.h>
#include <unistd.h>

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
zwp_dmabuf_add_from_gbm(struct gbm_bo*              bo,
                        struct wl_buffer**          wl_buffer,
                        struct wl_display*          wl_display,
                        struct zwp_linux_dmabuf_v1* zwp_linux_dmabuf)
{
    int      bo_fd    = gbm_bo_get_fd(bo);
    uint32_t stride   = gbm_bo_get_stride(bo);
    uint32_t offset   = gbm_bo_get_offset(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t width    = gbm_bo_get_width(bo);
    uint32_t height   = gbm_bo_get_height(bo);
    uint32_t format   = gbm_bo_get_format(bo);

    struct zwp_linux_buffer_params_v1* params =
      zwp_linux_dmabuf_v1_create_params(zwp_linux_dmabuf);
    assert(params);
    zwp_linux_buffer_params_v1_add(
      params, bo_fd, 0, offset, stride, modifier >> 32, modifier & 0xffffffff);

    if (wl_display_get_error(wl_display) != 0) {
        ROG_ERR("wayland connection is dead, bailing out");
        return 1;
    }
    *wl_buffer =
      zwp_linux_buffer_params_v1_create_immed(params, width, height, format, 0);

    zwp_linux_buffer_params_v1_destroy(params);
    close(bo_fd);

    if (!(*wl_buffer)) {
        ROG_ERR("faled to create immed wl_buffer");
        return 1;
    }
    wl_display_flush(wl_display);
    return 0;
}

EGLImageKHR
create_egl_image_from_gbm(EGLDisplay egl_display, struct gbm_bo* bo)
{
    int      fd       = gbm_bo_get_fd(bo);
    uint32_t stride   = gbm_bo_get_stride(bo);
    uint32_t offset   = gbm_bo_get_offset(bo, 0);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    uint32_t width    = gbm_bo_get_width(bo);
    uint32_t height   = gbm_bo_get_height(bo);
    uint32_t format   = gbm_bo_get_format(bo);

    EGLImageKHR img =
      init_egl_image(egl_display,
                     width,
                     height,
                     format,
                     1,
                     (struct dmabuf_plane[1]){ (struct dmabuf_plane){
                       .fd          = fd,
                       .offset      = offset,
                       .stride      = stride,
                       .modifier_lo = (uint32_t)(modifier & 0xFFFFFFFF),
                       .modifier_hi = (uint32_t)(modifier >> 32),
                     } });
    close(fd);
    return img;
}

static struct redbuffer*
init_redbuffer()
{
    struct redbuffer* rb;
    rb = calloc(1, sizeof(*rb));
    assert(rb);
    rb->fbo          = 0;
    rb->rbo          = 0;
    rb->gbm_bo       = NULL;
    rb->egl_image    = NULL;
    rb->wl_buffer    = NULL;
    rb->buf_id       = 0;
    rb->free         = 1;
    rb->needs_resize = 0;
    return rb;
}

void
free_redbuffer(struct redbuffer* rb, EGLDisplay egl_display)
{
    if (rb->wl_buffer)
        wl_buffer_destroy(rb->wl_buffer);
    if (rb->fbo)
        glDeleteFramebuffers(1, &rb->fbo);
    if (rb->rbo)
        glDeleteRenderbuffers(1, &rb->rbo);
    if (rb->egl_image)
        eglDestroyImage(egl_display, rb->egl_image);
    if (rb->gbm_bo)
        gbm_bo_destroy(rb->gbm_bo);
    if (rb)
        free(rb);
}

struct redbuffer*
init_wl_buffer(struct backend_wayland* bw)
{
    assert(bw->egl_display);
    assert(bw->gbm_dev);
    struct redbuffer* rb = init_redbuffer();

    if (!(rb->gbm_bo =
            gbm_bo_create(bw->gbm_dev,
                          bw->width,
                          bw->height,
                          GBM_FORMAT_XRGB8888,
                          GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR))) {
        ROG_ERR("failed to create gbm_bo");
        goto fail;
    }

    if (!(rb->egl_image =
            create_egl_image_from_gbm(bw->egl_display, rb->gbm_bo))) {
        goto fail;
    }

    if (gl_add_fb(rb->egl_image, &rb->fbo, &rb->rbo))
        goto fail;

    if (zwp_dmabuf_add_from_gbm(rb->gbm_bo,
                                &rb->wl_buffer,
                                bw->wc->wl_display,
                                bw->wc->zwp_linux_dmabuf))
        goto fail;

    return rb;
fail:
    free_redbuffer(rb, bw->egl_display);
    return NULL;
}

static int
drm_add_fb_from_gbm(struct gbm_bo* bo, int fd, uint32_t width, uint32_t height)
{
    uint32_t buf_id = -1;

    uint32_t handles[4]   = { 0 };
    uint32_t pitches[4]   = { 0 };
    uint32_t offsets[4]   = { 0 };
    uint64_t modifiers[4] = { 0 };

    handles[0]      = gbm_bo_get_handle(bo).u32;
    pitches[0]      = gbm_bo_get_stride(bo);
    offsets[0]      = gbm_bo_get_offset(bo, 0);
    modifiers[0]    = gbm_bo_get_modifier(bo);
    uint32_t format = gbm_bo_get_format(bo);

    int has_mods = modifiers[0] != DRM_FORMAT_MOD_INVALID &&
                   modifiers[0] != DRM_FORMAT_MOD_LINEAR;

    if (drmModeAddFB2WithModifiers(fd,
                                   width,
                                   height,
                                   format,
                                   handles,
                                   pitches,
                                   offsets,
                                   (has_mods) ? modifiers : NULL,
                                   &buf_id,
                                   (has_mods) ? DRM_MODE_FB_MODIFIERS : 0)) {
        return -1;
    }

    return buf_id;
}

// using drm->width, drm->height
struct redbuffer*
init_drm_buffer(struct backend_drm* bd)
{
    assert(bd->egl_display);
    struct redbuffer* rb = init_redbuffer();

    if (!(rb->gbm_bo =
            gbm_bo_create(bd->gbm_dev,
                          bd->width,
                          bd->height,
                          GBM_FORMAT_XRGB8888,
                          GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT))) {
        ROG_ERR("failed to create gbm_bo");
        goto fail;
    }

    // connect buffer to egl
    if (!(rb->egl_image =
            create_egl_image_from_gbm(bd->egl_display, rb->gbm_bo))) {
        goto fail;
    }

    // connect egl image to opengl
    if (gl_add_fb(rb->egl_image, &rb->fbo, &rb->rbo))
        goto fail;

    // connect buffer to drm
    int i = -1;
    if ((i = drm_add_fb_from_gbm(
           rb->gbm_bo, bd->drm_fd, bd->width, bd->height)) < 0) {
        ROG_ERR("failed to submit buffer drmModeAddFB2: %s", strerror(errno));
        goto fail;
    }
    rb->buf_id = (uint32_t)i;

    return rb;
fail:
    free_redbuffer(rb, bd->egl_display);
    return NULL;
}

int
init_drm_cursor_buffer(struct backend_drm* bd)
{
    struct gbm_bo* bo = gbm_bo_create(bd->gbm_dev,
                                      bd->cursor_plane_w,
                                      bd->cursor_plane_h,
                                      GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
    if (!bo) {
        ROG_ERR("failed to create cursor gbm_bo");
        goto fail;
    }

    // fill cursor plane
    {
        int buf_size = bd->cursor_plane_w * (bd->cursor_plane_h * 4);
        // suppporting up to 256 * 256 max cursor plane dimensions
        uint8_t buf[262144];
        memset(buf, 0, buf_size);
        if (gbm_bo_write(bo, buf, buf_size)) {
            ROG_ERR("failed to fill cursor gbm_bo: %s", strerror(errno));
            goto fail;
        }
    }

    int i = -1;
    if ((i = drm_add_fb_from_gbm(
           bo, bd->drm_fd, bd->cursor_plane_w, bd->cursor_plane_h)) < 0) {
        ROG_ERR("failed to submit cursor buffer drmModeAddFB2: %s",
                strerror(errno));
        goto fail;
    }
    uint32_t buf_id = (uint32_t)i;

    bd->cursor_gbm_bo = bo;
    bd->cursor_buf_id = buf_id;
    return 0;
fail:
    return 1;
}
