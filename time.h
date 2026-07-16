#pragma once

struct timespec*
time_get_now();

double
time_get_elapsed_sec(struct timespec* _tp);
