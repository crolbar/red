/*
 * tinycompositor.c -- a deliberately minimal Wayland compositor.
 *
 * Goal: show ONE client window on a real display using nothing but:
 *   - libwayland-server   (the wl_display/wl_surface/wl_shm protocol machinery)
 *   - libdrm              (talk directly to the kernel's KMS display driver)
 *   - the generated xdg-shell protocol glue (xdg-shell-protocol.c/.h)
 *
 * No wlroots, no libinput, no EGL/GL, no page-flip/vsync handling.
 * It is meant to be READ, not shipped. Once you understand every line here,
 * go look at wlroots' tinywl.c which does the same thing "for real".
 *
 * -----------------------------------------------------------------------
 * WHAT IT DOES
 * -----------------------------------------------------------------------
 * 1. Opens a DRM device (e.g. /dev/dri/card0), finds a connected monitor,
 *    picks a mode, and allocates ONE "dumb buffer" (plain CPU-writable
 *    framebuffer memory) which it scans out to the screen immediately
 *    with drmModeSetCrtc(). That buffer is the entire "screen".
 *
 * 2. Opens a Wayland listening socket (wl_display) and advertises a
 *    handful of globals to clients: wl_compositor, wl_shm (built into
 *    libwayland-server), wl_output, wl_seat (empty/no input), and
 *    xdg_wm_base (the modern desktop-shell protocol every normal
 *    application, e.g. GTK/Qt/foot/weston-terminal, uses to get a window).
 *
 * 3. When a client creates a surface, turns it into an xdg_toplevel, and
 *    commits a buffer, we grab the pixels straight out of the client's
 *    shared-memory buffer (wl_shm) and memcpy them into our DRM
 *    framebuffer at a fixed on-screen position. Because that framebuffer
 *    is already being scanned out, the pixels appear on the monitor the
 *    instant we write them -- no page flip needed.
 *
 * -----------------------------------------------------------------------
 * WHAT IT DELIBERATELY DOES NOT DO (so you can add it yourself later)
 * -----------------------------------------------------------------------
 *   - No input (keyboard/mouse). wl_seat is advertised with zero
 *     capabilities so clients don't hang waiting for a seat, but nothing
 *     is delivered.
 *   - No damage tracking / partial repaint -- every commit repaints the
 *     whole attached buffer.
 *   - No multiple windows, no window management, no resizing by the user.
 *   - No page-flip/vblank sync -- can tear. Real compositors double-buffer
 *     and use DRM page-flip events.
 *   - Only handles XRGB8888/ARGB8888 client buffers.
 *
 * It DOES, however, implement the VT-switch handshake (see the "VT
 * SWITCHING" section below) -- without that, pressing Ctrl+Alt+F<n> to
 * leave the VT this compositor is running on hangs the console, because
 * the kernel is waiting for us to release DRM master and never gets a
 * response. Real compositors normally get this for free from libseat
 * (talking to seatd or logind); here it's done by hand with raw VT
 * ioctls so you can see what libseat is actually doing underneath.
 *
 * -----------------------------------------------------------------------
 * BUILD (see accompanying Makefile / README)
 * -----------------------------------------------------------------------
 *   gcc compositor.c xdg-shell-protocol.c -o tinycompositor \
 *       $(pkg-config --cflags --libs wayland-server libdrm)
 *
 * -----------------------------------------------------------------------
 * RUN
 * -----------------------------------------------------------------------
 * This is a REAL display-server: it takes over a monitor via KMS. Run it
 * from a free virtual terminal (Ctrl+Alt+F3, log in, NOT inside an
 * existing X11/Wayland session), as a user in the "video" and "render"
 * groups (or as root):
 *
 *   $ ./tinycompositor
 *   -> prints "WAYLAND_DISPLAY=wayland-1" (or similar)
 *
 * Then, from another VT (or over SSH), in a second shell:
 *
 *   $ WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/$(id -u)
 * weston-terminal
 *
 * or any other simple Wayland client. Its window should appear at the
 * top-left corner of the screen.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "xdg-shell-protocol.h"

/* ======================================================================
 * DRM / KMS: talking to the kernel display driver
 * ====================================================================== */

struct drm_output
{
    int fd;

    uint32_t connector_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    int width, height;

    /* the one and only framebuffer we scan out */
    uint32_t fb_id;
    uint32_t handle; /* dumb-buffer handle */
    uint32_t pitch;  /* bytes per row, given to us by the kernel */
    uint64_t size;
    uint8_t* pixels; /* mmap()ed CPU pointer to the framebuffer */
};

/* Find the first connected connector, its preferred mode, and a CRTC that
 * can drive it. This is the classic "legacy KMS" dance -- an atomic-KMS
 * compositor would do this very differently (and more robustly). */
static int
drm_output_init(struct drm_output* out, const char* device_path)
{
    out->fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (out->fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", device_path, strerror(errno));
        return -1;
    }

    drmModeRes* res = drmModeGetResources(out->fd);
    if (!res) {
        fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
        return -1;
    }

    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(out->fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        if (c)
            drmModeFreeConnector(c);
    }
    if (!conn) {
        fprintf(stderr, "no connected display found\n");
        drmModeFreeResources(res);
        return -1;
    }

    out->connector_id = conn->connector_id;
    out->mode = conn->modes[0]; /* modes[0] is the preferred/highest mode */
    out->width = out->mode.hdisplay;
    out->height = out->mode.vdisplay;

    /* Find an encoder/CRTC pair that works with this connector. */
    drmModeEncoder* enc = NULL;
    if (conn->encoder_id)
        enc = drmModeGetEncoder(out->fd, conn->encoder_id);

    int crtc_id = -1;
    if (enc && enc->crtc_id) {
        crtc_id = enc->crtc_id;
    } else {
        /* Fall back: scan all encoders this connector supports, and all
         * CRTCs each encoder supports, for the first usable combination. */
        for (int i = 0; i < conn->count_encoders && crtc_id < 0; i++) {
            drmModeEncoder* e = drmModeGetEncoder(out->fd, conn->encoders[i]);
            if (!e)
                continue;
            for (int j = 0; j < res->count_crtcs; j++) {
                if (e->possible_crtcs & (1 << j)) {
                    crtc_id = res->crtcs[j];
                    break;
                }
            }
            drmModeFreeEncoder(e);
        }
    }
    if (enc)
        drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    if (crtc_id < 0) {
        fprintf(stderr, "no usable CRTC found for connector\n");
        return -1;
    }
    out->crtc_id = (uint32_t)crtc_id;
    return 0;
}

/* Allocate one CPU-writable "dumb" framebuffer and hand it to KMS so it is
 * scanned out to the monitor right now. From this point on, anything we
 * write into out->pixels shows up on screen. */
static int
drm_output_create_framebuffer(struct drm_output* out)
{
    struct drm_mode_create_dumb create = { 0 };
    create.width = out->width;
    create.height = out->height;
    create.bpp = 32;

    if (drmIoctl(out->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(
          stderr, "DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n", strerror(errno));
        return -1;
    }
    out->handle = create.handle;
    out->pitch = create.pitch;
    out->size = create.size;

    if (drmModeAddFB(out->fd,
                     out->width,
                     out->height,
                     24,
                     32,
                     out->pitch,
                     out->handle,
                     &out->fb_id)) {
        fprintf(stderr, "drmModeAddFB failed: %s\n", strerror(errno));
        return -1;
    }

    struct drm_mode_map_dumb map = { 0 };
    map.handle = out->handle;
    if (drmIoctl(out->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(
          stderr, "DRM_IOCTL_MODE_MAP_DUMB failed: %s\n", strerror(errno));
        return -1;
    }
    out->pixels = mmap(
      0, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, map.offset);
    if (out->pixels == MAP_FAILED) {
        fprintf(stderr, "mmap of dumb buffer failed: %s\n", strerror(errno));
        return -1;
    }

    /* Paint it a dark grey "desktop background" so we can tell the
     * compositor is alive even before any client connects. */
    memset(out->pixels, 0x20, out->size);

    if (drmModeSetCrtc(out->fd,
                       out->crtc_id,
                       out->fb_id,
                       0,
                       0,
                       &out->connector_id,
                       1,
                       &out->mode)) {
        fprintf(stderr, "drmModeSetCrtc failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ======================================================================
 * Compositor state and the one window we support
 * ====================================================================== */

struct tc_server
{
    struct wl_display* display;
    struct wl_event_loop* loop;
    struct drm_output drm;

    /* /dev/tty of the VT we're running on -- see "VT SWITCHING" below */
    int vt_fd;
    int have_drm_master;

    struct wl_global* compositor_global;
    struct wl_global* xdg_wm_base_global;
    struct wl_global* output_global;
    struct wl_global* seat_global;
    struct wl_global* subcompositor_global;
    struct wl_global* data_device_manager_global;

    /* We only ever draw ONE window, positioned here. Track it so we know
     * where on the DRM framebuffer to blit its pixels. */
    int win_x, win_y;
};

struct tc_surface
{
    struct tc_server* server;
    struct wl_resource* resource;     /* wl_surface */
    struct wl_resource* xdg_surface;  /* xdg_surface, once created */
    struct wl_resource* xdg_toplevel; /* xdg_toplevel, once created */

    struct tc_surface* parent;
    int sub_x, sub_y;

    struct wl_resource* pending_buffer;   /* set by wl_surface.attach */
    struct wl_resource* pending_callback; /* set by wl_surface.frame */

    int configured; /* has xdg config been acked? */
};

/* ----------------------------------------------------------------------
 * wl_region -- clients use this for input/opaque regions. We don't do
 * hit-testing or damage optimisation, so it's a pure no-op stub.
 * -------------------------------------------------------------------- */

static void
region_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}
static void
region_add(struct wl_client* c,
           struct wl_resource* r,
           int32_t x,
           int32_t y,
           int32_t w,
           int32_t h)
{
}
static void
region_subtract(struct wl_client* c,
                struct wl_resource* r,
                int32_t x,
                int32_t y,
                int32_t w,
                int32_t h)
{
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

static void
compositor_create_region(struct wl_client* client,
                         struct wl_resource* resource,
                         uint32_t id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_region_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &region_impl, NULL, NULL);
}

static void
surface_screen_position(struct tc_surface* surf, int* out_x, int* out_y)
{
    int x = 0, y = 0;
    struct tc_surface* s = surf;
    while (s->parent) {
        x += s->sub_x;
        y += s->sub_y;
        s = s->parent;
    }
    *out_x = s->server->win_x + x;
    *out_y = s->server->win_y + y;
}

/* ----------------------------------------------------------------------
 * The actual repaint: copy the client's shm pixels into the DRM
 * framebuffer at (win_x, win_y). This is the whole "renderer".
 * -------------------------------------------------------------------- */

static void
blit_surface_to_screen(struct tc_surface* surf)
{
    struct tc_server* server = surf->server;
    struct wl_resource* buffer = surf->pending_buffer;
    if (!buffer)
        return;

    struct wl_shm_buffer* shm = wl_shm_buffer_get(buffer);
    if (!shm) {
        /* Not an shm buffer (could be a DMA-BUF/EGL buffer) -- unsupported
         * by this tiny compositor. */
        return;
    }

    wl_shm_buffer_begin_access(shm);
    uint8_t* src = wl_shm_buffer_get_data(shm);
    int32_t src_stride = wl_shm_buffer_get_stride(shm);
    int32_t w = wl_shm_buffer_get_width(shm);
    int32_t h = wl_shm_buffer_get_height(shm);

    struct drm_output* out = &server->drm;
    int win_x, win_y;
    surface_screen_position(surf, &win_x, &win_y);

    for (int row = 0; row < h; row++) {
        int screen_y = win_y + row;
        if (screen_y < 0 || screen_y >= out->height)
            continue;

        int copy_w = w;
        if (win_x + copy_w > out->width)
            copy_w = out->width - win_x;
        if (copy_w <= 0 || win_x >= out->width || win_x < 0)
            continue;

        uint8_t* srow = src + row * src_stride;
        uint8_t* drow = out->pixels + screen_y * out->pitch + win_x * 4;
        /* Both sides are 32-bit XRGB/ARGB -- straight byte copy. */
        memcpy(drow, srow, copy_w * 4);
    }

    wl_shm_buffer_end_access(shm);

    /* Tell the client we're done reading its buffer so it may reuse it. */
    wl_buffer_send_release(buffer);
    surf->pending_buffer = NULL;

    /* Fire any pending frame callback so clients that redraw only in
     * response to frame events (e.g. for animation) get to run again.
     * A "real" compositor would send this right after an actual vblank;
     * we just fire it immediately since we have no vsync timing here. */
    if (surf->pending_callback) {
        uint32_t now_ms = (uint32_t)(wl_display_get_serial(server->display));
        wl_callback_send_done(surf->pending_callback, now_ms);
        wl_resource_destroy(surf->pending_callback);
        surf->pending_callback = NULL;
    }
}

/* ----------------------------------------------------------------------
 * wl_surface
 * -------------------------------------------------------------------- */

static void
surface_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
surface_attach(struct wl_client* client,
               struct wl_resource* resource,
               struct wl_resource* buffer,
               int32_t x,
               int32_t y)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    surf->pending_buffer = buffer; /* NULL buffer means "hide this surface" */
}

static void
surface_damage(struct wl_client* c,
               struct wl_resource* r,
               int32_t x,
               int32_t y,
               int32_t w,
               int32_t h)
{
}
static void
surface_damage_buffer(struct wl_client* c,
                      struct wl_resource* r,
                      int32_t x,
                      int32_t y,
                      int32_t w,
                      int32_t h)
{
}
static void
surface_set_opaque_region(struct wl_client* c,
                          struct wl_resource* r,
                          struct wl_resource* region)
{
}
static void
surface_set_input_region(struct wl_client* c,
                         struct wl_resource* r,
                         struct wl_resource* region)
{
}
static void
surface_set_buffer_transform(struct wl_client* c,
                             struct wl_resource* r,
                             int32_t t)
{
}
static void
surface_set_buffer_scale(struct wl_client* c,
                         struct wl_resource* r,
                         int32_t scale)
{
}

static void
surface_frame(struct wl_client* client,
              struct wl_resource* resource,
              uint32_t callback_id)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    struct wl_resource* cb =
      wl_resource_create(client, &wl_callback_interface, 1, callback_id);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);
    /* Only remember the latest one -- good enough for a single window. */
    surf->pending_callback = cb;
}

static void
surface_commit(struct wl_client* client, struct wl_resource* resource)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);

    /* Per xdg-shell, the very first commit on a not-yet-configured
     * xdg_surface must NOT have a buffer attached; it's just the client
     * telling us it wants a configure event. We already sent one when
     * get_toplevel() was called (see below), so there's nothing to do
     * here except wait for the client's next, real commit. */
    if (surf->xdg_surface && !surf->configured)
        return;

    blit_surface_to_screen(surf);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    /* .offset intentionally omitted (added in newer protocol versions);
     * leaving it NULL is fine since we advertise version 4 below. */
};

static void
surface_resource_destroy(struct wl_resource* resource)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    free(surf);
}

/* ----------------------------------------------------------------------
 * wl_compositor
 * -------------------------------------------------------------------- */

static void
compositor_create_surface(struct wl_client* client,
                          struct wl_resource* resource,
                          uint32_t id)
{
    struct tc_server* server = wl_resource_get_user_data(resource);

    struct tc_surface* surf = calloc(1, sizeof(*surf));
    surf->server = server;

    surf->resource = wl_resource_create(
      client, &wl_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(
      surf->resource, &surface_impl, surf, surface_resource_destroy);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void
bind_compositor(struct wl_client* client,
                void* data,
                uint32_t version,
                uint32_t id)
{
    struct wl_resource* r =
      wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}

/* ----------------------------------------------------------------------
 * wl_data_device_manager -- clipboard / drag-and-drop.
 * -------------------------------------------------------------------- */
static void
data_source_offer(struct wl_client* c,
                  struct wl_resource* r,
                  const char* mime_type)
{
}
static void
data_source_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}
static void
data_source_set_actions(struct wl_client* c,
                        struct wl_resource* r,
                        uint32_t dnd_actions)
{
}

static const struct wl_data_source_interface data_source_impl = {
    .offer = data_source_offer,
    .destroy = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void
data_device_start_drag(struct wl_client* client,
                       struct wl_resource* resource,
                       struct wl_resource* source,
                       struct wl_resource* origin,
                       struct wl_resource* icon,
                       uint32_t serial)
{
}

static void
data_device_set_selection(struct wl_client* client,
                          struct wl_resource* resource,
                          struct wl_resource* source,
                          uint32_t serial)
{
}

static void
data_device_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release = data_device_release,
};

static void
data_device_manager_create_data_source(struct wl_client* client,
                                       struct wl_resource* resource,
                                       uint32_t id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_source_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &data_source_impl, NULL, NULL);
}

static void
data_device_manager_get_data_device(struct wl_client* client,
                                    struct wl_resource* resource,
                                    uint32_t id,
                                    struct wl_resource* seat)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_device_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &data_device_impl, NULL, NULL);
}

static const struct wl_data_device_manager_interface
  data_device_manager_impl = {
      .create_data_source = data_device_manager_create_data_source,
      .get_data_device = data_device_manager_get_data_device,
  };

static void
bind_data_device_manager(struct wl_client* client,
                         void* data,
                         uint32_t version,
                         uint32_t id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_device_manager_interface, version, id);
    wl_resource_set_implementation(r, &data_device_manager_impl, data, NULL);
}

/* ----------------------------------------------------------------------
 * wl_subcompositor / wl_subsurface
 * -------------------------------------------------------------------- */

static void
subsurface_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
subsurface_set_position(struct wl_client* client,
                        struct wl_resource* resource,
                        int32_t x,
                        int32_t y)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    surf->sub_x = x;
    surf->sub_y = y;
}

static void
subsurface_place_above(struct wl_client* c,
                       struct wl_resource* r,
                       struct wl_resource* sibling)
{
}
static void
subsurface_place_below(struct wl_client* c,
                       struct wl_resource* r,
                       struct wl_resource* sibling)
{
}
static void
subsurface_set_sync(struct wl_client* c, struct wl_resource* r)
{
}
static void
subsurface_set_desync(struct wl_client* c, struct wl_resource* r)
{
}

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

static void
subcompositor_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
subcompositor_get_subsurface(struct wl_client* client,
                             struct wl_resource* resource,
                             uint32_t id,
                             struct wl_resource* surface_resource,
                             struct wl_resource* parent_resource)
{
    struct tc_surface* surf = wl_resource_get_user_data(surface_resource);
    struct tc_surface* parent = wl_resource_get_user_data(parent_resource);
    surf->parent = parent;

    struct wl_resource* r = wl_resource_create(
      client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &subsurface_impl, surf, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void
bind_subcompositor(struct wl_client* client,
                   void* data,
                   uint32_t version,
                   uint32_t id)
{
    struct wl_resource* r =
      wl_resource_create(client, &wl_subcompositor_interface, version, id);
    wl_resource_set_implementation(r, &subcompositor_impl, data, NULL);
}

/* ----------------------------------------------------------------------
 * xdg_toplevel / xdg_surface / xdg_wm_base
 * -------------------------------------------------------------------- */

static void
toplevel_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}
static void
toplevel_set_title(struct wl_client* c,
                   struct wl_resource* r,
                   const char* title)
{
    fprintf(stderr, "[tinycompositor] window title: %s\n", title);
}
static void
toplevel_set_app_id(struct wl_client* c, struct wl_resource* r, const char* id)
{
}
static void
toplevel_set_parent(struct wl_client* c,
                    struct wl_resource* r,
                    struct wl_resource* p)
{
}
static void
toplevel_show_window_menu(struct wl_client* c,
                          struct wl_resource* r,
                          struct wl_resource* seat,
                          uint32_t serial,
                          int32_t x,
                          int32_t y)
{
}
static void
toplevel_move(struct wl_client* c,
              struct wl_resource* r,
              struct wl_resource* seat,
              uint32_t serial)
{
}
static void
toplevel_resize(struct wl_client* c,
                struct wl_resource* r,
                struct wl_resource* seat,
                uint32_t serial,
                uint32_t edges)
{
}
static void
toplevel_set_max_size(struct wl_client* c,
                      struct wl_resource* r,
                      int32_t w,
                      int32_t h)
{
}
static void
toplevel_set_min_size(struct wl_client* c,
                      struct wl_resource* r,
                      int32_t w,
                      int32_t h)
{
}
static void
toplevel_set_maximized(struct wl_client* c, struct wl_resource* r)
{
}
static void
toplevel_unset_maximized(struct wl_client* c, struct wl_resource* r)
{
}
static void
toplevel_set_fullscreen(struct wl_client* c,
                        struct wl_resource* r,
                        struct wl_resource* output)
{
}
static void
toplevel_unset_fullscreen(struct wl_client* c, struct wl_resource* r)
{
}
static void
toplevel_set_minimized(struct wl_client* c, struct wl_resource* r)
{
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

static void
xdgsurface_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
xdgsurface_ack_configure(struct wl_client* client,
                         struct wl_resource* resource,
                         uint32_t serial)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    surf->configured = 1;
}

static void
xdgsurface_set_window_geometry(struct wl_client* c,
                               struct wl_resource* r,
                               int32_t x,
                               int32_t y,
                               int32_t w,
                               int32_t h)
{
}

static void
xdgsurface_get_popup(struct wl_client* client,
                     struct wl_resource* resource,
                     uint32_t id,
                     struct wl_resource* parent,
                     struct wl_resource* positioner)
{
    /* Popups (menus/tooltips) are not implemented by this tiny compositor.
     * Rather than protocol-erroring the client out (which would kill the
     * whole connection), just hand back an inert object that never gets
     * a configure event. Good enough for a learning example. */
    struct wl_resource* r = wl_resource_create(
      client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, NULL, NULL, NULL);
}

static void
xdgsurface_get_toplevel(struct wl_client* client,
                        struct wl_resource* resource,
                        uint32_t id)
{
    struct tc_surface* surf = wl_resource_get_user_data(resource);
    struct tc_server* server = surf->server;

    surf->xdg_toplevel = wl_resource_create(
      client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(
      surf->xdg_toplevel, &toplevel_impl, surf, NULL);

    /* Tell the client the size/state we'd like (0x0 = "you choose"), then
     * the matching xdg_surface.configure that must follow every toplevel
     * configure. The client must ack this before it may attach a buffer. */
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(
      surf->xdg_toplevel, server->drm.width, server->drm.height, &states);
    wl_array_release(&states);

    uint32_t serial = wl_display_next_serial(server->display);
    xdg_surface_send_configure(resource, serial);
}

static const struct xdg_surface_interface xdgsurface_impl = {
    .destroy = xdgsurface_destroy,
    .get_toplevel = xdgsurface_get_toplevel,
    .get_popup = xdgsurface_get_popup,
    .set_window_geometry = xdgsurface_set_window_geometry,
    .ack_configure = xdgsurface_ack_configure,
};

static void
wmbase_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
wmbase_create_positioner(struct wl_client* client,
                         struct wl_resource* resource,
                         uint32_t id)
{
    /* Only needed for popups, which we don't support; create a dummy
     * inert object so well-behaved clients don't crash if they call this
     * speculatively. */
    struct wl_resource* r = wl_resource_create(
      client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, NULL, NULL, NULL);
}

static void
wmbase_get_xdg_surface(struct wl_client* client,
                       struct wl_resource* resource,
                       uint32_t id,
                       struct wl_resource* surface_resource)
{
    struct tc_surface* surf = wl_resource_get_user_data(surface_resource);

    surf->xdg_surface = wl_resource_create(
      client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(
      surf->xdg_surface, &xdgsurface_impl, surf, NULL);
}

static void
wmbase_pong(struct wl_client* c, struct wl_resource* r, uint32_t serial)
{
}

static const struct xdg_wm_base_interface wmbase_impl = {
    .destroy = wmbase_destroy,
    .create_positioner = wmbase_create_positioner,
    .get_xdg_surface = wmbase_get_xdg_surface,
    .pong = wmbase_pong,
};

static void
bind_wm_base(struct wl_client* client,
             void* data,
             uint32_t version,
             uint32_t id)
{
    struct wl_resource* r =
      wl_resource_create(client, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(r, &wmbase_impl, data, NULL);
}

/* ----------------------------------------------------------------------
 * wl_output -- advertise our one monitor's geometry/mode so clients (and
 * their toolkits) know what size window makes sense.
 * -------------------------------------------------------------------- */

static void
output_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}
static const struct wl_output_interface output_impl = { .release =
                                                          output_release };

static void
bind_output(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    struct tc_server* server = data;
    struct wl_resource* r =
      wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(r, &output_impl, data, NULL);

    wl_output_send_geometry(r,
                            0,
                            0,
                            300,
                            200,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "tinycompositor",
                            "virtual",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        server->drm.width,
                        server->drm.height,
                        60000);
    if (version >= 2)
        wl_output_send_scale(r, 1);
    if (version >= 4)
        wl_output_send_name(r, "TINY-1");
    wl_output_send_done(r);
}

/* ----------------------------------------------------------------------
 * wl_seat -- advertised with zero capabilities. We don't implement input
 * in this minimal compositor, but many clients assert a seat exists.
 * -------------------------------------------------------------------- */

static void
seat_get_pointer(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    wl_resource_post_error(
      r, WL_SEAT_ERROR_MISSING_CAPABILITY, "no pointer capability");
}
static void
seat_get_keyboard(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    wl_resource_post_error(
      r, WL_SEAT_ERROR_MISSING_CAPABILITY, "no keyboard capability");
}
static void
seat_get_touch(struct wl_client* c, struct wl_resource* r, uint32_t id)
{
    wl_resource_post_error(
      r, WL_SEAT_ERROR_MISSING_CAPABILITY, "no touch capability");
}
static void
seat_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void
bind_seat(struct wl_client* client, void* data, uint32_t version, uint32_t id)
{
    struct wl_resource* r =
      wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(r, &seat_impl, data, NULL);
    wl_seat_send_capabilities(r, 0); /* no pointer, no keyboard, no touch */
    if (version >= 2)
        wl_seat_send_name(r, "seat0");
}

static int
on_vt_release(int signal_number, void* data)
{
    struct tc_server* server = data;
    fprintf(stderr,
            "[tinycompositor] VT release requested, giving up the display\n");

    if (server->have_drm_master) {
        drmDropMaster(server->drm.fd);
        server->have_drm_master = 0;
    }
    ioctl(server->vt_fd, VT_RELDISP, 1);
    return 1;
}

static int
on_vt_acquire(int signal_number, void* data)
{
    struct tc_server* server = data;
    fprintf(stderr,
            "[tinycompositor] VT acquired again, reclaiming the display\n");

    ioctl(server->vt_fd, VT_RELDISP, VT_ACKACQ);

    if (drmSetMaster(server->drm.fd) == 0) {
        server->have_drm_master = 1;
        /* The screen may show whatever the previous VT owner left behind
         * -- repaint our framebuffer's mode/scanout and last contents. */
        drmModeSetCrtc(server->drm.fd,
                       server->drm.crtc_id,
                       server->drm.fb_id,
                       0,
                       0,
                       &server->drm.connector_id,
                       1,
                       &server->drm.mode);
    } else {
        fprintf(stderr,
                "[tinycompositor] drmSetMaster failed on VT acquire: %s\n",
                strerror(errno));
    }
    return 1;
}

static int
on_terminate_signal(int signal_number, void* data)
{
    struct tc_server* server = data;
    wl_display_terminate(server->display);
    return 1;
}

/* Returns 0 on success. On failure we keep running anyway (with a
 * warning) rather than refusing to start -- e.g. this fails harmlessly
 * if you're not actually attached to a VT (some remote/CI setups). */
static int
vt_switching_init(struct tc_server* server)
{
    server->vt_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (server->vt_fd < 0) {
        fprintf(stderr,
                "[tinycompositor] warning: couldn't open /dev/tty (%s); "
                "VT switching will not work correctly\n",
                strerror(errno));
        return -1;
    }

    if (ioctl(server->vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        fprintf(stderr,
                "[tinycompositor] warning: KDSETMODE KD_GRAPHICS failed: %s\n",
                strerror(errno));
    }

    struct vt_mode mode = { 0 };
    mode.mode = VT_PROCESS;
    mode.relsig = SIGUSR1;
    mode.acqsig = SIGUSR2;
    if (ioctl(server->vt_fd, VT_SETMODE, &mode) < 0) {
        fprintf(stderr,
                "[tinycompositor] warning: VT_SETMODE VT_PROCESS failed: %s\n",
                strerror(errno));
        return -1;
    }

    wl_event_loop_add_signal(server->loop, SIGUSR1, on_vt_release, server);
    wl_event_loop_add_signal(server->loop, SIGUSR2, on_vt_acquire, server);
    return 0;
}

static void
vt_switching_finish(struct tc_server* server)
{
    if (server->vt_fd < 0)
        return;

    struct vt_mode mode = { 0 };
    mode.mode = VT_AUTO;
    ioctl(server->vt_fd, VT_SETMODE, &mode);
    ioctl(server->vt_fd, KDSETMODE, KD_TEXT);
    close(server->vt_fd);
}

/* ======================================================================
 * main
 * ====================================================================== */

int
main(int argc, char** argv)
{
    const char* drm_device = argc > 1 ? argv[1] : "/dev/dri/card1";

    struct tc_server server = { 0 };
    server.vt_fd = -1;

    if (drm_output_init(&server.drm, drm_device) < 0)
        return 1;
    if (drm_output_create_framebuffer(&server.drm) < 0)
        return 1;
    /* drm_output_create_framebuffer() already called drmModeSetCrtc(),
     * which implicitly grants us DRM master if nobody else held it. */
    server.have_drm_master = 1;

    fprintf(stderr,
            "[tinycompositor] display mode: %dx%d\n",
            server.drm.width,
            server.drm.height);

    /* Put the (only) window near the top-left with a small margin. */
    server.win_x = 0;
    server.win_y = 0;

    server.display = wl_display_create();
    if (!server.display) {
        fprintf(stderr, "wl_display_create failed\n");
        return 1;
    }
    server.loop = wl_display_get_event_loop(server.display);

    /* wl_shm is implemented FOR us by libwayland-server: this one call
     * creates the wl_shm global and handles wl_shm_pool/wl_buffer/format
     * negotiation internally. That's why there's no shm code above. */
    if (wl_display_init_shm(server.display) < 0) {
        fprintf(stderr, "wl_display_init_shm failed\n");
        return 1;
    }

    server.compositor_global = wl_global_create(
      server.display, &wl_compositor_interface, 4, &server, bind_compositor);
    server.subcompositor_global = wl_global_create(server.display,
                                                   &wl_subcompositor_interface,
                                                   1,
                                                   &server,
                                                   bind_subcompositor);
    server.data_device_manager_global =
      wl_global_create(server.display,
                       &wl_data_device_manager_interface,
                       3,
                       &server,
                       bind_data_device_manager);
    server.xdg_wm_base_global = wl_global_create(
      server.display, &xdg_wm_base_interface, 1, &server, bind_wm_base);
    server.output_global = wl_global_create(
      server.display, &wl_output_interface, 3, &server, bind_output);
    server.seat_global = wl_global_create(
      server.display, &wl_seat_interface, 5, &server, bind_seat);

    vt_switching_init(&server);
    wl_event_loop_add_signal(server.loop, SIGINT, on_terminate_signal, &server);
    wl_event_loop_add_signal(
      server.loop, SIGTERM, on_terminate_signal, &server);

    const char* socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        fprintf(stderr, "wl_display_add_socket_auto failed\n");
        return 1;
    }
    fprintf(
      stderr, "[tinycompositor] listening on WAYLAND_DISPLAY=%s\n", socket);
    fprintf(stderr, "[tinycompositor] connect a client with:\n");
    fprintf(stderr, "    WAYLAND_DISPLAY=%s <your-wayland-app>\n", socket);

    /* Do NOT set WAYLAND_DISPLAY in our own environment -- that's for
     * client processes, and would be inherited by children we might
     * spawn. Nothing to do here; wl_display_run() below just services
     * the socket libwayland already created. */

    wl_display_run(server.display);

    vt_switching_finish(&server);
    wl_display_destroy(server.display);
    return 0;
}
