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

// takes char* `str` that should represent an integer that fits into `int`
// + and - are allowed at the start of str
// variable _err is set to 1 on error
// output is stored in `n`
#define UTIL_PARSE_INT(in_str, n)                                              \
    _Bool _err = 0;                                                            \
    do {                                                                       \
        char* str = (in_str);                                                  \
        if ((str) == NULL)                                                     \
            goto _err;                                                         \
        int out    = 0;                                                        \
        int is_neg = *(str) == '-';                                            \
        if (*(str) == '-' || *(str) == '+')                                    \
            (str)++;                                                           \
        for (char* p = (str); *p != '\0'; p++) {                               \
            if (*p < '0' || *p > '9')                                          \
                goto _err;                                                     \
            out *= 10;                                                         \
            out += *p - '0';                                                   \
        }                                                                      \
        (n) = ((is_neg) ? -1 : 1) * out;                                       \
        break;                                                                 \
    _err:                                                                      \
        _err = 1;                                                              \
    } while (0)

// `arr` should be `struct redbind*`, `bind` should be `struct redbind`
// `arr_len` -> size_t of the number of elements
// `arr_cap` -> size_t of the allocated size
#define UTIL_ADD_BIND(arr, bind, arr_len, arr_cap)                             \
    do {                                                                       \
        int bind_bm_used_idx = -1;                                             \
        for (size_t i = 0; i < rs->binds_len; i++) {                           \
            if (rs->binds[i].key_mods_bm == bind.key_mods_bm &&                \
                rs->binds[i].preset_n == bind.preset_n) {                      \
                bind_bm_used_idx = i;                                          \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        if (bind_bm_used_idx != -1) {                                          \
            rs->binds[bind_bm_used_idx] = bind;                                \
            break;                                                             \
        }                                                                      \
        if ((arr_len) == (arr_cap)) {                                          \
            size_t new_cap = (arr_cap) + 10;                                   \
            void*  new_arr = realloc((arr), (new_cap * sizeof(*(arr))));       \
            if (!new_arr)                                                      \
                goto fail;                                                     \
            (arr_cap) = new_cap;                                               \
            (arr)     = new_arr;                                               \
        }                                                                      \
        (arr)[(arr_len)++] = (bind);                                           \
    } while (0)
