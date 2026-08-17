#pragma once
#include "red.h"

int
vt_switch(struct redstate* rs, int n);

int
vt_stop(struct redstate* rs);

int
init_vt(struct redstate* rs);
