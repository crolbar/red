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

#include "config.h"
#include "drm.h"
#include "drmProps.h"
#include "log.h"
#include "render.h"

static void
page_flip_handler(int          fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void*        user_data)
{
    struct redstate* rs      = user_data;
    rs->drm->page_flip_ready = true;
    if (!rs->drm->stop_flipping)
        render_trigger(rs->wrender_fd);
}

static drmEventContext drmevctx = {
    .version           = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = page_flip_handler,
};

void
drm_handle_event(struct drmstate* drm)
{
    drmHandleEvent(drm->fd, &drmevctx);
}

int
drm_flip(struct drmstate* drm, uint32_t buf_id, struct redstate* rs)
{
    // shouldn't happen
    if (!drm->page_flip_ready) {
        ROG_ERR("calling drm flip when prev flip is not finished");
        return 1;
    }

    drm->page_flip_ready = false;

    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    add_prop(req, drm->plane_id, drm->props->plane_fb_id, buf_id);

    if (drmModeAtomicCommit(drm->fd,
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
drm_set_crct(struct drmstate* drm, uint32_t buf_id)
{
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    // set mode
    {
        uint32_t mode_blob_id = 0;
        drmModeCreatePropertyBlob(
          drm->fd, &drm->mode, sizeof(drm->mode), &mode_blob_id);

        add_prop(req, drm->crtc_id, drm->props->crtc_mode_id, mode_blob_id);
        add_prop(req, drm->crtc_id, drm->props->crtc_active, 1);
    }

    // connect crtc to both connector and plane
    add_prop(req, drm->conn_id, drm->props->conn_crtc_id, drm->crtc_id);
    add_prop(req, drm->plane_id, drm->props->plane_crtc_id, drm->crtc_id);

    // fb to plane
    add_prop(req, drm->plane_id, drm->props->plane_fb_id, buf_id);

    add_prop(req, drm->plane_id, drm->props->plane_src_x, 0);
    add_prop(req, drm->plane_id, drm->props->plane_src_y, 0);
    add_prop(
      req, drm->plane_id, drm->props->plane_src_w, drm->mode.hdisplay << 16);
    add_prop(
      req, drm->plane_id, drm->props->plane_src_h, drm->mode.vdisplay << 16);

    add_prop(req, drm->plane_id, drm->props->plane_crtc_x, 0);
    add_prop(req, drm->plane_id, drm->props->plane_crtc_y, 0);
    add_prop(req, drm->plane_id, drm->props->plane_crtc_w, drm->mode.hdisplay);
    add_prop(req, drm->plane_id, drm->props->plane_crtc_h, drm->mode.vdisplay);

    if (drmModeAtomicCommit(
          drm->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL)) {
        ROG_ERR("failed set crtc: %s", strerror(errno));
        return 1;
    }

    drmModeAtomicFree(req);

    return 0;
}

int
drm_handle_render_trigger(struct redstate* rs, uint32_t buf_id, int r)
{
    if (r == RENDER_TRIGGER_FLIP) {
        if (drm_flip(rs->drm, buf_id, rs))
            return 1;

    } else if (r == RENDER_TRIGGER_INIT) {
        if (drm_set_crct(rs->drm, buf_id))
            return 1;
        render_trigger(rs->wrender_fd);
    }

    return 0;
}

int
set_client_caps(int fd)
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
get_first_dri_dev()
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
print_driver_version(int fd)
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
get_crtc_idx(int fd, uint32_t crtc_id)
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
get_primary_plane(int fd, int crtc_idx)
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
get_connector(int fd)
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

struct drmstate*
init_drm()
{
    int               fd       = -1;
    int               crtc_idx = -1;
    uint32_t          crtc_id  = -1;
    uint32_t          plane_id = -1;
    drmModeConnector* conn     = NULL;

    char* dri_dev_path;
    if (strcmp(cfg.dri_dev, "auto") == 0) {
        dri_dev_path = get_first_dri_dev();
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
    print_driver_version(fd);

    if (set_client_caps(fd))
        goto fail;

    conn = get_connector(fd);
    if (!conn) {
        ROG_ERR("failed to find a connected monitor. is monitor connected "
                "to gpu %s",
                cfg.dri_dev);
        return NULL;
    }

    drmModeEncoder* encoder = drmModeGetEncoder(fd, conn->encoder_id);
    if (!encoder) {
        ROG_ERR("failed to get current encoder. is monitor connected to gpu %s",
                cfg.dri_dev);
        goto fail;
    }
    crtc_id = encoder->crtc_id;
    drmModeFreeEncoder(encoder);

    crtc_idx = get_crtc_idx(fd, crtc_id);
    if (crtc_idx == -1) {
        ROG_ERR("failed to get crtc_idx");
        goto fail;
    }

    {
        int _plane_id = get_primary_plane(fd, crtc_idx);
        if (_plane_id == -1) {
            ROG_ERR("failed to get plane_id");
            goto fail;
        }
        plane_id = (uint32_t)_plane_id;
    }
    struct drmstate* drm;
    drm = malloc(sizeof(*drm));
    if (!drm) {
        goto fail;
    }

    drm->fd               = fd;
    drm->gbm_has_modifier = false;
    drm->page_flip_ready  = true;
    drm->stop_flipping    = false;
    drm->fd               = fd;
    drm->modes            = conn->modes;
    drm->mode             = conn->modes[1];
    drm->width            = drm->mode.hdisplay;
    drm->height           = drm->mode.vdisplay;
    drm->crtc_id          = crtc_id;
    drm->crtc_idx         = crtc_idx;
    drm->plane_id         = plane_id;
    drm->conn_id          = conn->connector_id;
    if (init_prop_ids(drm))
        goto fail;

    ROG_INFO(
      "Rendering at: %dx%d@%d", drm->width, drm->height, drm->mode.vrefresh);

    drmModeFreeConnector(conn);

    return drm;
fail:

    ROG("fail")

    if (fd)
        close(fd);
    if (conn)
        drmModeFreeConnector(conn);
    return NULL;
}
