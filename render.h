#pragma once

#include "red.h"

// pulls buf from backend, calls render_frame, pushes buf to backend
void
redraw(struct redstate* rs);

void
redraw_done(struct redstate* rs);

void
request_redraw(struct redstate* rs);
