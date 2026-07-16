#include <drm/drm_fourcc.h>
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <libinput.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "config.h"
#include "drm.h"
#include "log.h"
#include "red.h"
#include "render.h"

static void
page_flip_handler(int fd,
                  unsigned int sequence,
                  unsigned int tv_sec,
                  unsigned int tv_usec,
                  void* user_data)
{
    struct redstate* rs = user_data;
    rs->drm->page_flip_ready = true;
    render_trigger(rs->wrender_fd);
}

static drmEventContext drmevctx = {
    .version = DRM_EVENT_CONTEXT_VERSION,
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

    if (drmModePageFlip(
          drm->fd, drm->crtc_id, buf_id, DRM_MODE_PAGE_FLIP_EVENT, rs)) {
        ROG_ERR("page flip failed: %s", strerror(errno));
        return 1;
    }
    return 0;
}

int
drm_set_crct(struct drmstate* drm, uint32_t buf_id)
{
    if (drmModeSetCrtc(
          drm->fd, drm->crtc_id, buf_id, 0, 0, &drm->conn_id, 1, &drm->mode)) {
        ROG_ERR("failed set crtc: %s", strerror(errno));
        return 1;
    }
    return 0;
}

char*
get_first_dri_dev()
{
    char* fmt = "/dev/dri/card%d";
    for (int i = 0; i <= 50; i++) {
        int l = snprintf(NULL, 0, fmt, i);
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

drmModeConnector*
get_connector(int fd)
{
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        ROG_ERR("failed to get drmModResources");
        return NULL;
    }
    int count = res->count_connectors;
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

struct drmstate*
init_drm()
{
    int fd = -1;
    int crtc_id = -1;
    drmModeConnector* conn = NULL;

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

    struct drmstate* drm;
    drm = malloc(sizeof(*drm));
    if (!drm) {
        goto fail;
    }

    drm->fd = fd;
    drm->used_rb = 0;
    drm->gbm_has_modifier = false;
    drm->page_flip_ready = true;
    drm->fd = fd;
    drm->modes = conn->modes;
    drm->mode = conn->modes[0];
    drm->width = drm->mode.hdisplay;
    drm->height = drm->mode.vdisplay;
    drm->crtc_id = crtc_id;
    drm->conn_id = conn->connector_id;

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
