#pragma once

#include <assert.h>
#include <stdlib.h>

#define concat(a, b) a##b

#define _dll(TYPE, ID)                                                         \
    struct                                                                     \
    {                                                                          \
        size_t size;                                                           \
        struct concat(node, ID)                                                \
        {                                                                      \
            TYPE val;                                                          \
            struct concat(node, ID) * next, *prev;                             \
        } *head, *tail;                                                        \
    }
#define _gt(dll) typeof(*(dll).tail)

#define dll(TYPE) _dll(TYPE, __COUNTER__)

#define dll_init() { 0, NULL, NULL }

#define _dll_push_mk_new_node(dll, node, new_val)                              \
    assert(!(((node) == NULL) && ((dll).tail != NULL || (dll).head != NULL))); \
    assert(!(node != NULL && (dll).size == 0));                                \
    _gt(dll)* new_node = malloc(sizeof(*new_node));                            \
    new_node->next = new_node->prev = NULL;                                    \
    new_node->val                   = new_val;                                 \
    if (node == NULL) {                                                        \
        (dll).head = (dll).tail = new_node;                                    \
        (dll).size              = 1;                                           \
        break;                                                                 \
    }                                                                          \
    (dll).size++;

#define _dll_pop(dll, i)                                                       \
    ({                                                                         \
        typeof((dll).tail->val) n = i->val;                                    \
        dll_remove(dll, i);                                                    \
        n;                                                                     \
    })

#define dll_push_after(dll, item, new_val)                                     \
    do {                                                                       \
        _gt(dll)* node = item;                                                 \
        _dll_push_mk_new_node((dll), (node), (new_val));                       \
        new_node->next = node->next;                                           \
        new_node->prev = node;                                                 \
        if (node->next == NULL)                                                \
            (dll).tail = new_node;                                             \
        if (node->next != NULL)                                                \
            node->next->prev = new_node;                                       \
        node->next = new_node;                                                 \
    } while (0);

#define dll_push_before(dll, item, new_val)                                    \
    do {                                                                       \
        _gt(dll)* node = item;                                                 \
        _dll_push_mk_new_node((dll), (node), (new_val));                       \
        new_node->next = node;                                                 \
        new_node->prev = node->prev;                                           \
        if (node->prev == NULL)                                                \
            (dll).head = new_node;                                             \
        if (node->prev != NULL)                                                \
            node->prev->next = new_node;                                       \
        node->prev = new_node;                                                 \
    } while (0);

#define dll_push_tail(dll, new_val) dll_push_after((dll), (dll).tail, new_val);
#define dll_push_head(dll, new_val) dll_push_before((dll), (dll).head, new_val);
#define dll_for_each(d, v) for (_gt(d)* v = (d).head; v != NULL; v = v->next)
#define dll_rfor_each(d, v) for (_gt(d)* v = (d).tail; v != NULL; v = v->prev)
#define dll_pop(dll) _dll_pop(dll, (dll).tail)
#define dll_hpop(dll) _dll_pop(dll, (dll).head)

#define dll_destroy(dll)                                                       \
    do {                                                                       \
        for (_gt(dll)* node = (dll).head, *next; node != NULL; node = next) {  \
            next = node->next;                                                 \
            free(node);                                                        \
        }                                                                      \
        (dll).head = (dll).tail = NULL;                                        \
        (dll).size              = 0;                                           \
    } while (0);

#define dll_remove(dll, item)                                                  \
    do {                                                                       \
        _gt(dll)* node = (item);                                               \
        if (node->prev != NULL)                                                \
            node->prev->next = node->next;                                     \
        if (node->next != NULL)                                                \
            node->next->prev = node->prev;                                     \
        if (node->next == NULL)                                                \
            (dll).tail = node->prev;                                           \
        if (node->prev == NULL)                                                \
            (dll).head = node->next;                                           \
        free(node);                                                            \
        (dll).size--;                                                          \
    } while (0)

#define dll_remove_val(dll, _val)                                              \
    do {                                                                       \
        _gt(dll)* _n = NULL;                                                   \
        dll_for_each(dll, v)                                                     \
        {                                                                      \
            if (v->val == (_val))                                              \
                _n = v;                                                        \
        }                                                                      \
        if (_n == NULL)                                                        \
            break;                                                             \
        dll_remove(dll, _n);                                                   \
    } while (0)
