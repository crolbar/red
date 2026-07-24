#pragma once

#include "red.h"

#define CALL(_CALL)                                                            \
    do {                                                                       \
        (_CALL);                                                               \
        GLenum err = glGetError();                                             \
        if (err != GL_NO_ERROR) {                                              \
            ROG_ERR("gl err: %x", err);                                        \
            goto fail;                                                         \
        }                                                                      \
    } while (0)

int
gl_setup_cursor_program(struct redstate* rs);
