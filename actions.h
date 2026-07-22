#pragma once

#include "red.h"

#define RED_ACTION_QUIT       "quit"
#define RED_ACTION_DEBUG      "debug"
#define RED_ACTION_FOCUS_NEXT "focus_next"
#define RED_ACTION_FOCUS_PREV "focus_prev"
#define RED_ACTION_SPAWN      "spawn"

typedef struct redaction
{
    char* action_type;
    void (*f)(struct redstate* rs, char** args, size_t args_len);
} redaction;

// the action param is straight from the bind
void
exec_action(struct redstate* rs, char** action, size_t action_len);

extern struct redaction* redactions;
extern size_t            redactions_len;
