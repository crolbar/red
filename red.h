#pragma once

#include "backend.h"
#include "dll.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define RED_MOD_SHIFT   1
#define RED_MOD_CTRL    2
#define RED_MOD_ALT     4
#define RED_MOD_SUPER   8
#define RED_MOD_NO_MODS 0

#define min(x, y) ((x) < (y)) ? (x) : (y)
#define max(x, y) ((x) > (y)) ? (x) : (y)

#define RED_DMABUF_MAX_PLANES 4

struct dmabuf_format_modifier
{
    uint32_t format;
    uint64_t modifier;
};

struct pending_release
{
    struct wl_resource* buffer;
    EGLSyncKHR          sync;
};

struct dmabuf_plane
{
    int      fd;
    uint32_t offset;
    uint32_t stride;
    uint64_t modifier;
};

struct dmabuf_buffer_data
{
    struct redstate*    rs;
    int32_t             width, height;
    uint32_t            format;
    uint32_t            flags;
    int                 n_planes;
    struct dmabuf_plane planes[4];
};

struct dmabuf_buffer_data*
dmabuf_buffer_get(struct wl_resource* buffer);

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

    int32_t geom_x;
    int32_t geom_y;
    int32_t geom_width;
    int32_t geom_height;
    int     geom_configured;

    struct redsurface* parent;
    int                sub_x, sub_y;

    struct wl_resource* pending_buffer; // set by wl_surface.attach
    struct wl_resource* old_pending_buffer;
    struct wl_resource* pending_callback; // set by wl_surface.frame
    int                 configured;       // xdg_surface configure

    struct wl_resource*
      wp_viewport; /* the viewport object bound to this surface, or NULL */

    /* pending (uncommitted) viewport state */
    int     pending_viewport_src_set;
    double  pending_src_x, pending_src_y, pending_src_w, pending_src_h;
    int     pending_viewport_dst_set;
    int32_t pending_dst_w, pending_dst_h;

    /* committed (active) viewport state, applied at render time */
    int     viewport_src_set;
    double  src_x, src_y, src_w, src_h;
    int     viewport_dst_set;
    int32_t dst_w, dst_h;

    GLuint              tex;
    int32_t             tex_w;
    int32_t             tex_h;
    EGLImageKHR         egl_image;
    struct wl_resource* tex_buffer;
};

struct redclient
{
    struct redstate*    rs;
    struct redsurface*  rsurf;
    struct wl_client*   wl_client;
    struct wl_resource* wl_keyboard;
    struct wl_resource* wl_pointer;
    struct wl_listener  client_destroyed;
};

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
    const char*           wayland_display;
    struct wl_event_loop* wl_event_loop;
    struct wl_global*     wl_compositor;
    struct wl_global*     xdg_wm_base;
    struct wl_global*     xdg_decoration_manager;
    struct wl_global*     wl_output;
    struct wl_global*     wl_seat;
    struct wl_global*     subcompositor_global;
    struct wl_global*     data_device_manager_global;
    struct wl_global*     linux_dmabuf_global;
    struct wl_listener    client_created;

    struct dmabuf_format_modifier* dmabuf_formats;
    int                            dmabuf_formats_len;
    dev_t                          dmabuf_main_device;

    dll(struct pending_release) pending_releases;

    struct wl_global* viewporter_global;

    GLuint cursor_gl_program;
    GLuint cursor_gl_vao;
    double cursor_x;
    double cursor_y;
    int    using_hardware_cursor;

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
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNEGLCREATESYNCKHRPROC             eglCreateSyncKHR;
    PFNEGLGETSYNCATTRIBKHRPROC          eglGetSyncAttribKHR;
    PFNEGLDESTROYSYNCKHRPROC            eglDestroySyncKHR;
};

extern struct gl_proc* gl_proc;
