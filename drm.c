#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <libinput.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend-drm.h"
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

    bd->page_flip_ready = false;

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    add_prop(req, bd->plane_id, bd->props->plane_fb_id, buf_id);

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

    // connect crtc to both connector and plane
    add_prop(req, bd->conn_id, bd->props->conn_crtc_id, bd->crtc_id);
    add_prop(req, bd->plane_id, bd->props->plane_crtc_id, bd->crtc_id);

    // fb to plane
    add_prop(req, bd->plane_id, bd->props->plane_fb_id, buf_id);

    add_prop(req, bd->plane_id, bd->props->plane_src_x, 0);
    add_prop(req, bd->plane_id, bd->props->plane_src_y, 0);
    add_prop(req, bd->plane_id, bd->props->plane_src_w, mode.hdisplay << 16);
    add_prop(req, bd->plane_id, bd->props->plane_src_h, mode.vdisplay << 16);

    add_prop(req, bd->plane_id, bd->props->plane_crtc_x, 0);
    add_prop(req, bd->plane_id, bd->props->plane_crtc_y, 0);
    add_prop(req, bd->plane_id, bd->props->plane_crtc_w, mode.hdisplay);
    add_prop(req, bd->plane_id, bd->props->plane_crtc_h, mode.vdisplay);

    if (drmModeAtomicCommit(
          bd->drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
        ROG_ERR("failed set crtc: %s", strerror(errno));
        return 1;
    }

    drmModeAtomicFree(req);
    return 0;
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
drm_get_primary_plane(int fd, int crtc_idx)
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
        if (!v)
            return -1;

        if (v == DRM_PLANE_TYPE_PRIMARY)
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
