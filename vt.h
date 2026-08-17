#pragma once
#include "red.h"

int
vt_switch(struct redstate* rs, int n);

int
vt_stop(struct redstate* rs);

int
init_vt(struct redstate* rs);

int
open_device(struct redstate* rs, const char* path);

int
close_device_by_fd(struct redstate* rs, int fd);
