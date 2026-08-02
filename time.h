#pragma once

#include <stdint.h>
struct timespec*
time_get_now();

uint32_t
time_get_now_msec();

double
time_get_elapsed_sec(struct timespec* _tp);
