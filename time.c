#include "time.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct timespec*
time_get_now()
{
    struct timespec* tp;
    tp = calloc(1, sizeof(*tp));
    if (!tp)
        return NULL;
    clock_gettime(CLOCK_MONOTONIC, tp);
    return tp;
}

uint64_t
time_get_now_msec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

uint64_t
time_get_now_usec()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

double
time_get_elapsed_sec(struct timespec* _tp)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    return (tp.tv_sec - _tp->tv_sec) + (tp.tv_nsec - _tp->tv_nsec) / 1e9;
}
