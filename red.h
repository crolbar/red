#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

typedef struct redbuffer
{
    uint32_t          buf_id;       // drm backend, drm framebuffer id
    struct wl_buffer* wl_buffer;    // wayland backend, wl_buffer
    int               free;         // wl_buffer free
    int               needs_resize;
    struct gbm_bo*    gbm_bo;
    EGLImageKHR       egl_image;
    GLuint            rbo, fbo;
} redbuffer;

struct redstate
{
    struct drmstate*             drm;
    struct client_wayland_state* wl;
    struct libinput*             li;
    struct glProc*               glProc;

    int tty_fd;
    int sig_fd;

    struct redbuffer* rb0;
    struct redbuffer* rb1;
    int               used_rb; // indicates which buffer is displayed

    int rrender_fd; // read 1, and trigger render
    int wrender_fd; // write 1, to trigger render

    // TODO: use this
    int is_wayland_client; // in wayland compositor spawn as a client
    int active;            // VT is active
    int should_quit;

    struct timespec* time_start;
    double           last_frame_time;
    double           frame_latency;

    double rect_x;
    double rect_y;
};
