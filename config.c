#include "config.h"

/*
    ENV VARS:

    RED_DONT_SPAWN_CLIENT:
        Force not spawing in as a client.
        Currently using it to use my other gpu thats not used by anything
        while in a wayland compositor. Outputting directly on the monitor connected.

    
*/

redconfig cfg = (redconfig){
    /*
      /dev/dri/card0, /dev/dri/card1

      setting to `auto` will find the first card in /dev/dri
    */
    .dri_dev = "auto",
};
