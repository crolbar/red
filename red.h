#pragma once

#include "backend.h"
#include "dll.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <libinput.h>
#include <sys/poll.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define RED_MOD_SHIFT   1
#define RED_MOD_CTRL    2
#define RED_MOD_ALT     4
#define RED_MOD_SUPER   8
#define RED_MOD_NO_MODS 0

#define min(x, y) ((x) < (y)) ? (x) : (y)
#define max(x, y) ((x) > (y)) ? (x) : (y)

struct positioner_data
{

    int32_t  off_x;
    int32_t  off_y;
    int32_t  width;
    int32_t  height;
    uint32_t gravity;
    uint32_t anchor;
    int32_t  anchor_x;
    int32_t  anchor_y;
    int32_t  anchor_width;
    int32_t  anchor_height;
};

struct dmabuf_plane
{
    int32_t  fd;
    uint32_t offset;
    uint32_t stride;
    uint32_t modifier_hi;
    uint32_t modifier_lo;
};
struct dmabuf_params
{
    struct redstate*    rs;
    int                 planes_count;
    struct dmabuf_plane planes[4];
};

struct dmabuf
{
    struct redstate*    rs;
    int                 planes_count;
    int32_t             width;
    int32_t             height;
    uint32_t            format;
    uint32_t            flags;
    struct dmabuf_plane planes[4];
    EGLImageKHR         egl_img;
};

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

struct redsubsurface
{
    struct redsurface* rsurf;
};

enum red_surf_commited
{
    RED_SURF_COMMITED_BUFFER = 1 << 0,
};

struct redsurface
{
    struct redstate*    rs;
    struct redclient*   rc;
    struct wl_resource* wl_surface;
    struct wl_resource* xdg_surface;
    struct wl_resource* xdg_toplevel;
    struct wl_resource* xdg_popup;
    struct wl_resource* zwlr_layer_surface;

    struct redsurface* parent;
    // containing subsurfaceses and xdg_popups
    dll(struct redsurface*) subsurfs;

    // set in:
    //   subsurf x and y
    //   popup unscaled x and y
    int32_t x;
    int32_t y;

    // currently set when binding gl texture
    int32_t w;
    int32_t h;

    // gl texture
    GLuint gl_tex;

    int32_t geom_x;
    int32_t geom_y;
    int32_t geom_width;
    int32_t geom_height;
    int     geom_configured;

    int32_t buffer_scale;
    int     buffer_scale_set;

    uint32_t commited; // red_surf_commited bitmask

    struct wl_resource* pending_buffer;           // set by wl_surface.attach
    struct wl_listener  pending_buffer_destroyed; //

    struct wl_resource* current_buffer;           // set by wl_surface.commit
    struct wl_listener  current_buffer_destroyed; //
    // 0 not init, 1 -> shm, 2 -> dma
    int old_rendered_buf_type;

    dll(struct wl_resource*) pending_frame_cbs; // wl_surface.frame
    dll(struct wl_resource*) pending_pres_cbs;  // wp_presentation_feedback

    int configured; // xdg_surface configure

    int32_t  layer_width;
    int32_t  layer_height;
    int32_t  layer_margin_top;
    int32_t  layer_margin_right;
    int32_t  layer_margin_bottom;
    int32_t  layer_margin_left;
    uint32_t layer_anchor;
};

struct redclient
{
    struct redstate* rs;
    dll(struct redsurface*) rsurfs;

    struct wl_client*   wl_client;
    struct wl_resource* wl_keyboard;
    struct wl_resource* wl_pointer;
    struct wl_listener  client_destroyed;
};

struct redtoplevel
{
    struct redstate*   rs;
    struct redclient*  rc;
    struct redsurface* rsurf;
    char*              app_id;
};

// mods is a bitmask
typedef struct redbind
{
    uint8_t mods;
    char*   key;
    char**  action;
    size_t  action_len;
} redbind;

enum redpfds
{
    RFD_LIBINPUT,
    RFD_SIGNALS,
    RFD_BACKEND,
    RFD_WAYLAND,
    RFD_CURSOR,
    RFD_REDRAWSYNC,
    __REDPFDS_SIZE,
    __REDPFDS_NONE,
};

struct redstate
{
    struct backend*     backend;
    struct libinput*    li;
    struct xkb_context* xkb;
    struct pollfd*      pfds;

    int tty_fd;
    int sig_fd;
    int li_fd;
    int backend_fd;
    int wl_event_loop_fd;

    int is_wayland_client; // in wayland compositor spawn as a client
    int active;            // VT is active
    int should_quit;       // main loop condition
    int needs_redraw;      // changes were made to the focused client
    int should_draw;       // stop rendering at all. (using 2 as draw blank)

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
    struct wl_global*     zwp_linux_dmabuf;
    struct wl_global*     wp_viewporter;
    struct wl_global*     zwp_relative_pointer_manager;
    struct wl_global*     zwp_pointer_constraints;
    struct wl_global*     zwlr_layer_shell;
    struct wl_global*     wp_presentation;
    struct wl_global*     subcompositor_global;
    struct wl_global*     data_device_manager_global;
    struct wl_listener    client_created;

    GLuint program;
    GLuint vao;
    GLuint vbo;
    GLint  texture_loc;
    GLint  dimentions_loc;

    GLuint   cursor_gl_program;
    GLuint   cursor_gl_vao;
    double   cursor_x;
    double   cursor_y;
    int      using_hardware_cursor;
    uint32_t cursor_last_motion_time;
    uint32_t cursor_last_scroll_time;
    int      cursor_hide_timer_fd;
    int      cursor_locked;
    int      cursor_hidden;

    dll(struct wl_resource*) relative_pointers;

    // all clients
    dll(struct redclient*) rcs; // red clients

    // clients that have xdg_toplevel as wl_surface
    dll(struct redtoplevel*) rts; // red toplevels
    struct redtoplevel* focused_rt;
    struct redsurface*  pointer_focused_rsurf;

    dll(struct redsurface*) layer_rsurfs;

    int                xkb_keymap_fd;
    char*              xkb_keymap_string;
    struct xkb_keymap* xkb_keymap;
    size_t             xkb_keymap_size;
    struct xkb_state*  xkb_state;
    xkb_mod_mask_t     xkb_mods_depressed;
    xkb_mod_mask_t     xkb_mods_latched;
    xkb_mod_mask_t     xkb_mods_locked;
    xkb_layout_index_t xkb_group;

    struct redbuffer* queued_rb; // buffer we got queued rendering to
};

extern struct gl_proc* gl_proc;
