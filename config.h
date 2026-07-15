#pragma once

#include <stdbool.h>

typedef struct redconfig
{
    char* dri_dev;
} redconfig;

extern struct redconfig cfg;
