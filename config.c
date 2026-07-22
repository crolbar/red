#include "actions.h"
#include "config.h"
#include "red.h"

/*
    ENV VARS:

    RED_DONT_SPAWN_CLIENT:
        Force not spawing in as a client.
        Currently using it to use my other gpu thats not used by anything
        while in a wayland compositor. Outputting directly on the monitor
        connected.


*/

redconfig cfg = (redconfig){
    /*
      /dev/dri/card0, /dev/dri/card1

      setting to `auto` will find the first card in /dev/dri
    */
    .dri_dev = "auto",
    /*
      /dev/dri/renderD128, /dev/dri/renderD129

      setting to `auto` will find the first render node in /dev/dri
      starting at 128 searching forward
    */
    .dri_render_dev = "auto",

    .kb_repeat_delay = 300,
    .kb_repeat_rate  = 50,

    .xkb_rules   = "",
    .xkb_model   = "",
    .xkb_layout  = "us,us",
    .xkb_variant = ",dvorak",
    .xkb_options = "grp:win_space_toggle, ctrl:swap_ralt_rctl",

    // bind that use the shift mod, should change the
    // key - if it is a char (a - Z) - to uppercase
    // clang-format off
    BINDS(
       {
         .key = "F4",
         .mods = RED_MOD_NO_MODS,
         A( RED_ACTION_QUIT )
       },
       {
         .key = "F4",
         .mods = RED_MOD_SUPER | RED_MOD_ALT | RED_MOD_CTRL | RED_MOD_SHIFT,
         A( RED_ACTION_QUIT )
       },
       {
         .key = "k",
         .mods = RED_MOD_SUPER,
         A( RED_ACTION_FOCUS_PREV )
       },
       {
         .key = "j",
         .mods = RED_MOD_SUPER,
         A( RED_ACTION_FOCUS_NEXT )
       },
       {
         .key = "x",
         .mods = RED_MOD_SUPER,
         A( RED_ACTION_SPAWN, "foot" )
       },
       {
         .key = "d",
         .mods = RED_MOD_SUPER,
         A( RED_ACTION_SPAWN, "bash", "-c", "dunstify HELLO DUDE" )
       },
    )
    // clang-format on
};
