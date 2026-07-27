#include "backend-drm.h"
#include "config.h"
#include "drm.h"
#include "drmProps.h"
#include "gbm.h"
#include "log.h"
#include "opengl.h"
#include "render.h"
#include "wayland.h"
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

void*
backend_drm_init_data()
{
    struct backend_drm* bd = NULL;
    bd                     = malloc(sizeof(*bd));
    if (!bd)
        return NULL;

    bd->rs               = NULL;
    bd->drm_fd           = 0;
    bd->crtc_id          = 0;
    bd->crtc_idx         = 0;
    bd->conn_id          = 0;
    bd->primary_plane_id = 0;
    bd->cursor_plane_id  = 0;
    bd->egl_display      = NULL;
    bd->egl_context      = NULL;
    bd->gbm_dev          = NULL;
    bd->rb0              = NULL;
    bd->rb1              = NULL;
    bd->page_flip_ready  = 1;
    bd->used_rb          = 0;
    bd->width            = 300;
    bd->height           = 300;
    bd->cursor_gbm_bo    = 0;
    bd->cursor_buf_id    = 0;
    bd->cursor_plane_w   = 0;
    bd->cursor_plane_h   = 0;

    return bd;
}

uint32_t
backend_drm_get_width(void* data)
{
    struct backend_drm* bd = data;
    return bd->width;
}

uint32_t
backend_drm_get_height(void* data)
{
    struct backend_drm* bd = data;
    return bd->height;
}

int
backend_drm_init(void* data)
{
    struct redstate*    rs = data;
    struct backend_drm* bd = rs->backend->d;

    int               fd               = -1;
    int               crtc_idx         = -1;
    int               crtc_id          = -1;
    int               primary_plane_id = -1;
    int               cursor_plane_id  = -1;
    drmModeConnector* conn             = NULL;

    char* dri_dev_path;
    if (strcmp(cfg.dri_dev, "auto") == 0) {
        dri_dev_path = drm_get_first_primary_node();
        if (!dri_dev_path) {
            goto fail;
        }
    } else {
        dri_dev_path = cfg.dri_dev;
    }

    ROG_INFO("Using dri device: %s", dri_dev_path);
    fd = open(dri_dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ROG_ERR("failed oppening drm device: %s", strerror(errno));
        goto fail;
    }
    free(dri_dev_path);
    drm_print_driver_version(fd);

    if (drm_set_client_caps(fd))
        goto fail;

    conn = drm_get_connector(fd);
    if (!conn) {
        ROG_ERR("failed to find a connected monitor. is monitor connected "
                "to gpu %s",
                cfg.dri_dev);
        goto fail;
    }

    drmModeEncoder* encoder = drmModeGetEncoder(fd, conn->encoder_id);
    if (!encoder) {
        ROG_ERR(
          "failed to get current encoder. is monitor connected to gpu `%s`",
          cfg.dri_dev);
        goto fail;
    }
    crtc_id = encoder->crtc_id;
    drmModeFreeEncoder(encoder);

    crtc_idx = drm_get_crtc_idx(fd, crtc_id);
    if (crtc_idx == -1) {
        ROG_ERR("failed to get crtc_idx");
        goto fail;
    }

    {
        int _plane_id = drm_get_plane(fd, crtc_idx, DRM_PLANE_TYPE_PRIMARY);
        if (_plane_id == -1) {
            ROG_ERR("failed to get primary_plane_id");
            goto fail;
        }
        primary_plane_id = (uint32_t)_plane_id;
    }

    {
        int _plane_id = drm_get_plane(fd, crtc_idx, DRM_PLANE_TYPE_CURSOR);
        if (_plane_id == -1) {
            ROG_ERR("failed to get cursor_plane_id");
            goto fail;
        }
        cursor_plane_id = (uint32_t)_plane_id;
    }

    bd->rs               = rs;
    bd->drm_fd           = fd;
    bd->mode             = conn->modes[1];
    bd->width            = bd->mode.hdisplay;
    bd->height           = bd->mode.vdisplay;
    bd->crtc_id          = crtc_id;
    bd->crtc_idx         = crtc_idx;
    bd->primary_plane_id = primary_plane_id;
    bd->cursor_plane_id  = cursor_plane_id;
    bd->conn_id          = conn->connector_id;
    if (init_prop_ids(bd))
        goto fail;

    ROG_INFO("Rendering at: %dx%d@%d",
             bd->mode.hdisplay,
             bd->mode.vdisplay,
             bd->mode.vrefresh);

    drmModeFreeConnector(conn);
    conn = NULL;

    bd->gbm_dev = init_gbm(bd->drm_fd);
    if (!bd->gbm_dev)
        goto fail;

    if (init_egl(bd->gbm_dev, &bd->egl_display, &bd->egl_context))
        goto fail;

    bd->rb0 = init_drm_buffer(bd);
    if (!bd->rb0)
        goto fail;
    bd->rb1 = init_drm_buffer(bd);
    if (!bd->rb1)
        goto fail;

    if (drm_init_cursor_plane(bd))
        goto fail;

    rs->cursor_x = (float)bd->width / 2;
    rs->cursor_y = (float)bd->height / 2;

    return 0;
fail:
    if (fd)
        close(fd);
    if (conn)
        drmModeFreeConnector(conn);
    return 1;
}

redbuffer*
backend_drm_pull_buffer(void* data)
{
    struct backend_drm* bd = data;

    bd->used_rb = 1 - bd->used_rb;
    if (bd->used_rb)
        return bd->rb1;
    return bd->rb0;
}

int
backend_drm_push_buffer(void* data, redbuffer* rb)
{
    struct redstate*    rs = data;
    struct backend_drm* bd = rs->backend->d;

    drm_flip(bd, rb->buf_id, rs);
    return 0;
}

int
backend_drm_push_init_buffer(void* data)
{
    struct redstate*    rs = data;
    struct backend_drm* bd = rs->backend->d;
    struct redbuffer*   rb = (!bd->used_rb) ? bd->rb0 : bd->rb1;

    drm_set_crct(bd, rb->buf_id);
    redraw(rs);
    return 0;
}

int
backend_drm_resize_buffer(void* d, struct redbuffer* rb)
{
    return 0;
}

static void
page_flip_handler(int          fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void*        user_data)
{
    struct redstate*    rs = user_data;
    struct backend_drm* bd = rs->backend->d;

    bd->page_flip_ready = 1;
    red_on_frame_done(rs);
}

static drmEventContext drmevctx = {
    .version           = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = page_flip_handler,
};

int
backend_drm_handle_events(void* d)
{
    struct backend_drm* bd = d;
    drmHandleEvent(bd->drm_fd, &drmevctx);
    return 0;
}

int
backend_drm_get_fd(void* d)
{
    struct backend_drm* bd = d;
    return bd->drm_fd;
}

int
backend_drm_flush_events(void* d)
{
    return 0;
}

int
backend_drm_is_ready_for_frame(void* d)
{
    struct backend_drm* bd = d;
    return bd->page_flip_ready;
}

int
backend_drm_get_drm_node(void* d)
{
    struct backend_drm* bd = d;
    return bd->drm_fd;
}

EGLDisplay
backend_drm_egl_display(void* d)
{
    struct backend_drm* bd = d;
    return bd->egl_display;
}

struct backend backend_drm = {
    .d                  = NULL,
    .init_data          = backend_drm_init_data,
    .init               = backend_drm_init,
    .get_height         = backend_drm_get_height,
    .get_width          = backend_drm_get_width,
    .pull_buffer        = backend_drm_pull_buffer,
    .push_buffer        = backend_drm_push_buffer,
    .push_init_buffer   = backend_drm_push_init_buffer,
    .resize_buffer      = backend_drm_resize_buffer,
    .get_fd             = backend_drm_get_fd,
    .flush_events       = backend_drm_flush_events,
    .handle_events      = backend_drm_handle_events,
    .is_ready_for_frame = backend_drm_is_ready_for_frame,
    .get_drm_node       = backend_drm_get_drm_node,
    .get_egl_display    = backend_drm_egl_display,
};
