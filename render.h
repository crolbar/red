#pragma once

#include "drm.h"

int
render_frame(struct redstate* rs, struct redbuffer* rb);

#define RENDER_TRIGGER_FLIP 0x01
#define RENDER_TRIGGER_INIT 0x02

void
render_triggerI(int wrender_fd);

void
render_trigger(int wrender_fd);

int
should_render_trigger(int rrender_fd);
