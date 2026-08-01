#pragma once

#include "red.h"
#include <EGL/egl.h>
#include <stdint.h>

// current backends: drm and wayland
struct backend
{
    // data structure of the current backend
    void* d;
    // initializing the data structure and returning it
    void* (*init_data)();

    /*
     starts up the backend, up to the point of frame buffers
    */
    int (*init)(void* rs);

    // in resolution, not logical size
    uint32_t (*get_width)(void* d);
    uint32_t (*get_height)(void* d);

    // gives a buffer that is render ready
    struct redbuffer* (*pull_buffer)(void* d);
    // takes a buffer and displays it
    int (*push_buffer)(void* rs, struct redbuffer* rb);
    int (*push_init_buffer)(void* rs);
    int (*resize_buffer)(void* d, struct redbuffer* rb);

    // returns a fd that can be polled for events
    int (*get_fd)(void* d);
    int (*flush_events)(void* d);
    int (*handle_events)(void* d);

    int (*is_ready_for_frame)(void* d);

    int (*get_drm_node)(void* d);
    EGLDisplay (*get_egl_display)(void* d);
};
