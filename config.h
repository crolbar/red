#pragma once

#include <stdbool.h>

typedef struct redconfig
{
    char* dri_dev;
    char* dri_render_dev;
} redconfig;

extern struct redconfig cfg;
