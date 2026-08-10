#pragma once

#include "red.h"

// quit the server
#define RED_ACTION_QUIT "quit"
// close toplevel window
#define RED_ACTION_CLOSE "close"
// spawn program from PATH
#define RED_ACTION_SPAWN "spawn"
// focus next toplevel
#define RED_ACTION_FOCUS_NEXT "focus_next"
// focus prev toplevel
#define RED_ACTION_FOCUS_PREV "focus_prev"
// focus the last focused toplevel (alt+tap)
#define RED_ACTION_FOCUS_LAST "focus_last"
// focus the nth toplevel. start from 0 using the number as an index in a list
#define RED_ACTION_FOCUS_N "focus_n"
// draw blank frame & stop rendering until action is send again
#define RED_ACTION_STOP_RENDERER "stop_renderer"

#define RED_ACTION_DEBUG "debug"

typedef struct redaction
{
    char* action_type;
    void (*f)(struct redstate* rs, char** args, size_t args_len);
} redaction;

#define SPAWN_PROG(...)                                                        \
    spawn_program((char*[]){ __VA_ARGS__, NULL },                              \
                  (sizeof((char*[]){ __VA_ARGS__ }) /                          \
                   sizeof(((char*[]){ __VA_ARGS__ })[0])));

int
spawn_program(char** args, size_t args_len);

// the action param is straight from the bind
void
exec_action(struct redstate* rs, char** action, size_t action_len);

extern struct redaction* redactions;
extern size_t            redactions_len;
