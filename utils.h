#pragma once

// takes char* `str` puts '\' before each '\','"' and outputs into `esc`
// `esc` should be freed afterwards
#define UTIL_STR_ESCAPE(str, esc)                                              \
    do {                                                                       \
        if (str == NULL)                                                       \
            break;                                                             \
        int n = 0, i = 0;                                                      \
        for (; (str)[i] != '\0'; i++) {                                        \
            if ((str)[i] == '\"' || (str)[i] == '\\')                          \
                n++;                                                           \
        }                                                                      \
        char* out = calloc(1, (i + 1) + n + 1);                                \
        if (!out) {                                                            \
            (esc) = NULL;                                                      \
            break;                                                             \
        }                                                                      \
        i       = 0;                                                           \
        char* p = out;                                                         \
        for (i = 0; (str)[i] != '\0';) {                                       \
            if ((str)[i] == '\"' || (str)[i] == '\\')                          \
                *p++ = '\\';                                                   \
                                                                               \
            *p++ = (str)[i++];                                                 \
        }                                                                      \
        *p    = '\0';                                                          \
        (esc) = out;                                                           \
    } while (0);
