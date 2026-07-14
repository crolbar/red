#pragma once

#include "drm.h"

struct libinput*
init_input();

int
input_check_close(struct redstate* rs);
