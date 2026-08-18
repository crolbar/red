#pragma once

#include "backend.h"
#include <EGL/egl.h>
#include <stdint.h>
#include <xf86drmMode.h>

enum backend_drm_page_change
{
    PAGE_CHANGE_PRIMARY_PLANE_FB   = 1 << 0,
    PAGE_CHANGE_CURSOR_PLANE_COORD = 1 << 1,
};

struct backend_drm
{
    struct redstate* rs;

    int             drm_fd;
    uint32_t        crtc_id;
    int             crtc_idx;
    uint32_t        conn_id;
    uint32_t        primary_plane_id;
    uint32_t        cursor_plane_id;
    drmModeModeInfo mode;
    uint32_t        width, height;

    struct gbm_device* gbm_dev;
    EGLDisplay         egl_display;
    EGLContext         egl_context;

    struct drmprops* props;

    struct redbuffer* rb0;
    struct redbuffer* rb1;
    uint32_t          used_rb;         // indicates which buffer is displayed
    int               page_flip_ready; // are we ready to render next frame

    struct gbm_bo* cursor_gbm_bo;
    uint32_t       cursor_buf_id;
    uint64_t       cursor_plane_w;
    uint64_t       cursor_plane_h;

    // changes to be made in the next atomic commit
    int      page_change;
    uint32_t pending_buf_id;
};

struct backend_drm*
backend_drm_init_data();

int
backend_drm_init(struct redstate* rs, struct backend_drm* bd);

extern struct backend backend_drm;
