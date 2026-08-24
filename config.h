#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIND_PRESETS(...)                                                      \
    .bind_presets     = (redbindpreset[]){ __VA_ARGS__ },                      \
    .bind_presets_len = (sizeof((redbindpreset[]){ __VA_ARGS__ }) /            \
                         sizeof(((redbindpreset[]){ __VA_ARGS__ })[0])),
#define BINDS(...)                                                             \
    .binds     = (redbindcfg[]){ __VA_ARGS__ },                                \
    .binds_len = (sizeof((redbindcfg[]){ __VA_ARGS__ }) /                      \
                  sizeof(((redbindcfg[]){ __VA_ARGS__ })[0])),
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

#define MOUSES(...)                                                            \
    .mouses     = (redmousecfg[]){ __VA_ARGS__ },                              \
    .mouses_len = (sizeof((redmousecfg[]){ __VA_ARGS__ }) /                    \
                   sizeof(((redmousecfg[]){ __VA_ARGS__ })[0])),

typedef struct redautostartprog
{
    char** args;
    size_t args_len;
} redautostartprog;

typedef struct redmousecfg
{
    // TODO: print all dev names?
    const char* name;
    double      speed;
    int         flat_profile;

} redmousecfg;

typedef char* envvar[2];

// mods is a bitmask
typedef struct redbindcfg
{
    uint8_t mods;
    char*   key;
    char**  action;
    size_t  action_len;
    bool    wl_client;
    bool    not_repeated;
} redbindcfg;

typedef struct redbindpreset
{
    redbindcfg* binds;
    size_t      binds_len;
} redbindpreset;

typedef struct redconfig
{
    char* dri_dev;
    char* dri_render_dev;

    uint32_t screen_scale;

    int32_t kb_repeat_delay;
    int32_t kb_repeat_rate;

    char* xkb_rules;
    char* xkb_model;
    char* xkb_layout;
    char* xkb_variant;
    char* xkb_options;

    uint32_t cursor_autohide_time;

    uint32_t              sel_bind_preset;
    struct redbindpreset* bind_presets;
    size_t                bind_presets_len;

    bool center_cursor_hotspot;

    redautostartprog* auto_start_progs;
    size_t            auto_start_progs_len;

    envvar* env_vars;
    size_t  env_vars_len;

    redmousecfg* mouses;
    size_t       mouses_len;

    bool     animations;
    uint64_t animation_focus_change_duration;

    bool     autoscroll;
    double   autoscroll_scale;
    uint32_t autoscroll_expo;
} redconfig;

extern struct redconfig cfg;
