#include "time.h"
#include <stdlib.h>
#include <time.h>

struct timespec*
time_get_now()
{
    struct timespec* tp;
    tp = malloc(sizeof(*tp));
    clock_gettime(CLOCK_MONOTONIC, tp);
    return tp;
}

double
time_get_elapsed_sec(struct timespec* _tp)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);

    return (tp.tv_sec - _tp->tv_sec) + (tp.tv_nsec - _tp->tv_nsec) / 1e9;
}
