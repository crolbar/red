#pragma once

#include "backend-drm.h"
#include "red.h"

int
drm_get_crtc_idx(int fd, uint32_t crtc_id);

int
drm_set_crct(struct backend_drm* drm, uint32_t buf_id);

int
drm_flip(struct backend_drm* bd, uint32_t buf_id, struct redstate* rs);

int
drm_set_client_caps(int fd);

char*
drm_get_first_dri_dev();

void
drm_print_driver_version(int fd);

int
drm_get_primary_plane(int fd, int crtc_idx);

drmModeConnector*
drm_get_connector(int fd);
