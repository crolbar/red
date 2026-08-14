#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xf86drmMode.h>

#include "backend-drm.h"
#include "drmProps.h"
#include "log.h"

int
add_prop(drmModeAtomicReqPtr req,
         uint32_t            object_id,
         uint32_t            property_id,
         uint64_t            value)
{
    if (object_id == 0 || property_id == 0) {
        ROG_ERR("drm atomic prop add object id or prop id is 0");
        return -1;
    }
    if (drmModeAtomicAddProperty(req, object_id, property_id, value) < 0) {
        ROG_ERR("drm atomic prop add failed");
        return -1;
    }
    return 0;
}

int
get_prop_value(int fd, drmModeObjectPropertiesPtr props, char* name)
{
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            return -1;

        if (strcmp(prop->name, name) == 0) {
            uint64_t val = props->prop_values[0];
            drmModeFreeProperty(prop);
            return val;
        }
        drmModeFreeProperty(prop);
    }
    return -1;
}

int
get_prop_id(int fd, drmModeObjectPropertiesPtr props, char* name)
{
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop)
            return -1;

        if (strcmp(prop->name, name) == 0) {
            uint32_t id = prop->prop_id;
            drmModeFreeProperty(prop);
            return id;
        }
        drmModeFreeProperty(prop);
    }
    return -1;
}

// get props of crtc, connector and plane of the crtc
int
init_prop_ids(struct backend_drm* bd)
{
    struct drmprops* dp;
    dp = calloc(1, sizeof(*dp));
    if (!dp)
        return 1;

    // connector
    {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
          bd->drm_fd, bd->conn_id, DRM_MODE_OBJECT_CONNECTOR);
        if (!props) {
            ROG_ERR("failed get props for connector");
            return 1;
        }

        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_ID");
            if (v == -1) {
                ROG_ERR("failed get CRTC_ID prop of connector");
                return 1;
            }
            dp->conn_crtc_id = v;
        }
        drmModeFreeObjectProperties(props);
    }
    // crtc
    {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
          bd->drm_fd, bd->crtc_id, DRM_MODE_OBJECT_CRTC);
        if (!props) {
            ROG_ERR("failed get props for crtc");
            return 1;
        }

        {
            int v = get_prop_id(bd->drm_fd, props, "ACTIVE");
            if (v == -1) {
                ROG_ERR("failed get ACTIVE prop of connector");
                return 1;
            }
            dp->crtc_active = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "MODE_ID");
            if (v == -1) {
                ROG_ERR("failed get MODE_ID prop of connector");
                return 1;
            }
            dp->crtc_mode_id = v;
        }
        drmModeFreeObjectProperties(props);
    }

    // primary plane
    {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
          bd->drm_fd, bd->primary_plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            ROG_ERR("failed get props for plane");
            return 1;
        }

        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_X");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_X");
                return 1;
            }
            dp->pp_src_x = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_Y");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_Y");
                return 1;
            }
            dp->pp_src_y = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_W");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_W");
                return 1;
            }
            dp->pp_src_w = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_H");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_H");
                return 1;
            }
            dp->pp_src_h = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_X");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_X");
                return 1;
            }
            dp->pp_crtc_x = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_Y");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_Y");
                return 1;
            }
            dp->pp_crtc_y = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_W");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_W");
                return 1;
            }
            dp->pp_crtc_w = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_H");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_H");
                return 1;
            }
            dp->pp_crtc_h = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "FB_ID");
            if (v == -1) {
                ROG_ERR("failed to get plane prop FB_ID");
                return 1;
            }
            dp->pp_fb_id = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_ID");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_ID");
                return 1;
            }
            dp->pp_crtc_id = v;
        }
        drmModeFreeObjectProperties(props);
    }

    // cursor plane
    {
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
          bd->drm_fd, bd->cursor_plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            ROG_ERR("failed get props for plane");
            return 1;
        }

        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_X");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_X");
                return 1;
            }
            dp->cp_src_x = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_Y");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_Y");
                return 1;
            }
            dp->cp_src_y = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_W");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_W");
                return 1;
            }
            dp->cp_src_w = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "SRC_H");
            if (v == -1) {
                ROG_ERR("failed to get plane prop SRC_H");
                return 1;
            }
            dp->cp_src_h = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_X");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_X");
                return 1;
            }
            dp->cp_crtc_x = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_Y");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_Y");
                return 1;
            }
            dp->cp_crtc_y = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_W");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_W");
                return 1;
            }
            dp->cp_crtc_w = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_H");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_H");
                return 1;
            }
            dp->cp_crtc_h = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "FB_ID");
            if (v == -1) {
                ROG_ERR("failed to get plane prop FB_ID");
                return 1;
            }
            dp->cp_fb_id = v;
        }
        {
            int v = get_prop_id(bd->drm_fd, props, "CRTC_ID");
            if (v == -1) {
                ROG_ERR("failed to get plane prop CRTC_ID");
                return 1;
            }
            dp->cp_crtc_id = v;
        }
        drmModeFreeObjectProperties(props);
    }

    bd->props = dp;

    return 0;
}
