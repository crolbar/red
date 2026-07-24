#pragma once

#include "backend.h"
#include "dll.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

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

#define RED_MOD_SHIFT   1
#define RED_MOD_CTRL    2
#define RED_MOD_ALT     4
#define RED_MOD_SUPER   8
#define RED_MOD_NO_MODS 0

// mods is a bitmask
typedef struct redbind
{
    uint8_t mods;
    char*   key;
    char**  action;
    size_t  action_len;
} redbind;

struct redstate
{
    struct backend*     backend;
    struct libinput*    li;
    struct xkb_context* xkb;

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

    GLuint cursor_gl_program;
    GLuint cursor_gl_vao;
    double cursor_x;
    double cursor_y;

    // all clients
    dll(struct redclient*) rcs; // red clients

    // clients that have xdg_toplevel as wl_surface
    dll(struct redclient*) trcs; // top red clients
    struct redclient* focused_trc;

    int                xkb_keymap_fd;
    char*              xkb_keymap_string;
    struct xkb_keymap* xkb_keymap;
    size_t             xkb_keymap_size;
    struct xkb_state*  xkb_state;
    xkb_mod_mask_t     xkb_mods_depressed;
    xkb_mod_mask_t     xkb_mods_latched;
    xkb_mod_mask_t     xkb_mods_locked;
    xkb_layout_index_t xkb_group;
};

struct gl_proc
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
    PFNEGLCREATEIMAGEKHRPROC        eglCreateImageKHR;
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
    glEGLImageTargetRenderbufferStorageOES;
};

extern struct gl_proc* gl_proc;
