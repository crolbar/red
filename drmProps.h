#pragma once

#include "backend-drm.h"
#include <stdint.h>
#include <xf86drmMode.h>

struct drmprops
{
    // crtc
    uint32_t crtc_active;
    uint32_t crtc_mode_id;

    // connector
    uint32_t conn_crtc_id;

    // plane
    uint32_t plane_src_x;
    uint32_t plane_src_y;
    uint32_t plane_src_w;
    uint32_t plane_src_h;
    uint32_t plane_crtc_x;
    uint32_t plane_crtc_y;
    uint32_t plane_crtc_w;
    uint32_t plane_crtc_h;
    uint32_t plane_fb_id;
    uint32_t plane_crtc_id;
};

int
add_prop(drmModeAtomicReqPtr req,
         uint32_t            object_id,
         uint32_t            property_id,
         uint64_t            value);

int
get_prop_value(int fd, drmModeObjectPropertiesPtr props, char* name);

int
get_prop_id(int fd, drmModeObjectPropertiesPtr props, char* name);

int
init_prop_ids(struct backend_drm* drm);
