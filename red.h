#pragma once

#include "backend.h"
#include "dll.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-server.h>

typedef struct redbuffer
{
    uint32_t          buf_id;       // drm backend, drm framebuffer id
    struct wl_buffer* wl_buffer;    // wayland backend, wl_buffer
    int               free;         // wl_buffer free
    int               needs_resize; // only in wayland backend
    struct gbm_bo*    gbm_bo;
    EGLImageKHR       egl_image;
    GLuint            rbo, fbo;
} redbuffer;

struct redsurface
{
    struct redstate*    rs;
    struct wl_resource* wl_surface;
    struct wl_resource* xdg_surface;
    struct wl_resource* xdg_toplevel;
    char*               app_id; // xdg_toplevel title

    struct redsurface* parent;
    int                sub_x, sub_y;

    struct wl_resource* pending_buffer; // set by wl_surface.attach
    struct wl_resource* old_pending_buffer;
    struct wl_resource* pending_callback; // set by wl_surface.frame
    int                 configured;       // xdg_surface configure
};

struct redclient
{
    struct redstate*    rs;
    struct redsurface*  rsurf;
    struct wl_client*   wl_client;
    struct wl_resource* wl_keyboard;
    struct wl_listener  client_destroyed;
};

struct redstate
{
    struct backend*  backend;
    struct libinput* li;

    int tty_fd;
    int sig_fd;

    int is_wayland_client; // in wayland compositor spawn as a client
    int active;            // VT is active
    int should_quit;       // main loop condition
    int needs_redraw;      // changes were made to the focused client

    struct timespec* time_start;
    // frame info
    double last_frame_time;
    double frame_latency;

    double rect_x;
    double rect_y;

    struct wl_display*    wl_display;
    struct wl_event_loop* wl_event_loop;
    struct wl_global*     wl_compositor;
    struct wl_global*     xdg_wm_base;
    struct wl_global*     wl_output;
    struct wl_global*     wl_seat;
    struct wl_global*     subcompositor_global;
    struct wl_global*     data_device_manager_global;
    struct wl_listener    client_created;

    int32_t tex_w;
    int32_t tex_h;
    GLuint  tex;

    // all clients
    dll(struct redclient*) rcs; // red clients

    // clients that have xdg_toplevel as wl_surface
    dll(struct redclient*) trcs; // top red clients
    struct redclient* focused_trc;
};

struct gl_proc
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLCREATEIMAGEKHRPROC        eglCreateImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
};

extern struct gl_proc* gl_proc;
