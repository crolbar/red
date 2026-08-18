#pragma once

#include <EGL/egl.h>
#include <stdint.h>

typedef struct redstate redstate;

// initializes:
//   rs->backend
//   rs->backend->d
//   rs->is_wayland_client
//   other data assosiated with the backend like (libinput and libseat for drm backend)
int
init_backend(struct redstate* rs);

// current backends: drm and wayland
struct backend
{
    // data structure of the current backend
    void* d;

    // in resolution, not logical size
    uint32_t (*get_width)(void* d);
    uint32_t (*get_height)(void* d);

    // gives a buffer that is render ready
    struct redbuffer* (*pull_buffer)(void* d);
    struct redbuffer* (*get_current_buffer)(void* d);
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

    void (*destroy)(void* d);
};
