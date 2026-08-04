#pragma once

#include <stdint.h>
struct timespec*
time_get_now();

uint64_t
time_get_now_msec();

uint64_t
time_get_now_usec();

double
time_get_elapsed_sec(struct timespec* _tp);
