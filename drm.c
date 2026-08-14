#include "backend-drm.h"
#include "config.h"
#include "drm.h"
#include "drmProps.h"
#include "gbm.h"
#include "log.h"
#include "red_cursor.h"
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

int
drm_set_crct(struct backend_drm* bd, uint32_t buf_id)
{
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    assert(req);

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

        int x = (int)bd->rs->cursor_x;
        int y = (int)bd->rs->cursor_y;
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
drm_add_cursor_plane_props(struct redstate* rs, drmModeAtomicReqPtr req)
{
    if (!rs->active)
        return 0;
    struct backend_drm* bd = rs->backend->d;
    int                 x  = rs->cursor_x;
    int                 y  = rs->cursor_y;
    if (cfg.center_cursor_hotspot) {
        x -= gimp_image.width / 2;
        y -= gimp_image.height / 2;
    }
    add_prop(req, bd->cursor_plane_id, bd->props->cp_crtc_x, x);
    add_prop(req, bd->cursor_plane_id, bd->props->cp_crtc_y, y);
    return 0;
}

int
drm_commit(struct redstate* rs)
{
    if (!rs->active)
        return 0;
    struct backend_drm* bd = rs->backend->d;

    // shouldn't happen
    if (!bd->page_flip_ready) {
        ROG_ERR("calling drm flip when prev flip is not finished");
        return 1;
    }

    bd->page_flip_ready = 0;

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();
    assert(req);

    if (bd->page_change & PAGE_CHANGE_CURSOR_PLANE_COORD) {
        drm_add_cursor_plane_props(rs, req);
    }

    if (bd->page_change & PAGE_CHANGE_PRIMARY_PLANE_FB) {
        assert(bd->pending_buf_id != 0);
        add_prop(
          req, bd->primary_plane_id, bd->props->pp_fb_id, bd->pending_buf_id);
        bd->pending_buf_id = 0;
    }

    if (drmModeAtomicCommit(bd->drm_fd,
                            req,
                            DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK,
                            rs)) {
        ROG_ERR("failed commit a page flip: %s", strerror(errno));
        return 1;
    }
    drmModeAtomicFree(req);

    bd->page_change = 0;
    return 0;
}

int
drm_update_cursor_plane(struct redstate* rs)
{
    struct backend_drm* bd = rs->backend->d;
    bd->page_change |= PAGE_CHANGE_CURSOR_PLANE_COORD;

    // if we have a queued redraw, when its done we will commit this change
    if (bd->page_flip_ready && !rs->queued_rb) {
        drm_commit(rs);
    }

    return 0;
}

int
drm_update_primary_plane(struct redstate* rs, uint32_t buf_id)
{
    struct backend_drm* bd = rs->backend->d;
    bd->pending_buf_id     = buf_id;
    bd->page_change |= PAGE_CHANGE_PRIMARY_PLANE_FB;

    // if we have no page flip in progress just commit.
    // if we have, on the page flip done event we will see we have
    // changes and will commit them
    if (bd->page_flip_ready) {
        drm_commit(rs);
    }

    return 0;
}

int
drm_hide_cursor(struct redstate* rs)
{
    if (!rs->active)
        return 0;
    struct backend_drm* bd = rs->backend->d;
    if (drmModeMoveCursor(
          bd->drm_fd, bd->crtc_id, -gimp_image.width, -gimp_image.height)) {
        ROG_ERR("drmMode move cursor failed for hide: %s", strerror(errno));
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
        ROG_ERR("failed to get drm cursor cap size");
        goto fail;
    }
    if (bd->cursor_plane_h < gimp_image.height ||
        bd->cursor_plane_w < gimp_image.width) {
        ROG_ERR(
          "gimp image height or width is bigger than the cursor plane's.");
        goto fail;
    }
    assert(gimp_image.bytes_per_pixel == 4);

    if (init_drm_cursor_buffer(bd))
        goto fail;

    {
        uint32_t w          = gimp_image.width;
        uint32_t h          = gimp_image.height;
        uint32_t img_stride = w * gimp_image.bytes_per_pixel;
        void*    map_data   = NULL;
        uint32_t stride     = 0;
        void*    map        = gbm_bo_map(bd->cursor_gbm_bo,
                               0,
                               0,
                               w,
                               h,
                               GBM_BO_TRANSFER_WRITE,
                               &stride,
                               &map_data);
        if (!map) {
            ROG_ERR("gbm_bo_map failed\n");
            return 1;
        }

        for (uint32_t i = 0; i < gimp_image.height; i++) {
            uint8_t* gbm_row = map + i * stride;
            uint8_t* img_row = gimp_image.pixel_data + i * img_stride;

            for (uint32_t j = 0; j < img_stride;
                 j += gimp_image.bytes_per_pixel) {
                gbm_row[j + 0] = img_row[j + 2];
                gbm_row[j + 1] = img_row[j + 1];
                gbm_row[j + 2] = img_row[j + 0];
                gbm_row[j + 3] = img_row[j + 3];
            }
        }
        gbm_bo_unmap(bd->cursor_gbm_bo, map_data);
    }

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
drm_get_first_primary_node()
{
    char* fmt = "/dev/dri/card%d";
    for (int i = 0; i <= 50; i++) {
        int   l    = snprintf(NULL, 0, fmt, i);
        char* path = calloc(1, l + 1);
        assert(path);
        sprintf(path, fmt, i);
        struct stat sb;
        if (stat(path, &sb) != 0) {
            free(path);
            continue;
        }

        {
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                ROG_ERR("failed oppening drm device: %s", strerror(errno));
                free(path);
                return "";
            }
            if (drmIsMaster(fd) == 0) {
                close(fd);
                ROG_WARN("found dri dev %s, but its used, skipping it.", path);
                free(path);
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
        assert(crtc);
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
    int                out       = -1;
    drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res)
        goto end;

    for (uint32_t i = 0; i < plane_res->count_planes; i++) {
        drmModePlanePtr plane = drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane)
            continue;

        if (!(plane->possible_crtcs & (1 << crtc_idx))) {
            drmModeFreePlane(plane);
            continue;
        }

        uint32_t id = plane->plane_id;
        drmModeFreePlane(plane);

        // checking if plane is primary
        drmModeObjectPropertiesPtr props =
          drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
        if (!props)
            goto end;

        int v = get_prop_value(fd, props, "type");
        drmModeFreeObjectProperties(props);
        if (v == -1)
            goto end;

        if (v == type) {
            out = id;
            goto end;
        }
    }

end:
    drmModeFreePlaneResources(plane_res);
    return out;
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

    // get first connected connector
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
