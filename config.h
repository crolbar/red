#pragma once

#include "red.h"
#include <stdbool.h>
#include <stdint.h>

#define BINDS(...)                                                             \
    .binds     = (redbind[]){ __VA_ARGS__ },                                   \
    .binds_len = (sizeof((redbind[]){ __VA_ARGS__ }) /                         \
                  sizeof(((redbind[]){ __VA_ARGS__ })[0])),
#define A(...)                                                                 \
    .action     = (char*[]){ __VA_ARGS__, NULL },                              \
    .action_len = (sizeof((char*[]){ __VA_ARGS__ }) /                          \
                   sizeof(((char*[]){ __VA_ARGS__ })[0]))

typedef struct redconfig
{
    char* dri_dev;
    char* dri_render_dev;

    int32_t kb_repeat_delay;
    int32_t kb_repeat_rate;

    char* xkb_rules;
    char* xkb_model;
    char* xkb_layout;
    char* xkb_variant;
    char* xkb_options;

    redbind* binds;
    size_t   binds_len;
} redconfig;

extern struct redconfig cfg;
