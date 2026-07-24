#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <gbm.h>
#include <libinput.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend-drm.h"
#include "compositor.h"
#include "drm.h"
#include "drmProps.h"
#include "log.h"

int
drm_flip(struct backend_drm* bd, uint32_t buf_id, struct redstate* rs)
{
    // shouldn't happen
    if (!bd->page_flip_ready) {
        ROG_ERR("calling drm flip when prev flip is not finished");
        return 1;
    }

    bd->page_flip_ready = 0;

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    add_prop(req, bd->primary_plane_id, bd->props->pp_fb_id, buf_id);

    if (drmModeAtomicCommit(bd->drm_fd,
                            req,
                            DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK,
                            rs)) {
        ROG_ERR("failed commit a page flip: %s", strerror(errno));
        return 1;
    }
    drmModeAtomicFree(req);
    return 0;
}

int
drm_set_crct(struct backend_drm* bd, uint32_t buf_id)
{
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    // set mode
    drmModeModeInfo mode = bd->mode;
    {
        uint32_t mode_blob_id = 0;
        drmModeCreatePropertyBlob(
          bd->drm_fd, &mode, sizeof(mode), &mode_blob_id);

        add_prop(req, bd->crtc_id, bd->props->crtc_mode_id, mode_blob_id);
        add_prop(req, bd->crtc_id, bd->props->crtc_active, 1);
    }

    // connect crtc to connector and planes
    add_prop(req, bd->conn_id, bd->props->conn_crtc_id, bd->crtc_id);
    add_prop(req, bd->primary_plane_id, bd->props->pp_crtc_id, bd->crtc_id);

    // fb to cursor_plane
    if (bd->rs->using_hardware_cursor) //
    {
        add_prop(req, bd->cursor_plane_id, bd->props->cp_crtc_id, bd->crtc_id);

        add_prop(
          req, bd->cursor_plane_id, bd->props->cp_fb_id, bd->cursor_buf_id);

        add_prop(req, bd->cursor_plane_id, bd->props->cp_src_x, 0);
        add_prop(req, bd->cursor_plane_id, bd->props->cp_src_y, 0);
        add_prop(req,
                 bd->cursor_plane_id,
                 bd->props->cp_src_w,
                 bd->cursor_plane_w << 16);
        add_prop(req,
                 bd->cursor_plane_id,
                 bd->props->cp_src_h,
                 bd->cursor_plane_h << 16);

        int x = (int)red_get_lc_x(bd->rs);
        int y = (int)red_get_lc_y(bd->rs);
        add_prop(req, bd->cursor_plane_id, bd->props->cp_crtc_x, x);
        add_prop(req, bd->cursor_plane_id, bd->props->cp_crtc_y, y);
        add_prop(
          req, bd->cursor_plane_id, bd->props->cp_crtc_w, bd->cursor_plane_w);
        add_prop(
          req, bd->cursor_plane_id, bd->props->cp_crtc_h, bd->cursor_plane_h);
    }

    // fb to primary_plane
    {
        add_prop(req, bd->primary_plane_id, bd->props->pp_fb_id, buf_id);

        add_prop(req, bd->primary_plane_id, bd->props->pp_src_x, 0);
        add_prop(req, bd->primary_plane_id, bd->props->pp_src_y, 0);
        add_prop(
          req, bd->primary_plane_id, bd->props->pp_src_w, mode.hdisplay << 16);
        add_prop(
          req, bd->primary_plane_id, bd->props->pp_src_h, mode.vdisplay << 16);

        add_prop(req, bd->primary_plane_id, bd->props->pp_crtc_x, 0);
        add_prop(req, bd->primary_plane_id, bd->props->pp_crtc_y, 0);
        add_prop(
          req, bd->primary_plane_id, bd->props->pp_crtc_w, mode.hdisplay);
        add_prop(
          req, bd->primary_plane_id, bd->props->pp_crtc_h, mode.vdisplay);
    }

    if (drmModeAtomicCommit(
          bd->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
        ROG_ERR("failed set crtc: %s", strerror(errno));
        return 1;
    }

    drmModeAtomicFree(req);
    return 0;
}

int
drm_update_cursor_plane(struct redstate* rs)
{
    struct backend_drm* bd = rs->backend->d;
    int                 x  = (int)red_get_lc_x(rs);
    int                 y  = (int)red_get_lc_y(rs);
    if (drmModeMoveCursor(bd->drm_fd, bd->crtc_id, x, y)) {
        ROG_ERR("drmMode move cursor failed: %s", strerror(errno));
        return 1;
    };
    return 0;
}

int
drm_init_cursor_plane(struct backend_drm* bd)
{
    drmGetCap(bd->drm_fd, DRM_CAP_CURSOR_WIDTH, &bd->cursor_plane_w);
    drmGetCap(bd->drm_fd, DRM_CAP_CURSOR_HEIGHT, &bd->cursor_plane_h);
    if (bd->cursor_plane_h == 0 || bd->cursor_plane_w == 0) {
        ROG_ERR("failet to get drm cursor cap size");
        goto fail;
    }

    struct gbm_bo* bo = gbm_bo_create(bd->gbm_dev,
                                      bd->cursor_plane_w,
                                      bd->cursor_plane_h,
                                      GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
    if (!bo) {
        ROG_ERR("failed to create gbm_bo");
        goto fail;
    }

    uint32_t buf_id;
    {
        uint32_t handles[4] = { 0 };
        uint32_t pitches[4] = { 0 };
        uint32_t offsets[4] = { 0 };

        handles[0]      = gbm_bo_get_handle(bo).u32;
        pitches[0]      = gbm_bo_get_stride(bo);
        offsets[0]      = gbm_bo_get_offset(bo, 0);
        uint32_t format = gbm_bo_get_format(bo);

        if (drmModeAddFB2(bd->drm_fd,
                          bd->cursor_plane_w,
                          bd->cursor_plane_h,
                          format,
                          handles,
                          pitches,
                          offsets,
                          &buf_id,
                          0)) {
            ROG_ERR("failed to submit buffer drmModeAddFB2");
            goto fail;
        }
    }
    // TODO texture
    {
        void*    map_data = NULL;
        uint32_t stride   = 0;
        void*    map      = gbm_bo_map(
          bo, 0, 0, 32, 32, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
        if (!map) {
            ROG_ERR("gbm_bo_map failed\n");
            return 1;
        }

        for (uint32_t y = 0; y < 32; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)map + y * stride);
            for (uint32_t x = 0; x < 32; x++) {
                row[x] = 0x03FF3333;
            }
        }

        gbm_bo_unmap(bo, map_data);
    }

    bd->cursor_gbm_bo             = bo;
    bd->cursor_buf_id             = buf_id;
    bd->rs->using_hardware_cursor = 1;

    return 0;
fail:
    bd->rs->using_hardware_cursor = 0;
    return 1;
}

int
drm_set_client_caps(int fd)
{
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        ROG_ERR("failed to set drm client cap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
        return 1;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        ROG_ERR("failed to set drm client cap DRM_CLIENT_CAP_ATOMIC");
        return 1;
    }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        ROG_ERR("failed to set drm client cap DRM_CLIENT_CAP_UNIVERSAL_PLANES");
        return 1;
    }
    return 0;
}

char*
drm_get_first_dri_dev()
{
    char* fmt = "/dev/dri/card%d";
    for (int i = 0; i <= 50; i++) {
        int   l    = snprintf(NULL, 0, fmt, i);
        char* path = malloc(l + 1);
        sprintf(path, fmt, i);
        struct stat sb;
        if (stat(path, &sb) != 0) {
            continue;
        }

        {
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                ROG_ERR("failed oppening drm device: %s", strerror(errno));
                return "";
            }
            if (drmIsMaster(fd) == 0) {
                close(fd);
                ROG_WARN("found dri dev %s, but its used, skipping it.", path);
                continue;
            }
            close(fd);
        }
        return path;
    }
    return "";
}

void
drm_print_driver_version(int fd)
{
    drmVersion* ver = drmGetVersion(fd);
    if (!ver) {
        ROG_ERR("drmGetVersion failed");
        return;
    }
    ROG_INFO("Using Driver: %s", ver->name);
    drmFreeVersion(ver);
}

int
drm_get_crtc_idx(int fd, uint32_t crtc_id)
{
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        ROG_ERR("failed to get drmModResources");
        return -1;
    }

    for (int i = 0; i < res->count_crtcs; i++) {
        drmModeCrtcPtr crtc = drmModeGetCrtc(fd, res->crtcs[i]);
        if (crtc->crtc_id == crtc_id) {
            drmModeFreeCrtc(crtc);
            drmModeFreeResources(res);
            return i;
        }
        drmModeFreeCrtc(crtc);
    }

    drmModeFreeResources(res);
    return -1;
}

int
drm_get_plane(int fd, int crtc_idx, int type)
{
    drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res)
        return -1;

    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane)
            continue;

        if (!(plane->possible_crtcs & (1 << crtc_idx))) {
            continue;
        }

        // checking if plane is primary
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
          fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props)
            return -1;

        int v = get_prop_value(fd, props, "type");
        if (v == -1)
            return -1;

        if (v == type)
            return plane->plane_id;
    }

    return -1;
}

drmModeConnector*
drm_get_connector(int fd)
{
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        ROG_ERR("failed to get drmModResources");
        return NULL;
    }
    int       count = res->count_connectors;
    uint32_t* conns = res->connectors;

    // get first found connected connector
    for (int i = 0; i < count; i++) {
        drmModeConnector* conn = drmModeGetConnector(fd, conns[i]);
        if (!conn)
            continue;

        if (conn->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(conn);
            continue;
        }

        drmModeFreeResources(res);
        return conn;
    }

    drmModeFreeResources(res);
    return NULL;
}

// wayland backend uses render dri node
// int
// init_drm_render()
// {
// fail:
//     ROG_ERR("init_drm_render failed");
//     return -1;
// }
