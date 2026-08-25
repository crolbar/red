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

// makes the currently focused toplevel as a overlay.
// it will be shown when the focus moves to another toplevel
#define RED_ACTION_OVERLAY_SURFACE "overlay_surface"
// setters take one argument that should be like: +10, -20, +100...
// it must start with a + or -
#define RED_ACTION_OVERLAY_SET_WIDTH  "overlay_set_width"
#define RED_ACTION_OVERLAY_SET_HEIGHT "overlay_set_height"
#define RED_ACTION_OVERLAY_SET_X      "overlay_set_x"
#define RED_ACTION_OVERLAY_SET_Y      "overlay_set_y"

// saves the image from the currently focused toplevel into the current dir.
// *only the toplevel surface gets captured currently, so subsurfaces or popups
// will not be captured
#define RED_ACTION_CAPTURE_FOCUS "capture_focus"

// trigger an update of the frame image paths of all toplevels.
// this will create .ppm images in /tmp with the captured frames
// of all toplevels.
#define RED_ACTION_RT_FI_UPDATE "rt_fi_update"

// draw blank frame & stop rendering until action is send again
#define RED_ACTION_STOP_RENDERER "stop_renderer"
// change bind preset
#define RED_ACTION_SELECT_BIND_PRESET "sel_bind_preset"

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

int
is_valid_action(char* action);

// the action param is straight from the bind
void
exec_action(struct redstate* rs, char** action, size_t action_len);

extern struct redaction* redactions;
extern size_t            redactions_len;
