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
    uint32_t pp_src_x;
    uint32_t pp_src_y;
    uint32_t pp_src_w;
    uint32_t pp_src_h;
    uint32_t pp_crtc_x;
    uint32_t pp_crtc_y;
    uint32_t pp_crtc_w;
    uint32_t pp_crtc_h;
    uint32_t pp_fb_id;
    uint32_t pp_crtc_id;
    uint32_t cp_src_x;
    uint32_t cp_src_y;
    uint32_t cp_src_w;
    uint32_t cp_src_h;
    uint32_t cp_crtc_x;
    uint32_t cp_crtc_y;
    uint32_t cp_crtc_w;
    uint32_t cp_crtc_h;
    uint32_t cp_fb_id;
    uint32_t cp_crtc_id;
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
