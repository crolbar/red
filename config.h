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

#define AUTO_START(...)                                                        \
    .auto_start_progs = (struct redautostartprog[]){ __VA_ARGS__ },            \
    .auto_start_progs_len =                                                    \
      (sizeof((struct redautostartprog[]){ __VA_ARGS__ }) /                    \
       sizeof(((struct redautostartprog[]){ __VA_ARGS__ })[0])),
#define PROG(...)                                                              \
    {                                                                          \
        .args     = (char*[]){ __VA_ARGS__ },                                  \
        .args_len = (sizeof((char*[]){ __VA_ARGS__ }) /                        \
                     sizeof(((char*[]){ __VA_ARGS__ })[0])),                   \
    }

#define ENV(...)                                                               \
    .env_vars     = (envvar[]){ __VA_ARGS__ },                                 \
    .env_vars_len = (sizeof((envvar[]){ __VA_ARGS__ }) /                       \
                     sizeof(((envvar[]){ __VA_ARGS__ })[0])),

typedef struct redautostartprog
{
    char** args;
    size_t args_len;
} redautostartprog;

typedef char* envvar[2];

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

    bool center_cursor_hotspot;

    redautostartprog* auto_start_progs;
    size_t            auto_start_progs_len;

    envvar* env_vars;
    size_t  env_vars_len;
} redconfig;

extern struct redconfig cfg;
