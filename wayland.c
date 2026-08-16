#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "foreign-toplevel-server-protocol.h"
#include "layer-shell-server-protocol.h"
#include "linux-dmabuf-server-protocol.h"
#include "log.h"
#include "opengl.h"
#include "pointer-constraints-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "red.h"
#include "relative-pointer-server-protocol.h"
#include "render.h"
#include "screencopy-server-protocol.h"
#include "time.h"
#include "viewporter-server-protocol.h"
#include "wayland.h"
#include "xdg-decoration-server-protocol.h"
#include "xdg-output-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include <GLES3/gl3.h>
#include <assert.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>
#include <wayland-util.h>

static int seq = 0;

int
red_send_pending_callbacks(struct redsurface* rsurf, uint32_t time_msec)
{
    if (!rsurf)
        return 0;

    while (rsurf->pending_pres_cbs.size > 0) {
        struct wl_resource* cb = dll_hpop(rsurf->pending_pres_cbs);
        wp_presentation_feedback_send_presented(
          cb,
          0,
          0,
          0,
          8333333,
          0,
          seq++,
          WP_PRESENTATION_FEEDBACK_KIND_VSYNC |
            WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK |
            WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION);
        wl_resource_destroy(cb);
    }

    while (rsurf->pending_frame_cbs.size > 0) {
        struct wl_resource* cb = dll_hpop(rsurf->pending_frame_cbs);
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
        ROG("sending back frame cb: %d", cb)
#endif
        wl_callback_send_done(cb, time_msec);
        wl_resource_destroy(cb);
    }

    return 0;
}

int
red_handle_send_callbacks(struct redsurface* rsurf, uint32_t time_msec)
{
    red_send_pending_callbacks(rsurf, time_msec);

    dll_for_each(rsurf->subsurfs, v)
      red_handle_send_callbacks(v->val, time_msec);
    return 0;
}

int
red_on_frame_done(struct redstate* rs, uint32_t time_msec)
{
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("frame done");
#endif

    {
        uint64_t now = time_get_now_msec();
        // clamping to 16ms because we don't page flip on every vblank
        rs->frame_latency   = min(now - rs->last_frame_time, 16);
        rs->last_frame_time = now;
    }

    // callbacks
    {
        if (rs->focused_rt)
            red_handle_send_callbacks(rs->focused_rt->rsurf, time_msec);

        dll_for_each(rs->layer_rsurfs, v)
          red_handle_send_callbacks(v->val, time_msec);
    }

    if (red_handle_animation_frame_done(rs)) {
        request_redraw(rs);
        return 0;
    }

    // if updates happened on page flip
    redraw(rs);
    return 0;
}

static void
wl_surface_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wl_surface_pending_buffer_resource_destroyed(struct wl_listener* listener,
                                             void*               data)
{
    struct redsurface* rsurf =
      wl_container_of(listener, rsurf, pending_buffer_destroyed);
    if (!rsurf)
        return;

    rsurf->pending_buffer = NULL;
}

static void
wl_surface_current_buffer_resource_destroyed(struct wl_listener* listener,
                                             void*               data)
{
    struct redsurface* rsurf =
      wl_container_of(listener, rsurf, current_buffer_destroyed);
    if (!rsurf)
        return;

    rsurf->current_buffer = NULL;
}

static void
wl_surface_attach(struct wl_client*   client,
                  struct wl_resource* resource,
                  struct wl_resource* buffer,
                  int32_t             x,
                  int32_t             y)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("attach: %d rsurf: %d", buffer, rsurf);
#endif

    wl_list_remove(&rsurf->pending_buffer_destroyed.link);
    rsurf->pending_buffer = buffer;
    rsurf->commited |= RED_SURF_COMMITED_BUFFER;

    if (buffer != NULL) {
        wl_resource_add_destroy_listener(buffer,
                                         &rsurf->pending_buffer_destroyed);
    } else {
        wl_list_init(&rsurf->pending_buffer_destroyed.link);
    }
}

static void
wl_surface_damage(struct wl_client*   client,
                  struct wl_resource* resource,
                  int32_t             x,
                  int32_t             y,
                  int32_t             width,
                  int32_t             height)
{
}

static void
wl_surface_frame(struct wl_client*   client,
                 struct wl_resource* resource,
                 uint32_t            callback)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);

    struct wl_resource* cb =
      wl_resource_create(client, &wl_callback_interface, 1, callback);
    assert(cb);
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("frame cb req: %d rsurf: %d", cb, rsurf);
#endif
    wl_resource_set_implementation(cb, NULL, NULL, NULL);

    dll_push_tail(rsurf->pending_frame_cbs, cb);
}

// for shm should happen after `glTexImage2D` call
// dma should be released after a new one is commited
int
red_current_buffer_release(struct redsurface* rsurf)
{
    assert(rsurf->current_buffer);
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("releasing buf: %d rsurf: %d", rsurf->current_buffer, rsurf);
#endif

    // if the attached pending buffer is the same as the one
    // we are currently holding, we do not send release
    if (rsurf->pending_buffer)
        if (rsurf->pending_buffer == rsurf->current_buffer)
            return 0;

    wl_buffer_send_release(rsurf->current_buffer);
    rsurf->current_buffer = NULL;
    return 0;
}

int
red_commit_handle_configure(struct redsurface* rsurf)
{
    if (rsurf->xdg_surface) {
        if (rsurf->xdg_toplevel) {
            red_send_toplevel_configure(rsurf, 0, 0);
        } else if (rsurf->xdg_popup) {
            red_send_popup_configure(rsurf);
        }
    } else if (rsurf->zwlr_layer_surface) {
        red_send_zwlr_layer_configure(rsurf);
    }

    return 0;
}

int
red_commit_handle_attach(struct redsurface* rsurf)
{
    wl_list_remove(&rsurf->current_buffer_destroyed.link);

    // TODO
    if (rsurf->pending_buffer == NULL)
        ROG("pending buffer NULL. handle!")

    // mostly this should be a dmabuf, that should be released now
    // also can be a shmbuf that we did not use to draw
    if (rsurf->current_buffer)
        red_current_buffer_release(rsurf);

    rsurf->current_buffer = rsurf->pending_buffer;
    rsurf->pending_buffer = NULL;

    if (rsurf->current_buffer != NULL) {
        wl_resource_add_destroy_listener(rsurf->current_buffer,
                                         &rsurf->current_buffer_destroyed);
    } else {
        wl_list_init(&rsurf->current_buffer_destroyed.link);
    }
    return 0;
}

static void
wl_surface_commit(struct wl_client* client, struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    struct redstate*   rs    = rsurf->rs;
    assert(rsurf);
    assert(rs);
    int rsurf_is_focused = red_is_rsurf_focused(rs, rsurf);

#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("commit rsurf: %d, conf: %d, client: %d",
        rsurf,
        rsurf->configured,
        rsurf->rc->wl_client);

    ROG("comm pending: %d, current: %d",
        rsurf->pending_buffer,
        rsurf->current_buffer)
#endif

    // handle attached buffer
    if (rsurf->commited & RED_SURF_COMMITED_BUFFER)
        red_commit_handle_attach(rsurf);

    rsurf->commited = 0;

    // on first commit, configure surface
    if ((rsurf->xdg_surface || rsurf->zwlr_layer_surface) &&
        !rsurf->configured) {
        red_commit_handle_configure(rsurf);
        return;
    }

    // NOTE: redsurfaces that are on background
    // do not need redraw or frame callback
    if (rsurf_is_focused)
        goto req_redraw;

    if (rsurf->zwlr_layer_surface)
        goto req_redraw;

    return;
req_redraw:
#ifdef RED_DEBUG_TRACK_SURFACE_BUFS
    ROG("req redraw")
#endif
    request_redraw(rsurf->rs);
}

static void
wl_surface_set_opaque_region(struct wl_client*   client,
                             struct wl_resource* resource,
                             struct wl_resource* region)
{
}

static void
wl_surface_set_input_region(struct wl_client*   client,
                            struct wl_resource* resource,
                            struct wl_resource* region)
{
}

static void
wl_surface_set_buffer_transform(struct wl_client*   client,
                                struct wl_resource* resource,
                                int32_t             transform)
{
}

static void
wl_surface_set_buffer_scale(struct wl_client*   client,
                            struct wl_resource* resource,
                            int32_t             scale)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);
    assert(scale > 0);

    int32_t prev_scale      = rsurf->buffer_scale;
    rsurf->buffer_scale     = scale;
    rsurf->buffer_scale_set = 1;

    // if scale in changed, and surface is focused toplevel, send configure
    if (red_is_rsurf_focused(rsurf->rs, rsurf))
        if (rsurf->buffer_scale != prev_scale) {
            if (rsurf)
                red_commit_handle_configure(rsurf);

            if (rsurf->rs->backend->is_ready_for_frame(rsurf->rs->backend->d))
                red_send_pending_callbacks(rsurf, time_get_now_msec());
        }
}

static void
wl_surface_damage_buffer(struct wl_client*   client,
                         struct wl_resource* resource,
                         int32_t             x,
                         int32_t             y,
                         int32_t             width,
                         int32_t             height)
{
}

static void
wl_surface_offset(struct wl_client*   client,
                  struct wl_resource* resource,
                  int32_t             x,
                  int32_t             y)
{
}

static void
wl_surface_get_release(struct wl_client*   client,
                       struct wl_resource* resource,
                       uint32_t            callback)
{
}

static const struct wl_surface_interface wl_surface_implementation = {
    .destroy              = wl_surface_destroy,
    .attach               = wl_surface_attach,
    .damage               = wl_surface_damage,
    .frame                = wl_surface_frame,
    .set_opaque_region    = wl_surface_set_opaque_region,
    .set_input_region     = wl_surface_set_input_region,
    .commit               = wl_surface_commit,
    .set_buffer_transform = wl_surface_set_buffer_transform,
    .set_buffer_scale     = wl_surface_set_buffer_scale,
    .damage_buffer        = wl_surface_damage_buffer,
    .offset               = wl_surface_offset,
    .get_release          = wl_surface_get_release,
};

static void
wl_surface_resource_destroy(struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);

#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("destroing rsurf: %d", rsurf);
#endif

    if (rsurf->rs->last_focused_rt &&
        rsurf->rs->last_focused_rt->rsurf == rsurf)
        rsurf->rs->last_focused_rt = NULL;

    if (rsurf->rs->keyboard_focused_rsurf == rsurf) {
        rsurf->rs->keyboard_focused_rsurf           = NULL;
        rsurf->rs->keyboard_focused_rsurf_exclusive = 0;
        if (rsurf->rs->focused_rt && rsurf->rs->focused_rt->rsurf &&
            rsurf->rs->focused_rt->rsurf != rsurf)
            red_keyboard_send_enter(rsurf->rs->focused_rt->rsurf);
    }

    if (rsurf->rs->pointer_focused_rsurf == rsurf)
        rsurf->rs->pointer_focused_rsurf = NULL;

    if (rsurf->current_buffer)
        red_current_buffer_release(rsurf);

    dll_remove_val(rsurf->rs->layer_rsurfs, rsurf);

    dll_for_each(rsurf->subsurfs, v)
    {
        v->val->parent = NULL;
    }

    // remove wl_surf from rsurfs of redclient
    if (red_is_client_valid(rsurf->rs, rsurf->rc))
        dll_remove_val(rsurf->rc->rsurfs, rsurf);

    wl_list_remove(&rsurf->pending_buffer_destroyed.link);
    wl_list_remove(&rsurf->current_buffer_destroyed.link);
    gl_destroy_surface_texture(rsurf);
    dll_destroy(rsurf->subsurfs);
    dll_destroy(rsurf->pending_frame_cbs);
    dll_destroy(rsurf->pending_pres_cbs);
    if (rsurf)
        free(rsurf);
    rsurf = NULL;
}

struct redsurface*
init_redsurface()
{
    struct redsurface* rsurf = NULL;
    rsurf                    = calloc(1, sizeof(*rsurf));
    if (!rsurf) {
        return NULL;
    }
    rsurf->rs                    = NULL;
    rsurf->rc                    = NULL;
    rsurf->x                     = 0;
    rsurf->y                     = 0;
    rsurf->w                     = 0;
    rsurf->h                     = 0;
    rsurf->configured            = 0;
    rsurf->pending_buffer        = NULL;
    rsurf->current_buffer        = NULL;
    rsurf->old_rendered_buf_type = 0;
    rsurf->commited              = 0;
    rsurf->pending_frame_cbs     = (typeof(rsurf->pending_frame_cbs))dll_init();
    rsurf->pending_pres_cbs      = (typeof(rsurf->pending_pres_cbs))dll_init();
    rsurf->parent                = NULL;
    rsurf->subsurfs              = (typeof(rsurf->subsurfs))dll_init();
    rsurf->wl_surface            = NULL;
    rsurf->xdg_toplevel          = NULL;
    rsurf->xdg_popup             = NULL;
    rsurf->geom_x                = 0;
    rsurf->geom_y                = 0;
    rsurf->geom_width            = 0;
    rsurf->geom_height           = 0;
    rsurf->geom_configured       = 0;
    rsurf->gl_tex                = 0;
    rsurf->buffer_scale          = 1;
    rsurf->buffer_scale_set      = 0;
    rsurf->zwlr_layer_surface    = NULL;
    rsurf->layer_anchor          = 0;
    rsurf->layer_margin_bottom   = 0;
    rsurf->layer_margin_top      = 0;
    rsurf->layer_margin_left     = 0;
    rsurf->layer_margin_right    = 0;
    rsurf->layer_width           = 0;
    rsurf->layer_height          = 0;
    rsurf->current_buffer_destroyed.notify =
      wl_surface_current_buffer_resource_destroyed;
    wl_list_init(&rsurf->current_buffer_destroyed.link);
    rsurf->pending_buffer_destroyed.notify =
      wl_surface_pending_buffer_resource_destroyed;
    wl_list_init(&rsurf->pending_buffer_destroyed.link);

    return rsurf;
}

static void
wl_compositor_create_surface(struct wl_client*   client,
                             struct wl_resource* resource,
                             uint32_t            id)
{
    struct redstate* rs = resource->data;
    assert(rs);

    struct redclient* rc = red_get_client(rs, client);
    assert(rc);

    struct redsurface* rsurf = init_redsurface();
    if (!rsurf) {
        ROG_ERR("oom?");
        return;
    }
    rsurf->rs         = rs;
    rsurf->rc         = rc;
    rsurf->wl_surface = wl_resource_create(
      client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!rsurf->wl_surface) {
        ROG_ERR("oom?");
        return;
    }
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating surface. client: %d, rsurf: %d", rc->wl_client, rsurf);
#endif

    wl_resource_set_implementation(rsurf->wl_surface,
                                   &wl_surface_implementation,
                                   rsurf,
                                   wl_surface_resource_destroy);

    if (wl_resource_get_version(resource) >= 6)
        wl_surface_send_preferred_buffer_scale(rsurf->wl_surface,
                                               cfg.screen_scale);

    dll_push_tail(rc->rsurfs, rsurf);
}

static void
wl_region_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
wl_region_add(struct wl_client*   c,
              struct wl_resource* r,
              int32_t             x,
              int32_t             y,
              int32_t             w,
              int32_t             h)
{
}

static void
wl_region_subtract(struct wl_client*   c,
                   struct wl_resource* r,
                   int32_t             x,
                   int32_t             y,
                   int32_t             w,
                   int32_t             h)
{
}

static const struct wl_region_interface wl_region_implementation = {
    .destroy  = wl_region_destroy,
    .add      = wl_region_add,
    .subtract = wl_region_subtract,
};

static void
wl_compositor_create_region(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            id)
{
    struct wl_resource* wl_region = wl_resource_create(
      client, &wl_region_interface, wl_resource_get_version(resource), id);
    assert(wl_region);
    wl_resource_set_implementation(
      wl_region, &wl_region_implementation, NULL, NULL);
}

static void
wl_compositor_release(struct wl_client* client, struct wl_resource* resource)
{
}

static const struct wl_compositor_interface wl_compositor_implementation = {
    .create_surface = wl_compositor_create_surface,
    .create_region  = wl_compositor_create_region,
    .release        = wl_compositor_release,
};

static void
wl_global_bind_compositor(struct wl_client* client,
                          void*             data,
                          uint32_t          version,
                          uint32_t          id)
{
    struct wl_resource* wl_compositor =
      wl_resource_create(client, &wl_compositor_interface, version, id);
    if (!wl_compositor) {
        ROG_ERR("oom?");
        return;
    }
    wl_resource_set_implementation(
      wl_compositor, &wl_compositor_implementation, data, NULL);
}

static void
xdg_toplevel_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_toplevel_set_parent(struct wl_client*   client,
                        struct wl_resource* resource,
                        struct wl_resource* parent)
{
}

static void
xdg_toplevel_set_title(struct wl_client*   client,
                       struct wl_resource* resource,
                       const char*         title)
{
    struct redtoplevel* rt = resource->data;
    assert(rt);

    if (rt->title)
        free(rt->title);

    rt->title = calloc(1, strlen(title) + 1);
    assert(rt->title);

    strcpy(rt->title, title);
}

static void
xdg_toplevel_set_app_id(struct wl_client*   client,
                        struct wl_resource* resource,
                        const char*         app_id)
{
    struct redtoplevel* rt = resource->data;
    assert(rt);

    if (rt->app_id)
        free(rt->app_id);

    rt->app_id = calloc(1, strlen(app_id) + 1);
    assert(rt->app_id);

    strcpy(rt->app_id, app_id);
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("app id: %s", rt->app_id);
#endif
}

static void
xdg_toplevel_show_window_menu(struct wl_client*   client,
                              struct wl_resource* resource,
                              struct wl_resource* seat,
                              uint32_t            serial,
                              int32_t             x,
                              int32_t             y)
{
}

static void
xdg_toplevel_move(struct wl_client*   client,
                  struct wl_resource* resource,
                  struct wl_resource* seat,
                  uint32_t            serial)
{
}

static void
xdg_toplevel_resize(struct wl_client*   client,
                    struct wl_resource* resource,
                    struct wl_resource* seat,
                    uint32_t            serial,
                    uint32_t            edges)
{
}

static void
xdg_toplevel_set_max_size(struct wl_client*   client,
                          struct wl_resource* resource,
                          int32_t             width,
                          int32_t             height)
{
}

static void
xdg_toplevel_set_min_size(struct wl_client*   client,
                          struct wl_resource* resource,
                          int32_t             width,
                          int32_t             height)
{
}

static void
xdg_toplevel_set_maximized(struct wl_client*   client,
                           struct wl_resource* resource)
{
    struct redtoplevel* rt = resource->data;
    red_send_toplevel_configure(rt->rsurf, 1, 0);
}

static void
xdg_toplevel_unset_maximized(struct wl_client*   client,
                             struct wl_resource* resource)
{
    struct redtoplevel* rt = resource->data;
    red_send_toplevel_configure(rt->rsurf, 1, 0);
}

static void
xdg_toplevel_set_fullscreen(struct wl_client*   client,
                            struct wl_resource* resource,
                            struct wl_resource* output)
{
    struct redtoplevel* rt = resource->data;
    red_send_toplevel_configure(rt->rsurf, 1, 0);
}

static void
xdg_toplevel_unset_fullscreen(struct wl_client*   client,
                              struct wl_resource* resource)
{
    struct redtoplevel* rt = resource->data;
    red_send_toplevel_configure(rt->rsurf, 1, 0);
}

static void
xdg_toplevel_set_minimized(struct wl_client*   client,
                           struct wl_resource* resource)
{
    struct redtoplevel* rt = resource->data;
    red_send_toplevel_configure(rt->rsurf, 1, 0);
}

static const struct xdg_toplevel_interface xdg_toplevel_implementation = {
    .destroy          = xdg_toplevel_destroy,
    .set_parent       = xdg_toplevel_set_parent,
    .set_title        = xdg_toplevel_set_title,
    .set_app_id       = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move             = xdg_toplevel_move,
    .resize           = xdg_toplevel_resize,
    .set_max_size     = xdg_toplevel_set_max_size,
    .set_min_size     = xdg_toplevel_set_min_size,
    .set_maximized    = xdg_toplevel_set_maximized,
    .unset_maximized  = xdg_toplevel_unset_maximized,
    .set_fullscreen   = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized    = xdg_toplevel_set_minimized
};

static void
xdg_toplevel_resource_destroy(struct wl_resource* resource)
{
    struct redtoplevel* rt = resource->data;
    assert(rt && rt->rs);
    red_destroy_rt(rt->rs, rt);
}

// by default using screen scale set by us
//
// if client sets scale with set_buffer_scale
// we use it.
uint32_t
red_get_scale(struct redsurface* rsurf)
{
    if (rsurf->buffer_scale_set)
        return rsurf->buffer_scale;
    return cfg.screen_scale;
}

int
red_send_toplevel_configure(struct redsurface* rsurf,
                            int                activated,
                            int                resizing)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG(
      "sendig tl configure. client: %d, rsurf: %d", rsurf->rc->wl_client, rsurf)
#endif
    // rsurf->configured = 0;

    uint32_t width  = rsurf->rs->backend->get_width(rsurf->rs->backend->d);
    uint32_t height = rsurf->rs->backend->get_height(rsurf->rs->backend->d);

    // making the surface cover the whole screen
    uint32_t scale = red_get_scale(rsurf);
    width /= scale;
    height /= scale;

    struct wl_array states;
    wl_array_init(&states);
    // setting maximized state as it basicly tells the client to not render csd
    // also if we have one toplevel drawn at a time,
    // it might as well be maximized
    if (activated) {
        uint32_t* s = wl_array_add(&states, sizeof(uint32_t));
        *s          = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    if (resizing) {
        uint32_t* s = wl_array_add(&states, sizeof(uint32_t));
        *s          = XDG_TOPLEVEL_STATE_RESIZING;
    }
    {
        uint32_t* s = wl_array_add(&states, sizeof(uint32_t));
        *s          = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    assert(rsurf->xdg_toplevel);
    if (wl_resource_get_version(rsurf->xdg_toplevel) > 4) {
        struct wl_array a;
        wl_array_init(&a);
        xdg_toplevel_send_wm_capabilities(rsurf->xdg_toplevel, &a);
        wl_array_release(&a);
    }

    if (wl_resource_get_version(rsurf->xdg_toplevel) > 3)
        xdg_toplevel_send_configure_bounds(rsurf->xdg_toplevel, width, height);
    xdg_toplevel_send_configure(rsurf->xdg_toplevel, width, height, &states);
    wl_array_release(&states);

    assert(rsurf->xdg_surface);
    assert(rsurf->rs && rsurf->rs->wl_display);
    uint32_t serial = wl_display_next_serial(rsurf->rs->wl_display);
    xdg_surface_send_configure(rsurf->xdg_surface, serial);

    return 0;
}

int
red_send_popup_configure(struct redsurface* rsurf)
{
    // NOTE: if we send this after we render, we will send scaled width and
    // height as render stores buffer dimentions in w, h
    xdg_popup_send_configure(rsurf->xdg_popup,
                             red_get_rsurf_x(rsurf),
                             red_get_rsurf_y(rsurf),
                             rsurf->w,
                             rsurf->h);
    uint32_t serial = wl_display_next_serial(rsurf->rs->wl_display);
    xdg_surface_send_configure(rsurf->xdg_surface, serial);
    return 0;
}

static void
xdg_popup_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_popup_grab(struct wl_client*   client,
               struct wl_resource* resource,
               struct wl_resource* seat,
               uint32_t            serial)
{
}
static void
xdg_popup_reposition(struct wl_client*   client,
                     struct wl_resource* resource,
                     struct wl_resource* positioner,
                     uint32_t            token)
{
}

static const struct xdg_popup_interface xdg_popup_implementation = {
    .destroy    = xdg_popup_destroy,
    .grab       = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void
xdg_popup_resource_destroy(struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);

    if (!rsurf->parent)
        return;

    dll_for_each(rsurf->parent->subsurfs, v)
    {
        if (rsurf == v->val) {
            dll_remove_val(rsurf->parent->subsurfs, rsurf);
            break;
        }
    }
    rsurf->xdg_popup = NULL;
    rsurf->parent    = NULL;
}

static void
xdg_surface_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_surface_get_toplevel(struct wl_client*   client,
                         struct wl_resource* resource,
                         uint32_t            id)
{
    struct redsurface* rsurf = resource->data;
    assert(rsurf);

    rsurf->xdg_toplevel = wl_resource_create(
      client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!rsurf->xdg_toplevel) {
        ROG_ERR("oom?");
        return;
    }

    struct redtoplevel* rt = red_create_rt(rsurf->rs, rsurf, client);
    assert(rt);

    wl_resource_set_implementation(rsurf->xdg_toplevel,
                                   &xdg_toplevel_implementation,
                                   rt,
                                   xdg_toplevel_resource_destroy);
}

static void
xdg_surface_get_popup(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            id,
                      struct wl_resource* parent,
                      struct wl_resource* positioner)
{
    struct redsurface*      rsurf    = wl_resource_get_user_data(resource);
    struct redsurface*      prsurf   = wl_resource_get_user_data(parent);
    struct positioner_data* pos_data = wl_resource_get_user_data(positioner);
    assert(rsurf && prsurf && pos_data);

    struct wl_resource* xdg_popup = wl_resource_create(
      client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    assert(xdg_popup);
    wl_resource_set_implementation(
      xdg_popup, &xdg_popup_implementation, rsurf, xdg_popup_resource_destroy);
    rsurf->xdg_popup = xdg_popup;

#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("popup: %d with pos: off_x: %d, off_y: %d, width: %d, height: %d, "
        "gravity: %d, anchor: %d, anchor_x: %d, anchor_y: %d, anchor_width: "
        "%d, anchor_height: %d",
        rsurf,
        pos_data->off_x,
        pos_data->off_y,
        pos_data->width,
        pos_data->height,
        pos_data->gravity,
        pos_data->anchor,
        pos_data->anchor_x,
        pos_data->anchor_y,
        pos_data->anchor_width,
        pos_data->anchor_height);
#endif

    int32_t x = prsurf->x;
    int32_t y = prsurf->y;

    x += pos_data->anchor_x;
    y += pos_data->anchor_y;

    switch (pos_data->anchor) {
        case XDG_POSITIONER_ANCHOR_TOP_LEFT:
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
            y += pos_data->anchor_height;
            break;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
            x += pos_data->anchor_width;
            break;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
            x += pos_data->anchor_width;
            y += pos_data->anchor_height;
            break;
        default:
            ROG_WARN("unhandled pos anchor!!");
            break;
    }
    switch (pos_data->gravity) {
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
            break;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
            x -= pos_data->width;
            break;
        default:
            ROG_WARN("unhandled pos gravity!!");
            break;
    }

    uint32_t scale = red_get_scale(rsurf);

    x += pos_data->off_x / (int32_t)scale;
    y += pos_data->off_y / (int32_t)scale;

    rsurf->x      = x;
    rsurf->y      = y;
    rsurf->w      = pos_data->width;
    rsurf->h      = pos_data->height;
    rsurf->parent = prsurf;
    dll_push_tail(prsurf->subsurfs, rsurf);
    red_send_popup_configure(rsurf);
}

static void
xdg_surface_set_window_geometry(struct wl_client*   client,
                                struct wl_resource* resource,
                                int32_t             x,
                                int32_t             y,
                                int32_t             width,
                                int32_t             height)
{
    struct redsurface* rsurf = resource->data;
    assert(rsurf);
    uint32_t scale         = red_get_scale(rsurf);
    rsurf->geom_configured = 1;
    rsurf->geom_width      = width * scale;
    rsurf->geom_height     = height * scale;
    rsurf->geom_x          = x * scale;
    rsurf->geom_y          = y * scale;
}

static void
xdg_surface_ack_configure(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            serial)
{
    struct redsurface* rsurf = resource->data;
    assert(rsurf);
    rsurf->configured = 1;
    if (red_is_rsurf_focused(rsurf->rs, rsurf))
        request_redraw(rsurf->rs);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy             = xdg_surface_destroy,
    .get_toplevel        = xdg_surface_get_toplevel,
    .get_popup           = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure       = xdg_surface_ack_configure,
};

static void
xdg_positioner_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
xdg_positioner_set_size(struct wl_client*   client,
                        struct wl_resource* resource,
                        int32_t             width,
                        int32_t             height)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    pos_data->width                  = width;
    pos_data->height                 = height;
}
static void
xdg_positioner_set_anchor_rect(struct wl_client*   client,
                               struct wl_resource* resource,
                               int32_t             x,
                               int32_t             y,
                               int32_t             width,
                               int32_t             height)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    pos_data->anchor_x               = x;
    pos_data->anchor_y               = y;
    pos_data->anchor_width           = width;
    pos_data->anchor_height          = height;
}
static void
xdg_positioner_set_anchor(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            anchor)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    pos_data->anchor                 = anchor;
}
static void
xdg_positioner_set_gravity(struct wl_client*   client,
                           struct wl_resource* resource,
                           uint32_t            gravity)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    pos_data->gravity                = gravity;
}
static void
xdg_positioner_set_offset(struct wl_client*   client,
                          struct wl_resource* resource,
                          int32_t             x,
                          int32_t             y)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    pos_data->off_x                  = x;
    pos_data->off_y                  = y;
}
static void
xdg_positioner_set_constraint_adjustment(struct wl_client*   client,
                                         struct wl_resource* resource,
                                         uint32_t constraint_adjustment)
{
}
static void
xdg_positioner_set_reactive(struct wl_client*   client,
                            struct wl_resource* resource)
{
}
static void
xdg_positioner_set_parent_size(struct wl_client*   client,
                               struct wl_resource* resource,
                               int32_t             parent_width,
                               int32_t             parent_height)
{
}
static void
xdg_positioner_set_parent_configure(struct wl_client*   client,
                                    struct wl_resource* resource,
                                    uint32_t            serial)
{
}

static const struct xdg_positioner_interface xdg_positioner_implementation = {
    .destroy                   = xdg_positioner_destroy,
    .set_size                  = xdg_positioner_set_size,
    .set_anchor_rect           = xdg_positioner_set_anchor_rect,
    .set_anchor                = xdg_positioner_set_anchor,
    .set_gravity               = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset                = xdg_positioner_set_offset,
    .set_reactive              = xdg_positioner_set_reactive,
    .set_parent_size           = xdg_positioner_set_parent_size,
    .set_parent_configure      = xdg_positioner_set_parent_configure,
};

static void
xdg_positioner_resource_destroy(struct wl_resource* resource)
{
    struct positioner_data* pos_data = wl_resource_get_user_data(resource);
    free(pos_data);
}

static void
xdg_wm_base_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_wm_base_create_positioner(struct wl_client*   client,
                              struct wl_resource* resource,
                              uint32_t            id)
{
    struct positioner_data* pos_data;
    pos_data                = calloc(1, sizeof(*pos_data));
    pos_data->off_x         = 0;
    pos_data->off_y         = 0;
    pos_data->width         = 0;
    pos_data->height        = 0;
    pos_data->gravity       = 0;
    pos_data->anchor        = 0;
    pos_data->anchor_x      = 0;
    pos_data->anchor_y      = 0;
    pos_data->anchor_width  = 0;
    pos_data->anchor_height = 0;

    struct wl_resource* xdg_positioner = wl_resource_create(
      client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    assert(xdg_positioner);
    wl_resource_set_implementation(xdg_positioner,
                                   &xdg_positioner_implementation,
                                   pos_data,
                                   xdg_positioner_resource_destroy);
}

static void
xdg_wm_base_get_xdg_surface(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            id,
                            struct wl_resource* surface)
{
    struct redsurface* rsurf = surface->data;
    assert(rsurf);

#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating XDG_surface. client: %d, rsurf: %d",
        rsurf->rc->wl_client,
        rsurf);
#endif

    rsurf->xdg_surface = wl_resource_create(
      client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    assert(rsurf->xdg_surface);
    wl_resource_set_implementation(
      rsurf->xdg_surface, &xdg_surface_implementation, rsurf, NULL);
}

static void
xdg_wm_base_pong(struct wl_client*   client,
                 struct wl_resource* resource,
                 uint32_t            serial)
{
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
    .destroy           = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface   = xdg_wm_base_get_xdg_surface,
    .pong              = xdg_wm_base_pong,
};

static void
wl_global_bind_xdg_wm_base(struct wl_client* client,
                           void*             data,
                           uint32_t          version,
                           uint32_t          id)
{
    struct wl_resource* xdg_wm_base =
      wl_resource_create(client, &xdg_wm_base_interface, version, id);
    if (!xdg_wm_base) {
        ROG_ERR("oom?");
        return;
    }
    wl_resource_set_implementation(
      xdg_wm_base, &xdg_wm_base_implementation, data, NULL);
}

static void
wl_output_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static const struct wl_output_interface wl_output_implementation = {
    .release = wl_output_release
};

static void
wl_global_bind_output(struct wl_client* client,
                      void*             data,
                      uint32_t          version,
                      uint32_t          id)
{
    struct redstate*    rs = data;
    struct wl_resource* wl_output =
      wl_resource_create(client, &wl_output_interface, version, id);
    assert(wl_output);
    wl_resource_set_implementation(
      wl_output, &wl_output_implementation, data, NULL);

    wl_output_send_geometry(wl_output,
                            0,
                            0,
                            600,
                            340,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Dell Inc.",
                            "DELL S2725QS",
                            WL_OUTPUT_TRANSFORM_NORMAL);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);

    wl_output_send_mode(wl_output,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        width,
                        height,
                        120 * 1000);
    if (version >= 2)
        wl_output_send_scale(wl_output, cfg.screen_scale);

    if (version >= 4) {
        wl_output_send_name(wl_output, "DP-1");
        wl_output_send_description(wl_output,
                                   "Dell Inc. - DELL S2725QS - DP-1");
    }
    if (version >= 2)
        wl_output_send_done(wl_output);
}

static void
wl_keyboard_release(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface wl_keyboard_implementation = {
    .release = wl_keyboard_release,
};

int
red_keyboard_send_enter(struct redsurface* rsurf)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("keybord focus on client: %d", rsurf->rc->wl_client)
#endif
    // we must send leave on the exclusive surf first
    if (rsurf->rs->keyboard_focused_rsurf_exclusive)
        return 0;

    if (rsurf->rs->keyboard_focused_rsurf)
        red_keyboard_send_leave(rsurf->rs->keyboard_focused_rsurf);

    rsurf->rs->keyboard_focused_rsurf = rsurf;

    if (!rsurf->rc->wl_keyboard)
        return 0;

    uint32_t        serial = wl_display_next_serial(rsurf->rs->wl_display);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(
      rsurf->rc->wl_keyboard, serial, rsurf->wl_surface, &keys);
    wl_array_release(&keys);

    dll_for_each(rsurf->rs->dds, v)
    {
        if (v->val->wl_client == rsurf->rc->wl_client) {
            red_data_device_offer_selection(v->val,
                                            rsurf->rs->selection_source);
            break;
        }
    }
    return 0;
}
int
red_keyboard_send_leave(struct redsurface* rsurf)
{
    if (rsurf->rs->keyboard_focused_rsurf == rsurf) {
        rsurf->rs->keyboard_focused_rsurf           = NULL;
        rsurf->rs->keyboard_focused_rsurf_exclusive = 0;
    } else if (rsurf->rs->keyboard_focused_rsurf_exclusive)
        return 0;

    if (!rsurf->rc->wl_keyboard)
        return 0;

    uint32_t serial = wl_display_next_serial(rsurf->rs->wl_display);
    wl_keyboard_send_leave(rsurf->rc->wl_keyboard, serial, rsurf->wl_surface);
    return 0;
}
int
red_keyboard_send_leave_and_find_new(struct redsurface* rsurf)
{
    red_keyboard_send_leave(rsurf);
    if (rsurf->rs->focused_rt && rsurf->rs->focused_rt->rsurf)
        red_keyboard_send_enter(rsurf->rs->focused_rt->rsurf);
    return 0;
}

static void
wl_pointer_set_cursor(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            serial,
                      struct wl_resource* surface,
                      int32_t             hotspot_x,
                      int32_t             hotspot_y)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    // as a client we don't do cursors
    if (rs->is_wayland_client)
        return;

    if (surface == NULL) {
        drm_hide_cursor(rs);
        rs->cursor_hidden = 1;
    }
    // currently not using surface to change cursor, just update.
    else {
        drm_update_cursor_plane(rs);
        rs->cursor_hidden = 0;
    }
}

static void
wl_pointer_release(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface wl_pointer_implementation = {
    .set_cursor = wl_pointer_set_cursor,
    .release    = wl_pointer_release,
};

int
red_pointer_send_enter(struct redclient* rc, struct wl_resource* wl_surface)
{
    uint32_t   serial = wl_display_next_serial(rc->rs->wl_display);
    wl_fixed_t x      = wl_fixed_from_double(red_get_lc_x(rc->rs));
    wl_fixed_t y      = wl_fixed_from_double(red_get_lc_y(rc->rs));
    wl_pointer_send_enter(rc->wl_pointer, serial, wl_surface, x, y);
    if (wl_resource_get_version(rc->wl_pointer) >=
        WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(rc->wl_pointer);
    return 0;
}

int
red_pointer_send_leave(struct redclient* rc, struct wl_resource* wl_surface)
{
    uint32_t serial = wl_display_next_serial(rc->rs->wl_display);
    wl_pointer_send_leave(rc->wl_pointer, serial, wl_surface);
    if (wl_resource_get_version(rc->wl_pointer) >=
        WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(rc->wl_pointer);
    return 0;
}

static void
wl_seat_get_pointer(struct wl_client*   client,
                    struct wl_resource* resource,
                    uint32_t            id)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("get poniter %d", client)
#endif

    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* wl_pointer = wl_resource_create(
      client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    assert(wl_pointer);
    wl_resource_set_implementation(
      wl_pointer, &wl_pointer_implementation, rs, NULL);

    dll_for_each(rs->rcs, v)
    {
        if (v->val->wl_client != client)
            continue;

        v->val->wl_pointer = wl_pointer;
        break;
    }
}

static void
wl_seat_get_keyboard(struct wl_client*   client,
                     struct wl_resource* resource,
                     uint32_t            id)
{
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("get keybord: %d", client)
#endif

    struct redstate*    rs          = wl_resource_get_user_data(resource);
    struct wl_resource* wl_keyboard = wl_resource_create(
      client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    assert(wl_keyboard);
    wl_resource_set_implementation(
      wl_keyboard, &wl_keyboard_implementation, wl_keyboard, NULL);

    assert(rs->xkb_keymap_fd >= 0);
    wl_keyboard_send_keymap(wl_keyboard,
                            WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            rs->xkb_keymap_fd,
                            rs->xkb_keymap_size);

    if (wl_resource_get_version(wl_keyboard) >=
        WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
        wl_keyboard_send_repeat_info(
          wl_keyboard, cfg.kb_repeat_rate, cfg.kb_repeat_delay);
    }

    dll_for_each(rs->rcs, v)
    {
        if (v->val->wl_client != client)
            continue;

        // TODO: multiple wl_keyboards?
        if (!v->val->wl_keyboard)
            v->val->wl_keyboard = wl_keyboard;
        break;
    }
}

static void
wl_seat_get_touch(struct wl_client*   client,
                  struct wl_resource* resource,
                  uint32_t            id)
{
    wl_resource_post_error(
      resource, WL_SEAT_ERROR_MISSING_CAPABILITY, "no touch capability");
}

static void
wl_seat_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static const struct wl_seat_interface wl_seat_implementation = {
    .get_pointer  = wl_seat_get_pointer,
    .get_keyboard = wl_seat_get_keyboard,
    .get_touch    = wl_seat_get_touch,
    .release      = wl_seat_release,
};

static void
wl_global_bind_seat(struct wl_client* client,
                    void*             data,
                    uint32_t          version,
                    uint32_t          id)
{
    struct wl_resource* wl_seat =
      wl_resource_create(client, &wl_seat_interface, version, id);
    assert(wl_seat);

    wl_resource_set_implementation(
      wl_seat, &wl_seat_implementation, data, NULL);

    wl_seat_send_capabilities(
      wl_seat, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);

    if (version >= 2)
        wl_seat_send_name(wl_seat, "seat0");
}

static void
wl_subsurface_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wl_subsurface_set_position(struct wl_client*   client,
                           struct wl_resource* resource,
                           int32_t             x,
                           int32_t             y)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    rsurf->x                 = x;
    rsurf->y                 = y;
}

static void
wl_subsurface_place_above(struct wl_client*   client,
                          struct wl_resource* resource,
                          struct wl_resource* sibling)
{
}
static void
wl_subsurface_place_below(struct wl_client*   client,
                          struct wl_resource* resource,
                          struct wl_resource* sibling)
{
}
static void
wl_subsurface_set_sync(struct wl_client* client, struct wl_resource* resource)
{
}
static void
wl_subsurface_set_desync(struct wl_client* client, struct wl_resource* resource)
{
}

static const struct wl_subsurface_interface wl_subsurface_implementation = {
    .destroy      = wl_subsurface_destroy,
    .set_position = wl_subsurface_set_position,
    .place_above  = wl_subsurface_place_above,
    .place_below  = wl_subsurface_place_below,
    .set_sync     = wl_subsurface_set_sync,
    .set_desync   = wl_subsurface_set_desync,
};

// TODO use after free!
static void
wl_subsurface_resource_destroy(struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);

    // parent destroyed nothing to do
    if (!rsurf->parent)
        return;

    dll_for_each(rsurf->parent->subsurfs, v)
    {
        if (rsurf == v->val) {
            dll_remove_val(rsurf->parent->subsurfs, rsurf);
            break;
        }
    }
}

static void
wl_subcompositor_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wl_subcompositor_get_subsurface(struct wl_client*   client,
                                struct wl_resource* resource,
                                uint32_t            id,
                                struct wl_resource* surface,
                                struct wl_resource* parent)
{
    struct wl_resource* wl_subsurface = wl_resource_create(
      client, &wl_subsurface_interface, wl_resource_get_version(resource), id);

    struct redsurface* rsurf  = wl_resource_get_user_data(surface);
    struct redsurface* prsurf = wl_resource_get_user_data(parent);

#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating SUBsurface. client: %d, rsurf: %d",
        rsurf->rc->wl_client,
        rsurf);
#endif

    wl_resource_set_implementation(wl_subsurface,
                                   &wl_subsurface_implementation,
                                   rsurf,
                                   wl_subsurface_resource_destroy);

    rsurf->parent = prsurf;
    dll_push_tail(prsurf->subsurfs, rsurf);
}

static const struct wl_subcompositor_interface
  wl_subcompositor_implementation = {
      .destroy        = wl_subcompositor_destroy,
      .get_subsurface = wl_subcompositor_get_subsurface,
  };

static void
wl_global_bind_wl_subcompositor(struct wl_client* client,
                                void*             data,
                                uint32_t          version,
                                uint32_t          id)
{
    struct wl_resource* wl_subcompositor =
      wl_resource_create(client, &wl_subcompositor_interface, version, id);
    wl_resource_set_implementation(
      wl_subcompositor, &wl_subcompositor_implementation, data, NULL);
}

static void
wl_data_source_offer(struct wl_client*   client,
                     struct wl_resource* resource,
                     const char*         mime_type)
{
    struct data_source* source = wl_resource_get_user_data(resource);
    if (!mime_type)
        return;

    char* dub_mime_type = strdup(mime_type);
    dll_push_tail(source->mime_types, dub_mime_type);
}

static void
wl_data_source_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wl_data_source_set_actions(struct wl_client*   client,
                           struct wl_resource* resource,
                           uint32_t            dnd_actions)
{
    struct data_source* source = wl_resource_get_user_data(resource);

    source->dnd_actions = dnd_actions;
    source->actions_set = true;
}

static const struct wl_data_source_interface wl_data_source_implementation = {
    .offer       = wl_data_source_offer,
    .destroy     = wl_data_source_destroy,
    .set_actions = wl_data_source_set_actions,
};

static void
wl_data_source_resource_destroy(struct wl_resource* resource)
{
    struct data_source* source = wl_resource_get_user_data(resource);
    assert(source);

    if (source->rs && source->rs->selection_source == source)
        source->rs->selection_source = NULL;

    dll_for_each(source->offers, v)
    {
        v->val->source = NULL;
    }
    dll_for_each(source->mime_types, v)
    {
        free(v->val);
    }

    dll_destroy(source->offers);
    dll_destroy(source->mime_types);
    free(source);
}

static void
wl_data_offer_accept(struct wl_client*   client,
                     struct wl_resource* resource,
                     uint32_t            serial,
                     const char*         mime_type)
{
    struct data_offer* offer = wl_resource_get_user_data(resource);
    if (offer->source)
        wl_data_source_send_target(offer->source->wl_data_source, mime_type);
}

static void
wl_data_offer_receive(struct wl_client*   client,
                      struct wl_resource* resource,
                      const char*         mime_type,
                      int32_t             fd)
{
    struct data_offer* offer = wl_resource_get_user_data(resource);
    if (offer->source) {
        wl_data_source_send_send(offer->source->wl_data_source, mime_type, fd);
    }
    close(fd);
}

static void
wl_data_offer_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wl_data_offer_finish(struct wl_client* client, struct wl_resource* resource)
{
    struct data_offer* offer = wl_resource_get_user_data(resource);

    if (offer->source)
        wl_data_source_send_dnd_finished(offer->source->wl_data_source);
}

static void
wl_data_offer_set_actions(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            dnd_actions,
                          uint32_t            preferred_action)
{
    struct data_offer* offer = wl_resource_get_user_data(resource);

    offer->actions          = dnd_actions;
    offer->preferred_action = preferred_action;

    if (!offer->source)
        return;

    uint32_t available = dnd_actions & offer->source->dnd_actions;
    uint32_t chosen    = 0;
    if (preferred_action & available)
        chosen = preferred_action;
    else if (available)
        chosen = available & ~(available - 1);

    if (wl_resource_get_version(resource) >= WL_DATA_OFFER_ACTION_SINCE_VERSION)
        wl_data_offer_send_action(resource, chosen);

    if (offer->source)
        if (wl_resource_get_version(offer->source->wl_data_source) >=
            WL_DATA_SOURCE_ACTION_SINCE_VERSION)
            wl_data_source_send_action(offer->source->wl_data_source, chosen);
}

static const struct wl_data_offer_interface wl_data_offer_implementation = {
    .accept      = wl_data_offer_accept,
    .receive     = wl_data_offer_receive,
    .destroy     = wl_data_offer_destroy,
    .finish      = wl_data_offer_finish,
    .set_actions = wl_data_offer_set_actions,
};

static void
wl_data_offer_resource_destroy(struct wl_resource* resource)
{
    struct data_offer* offer = wl_resource_get_user_data(resource);
    assert(offer);

    if (offer->source)
        dll_remove_val(offer->source->offers, offer);
    free(offer);
}

static struct wl_resource*
red_data_offer_create(struct wl_client*   client,
                      uint32_t            version,
                      struct data_source* source)
{
    struct data_offer* offer = calloc(1, sizeof(*offer));
    if (!offer)
        return NULL;

    offer->wl_data_offer =
      wl_resource_create(client, &wl_data_offer_interface, version, 0);
    if (!offer->wl_data_offer) {
        free(offer);
        return NULL;
    }

    offer->actions          = 0;
    offer->preferred_action = 0;
    offer->source           = source;
    dll_push_tail(source->offers, offer);

    wl_resource_set_implementation(offer->wl_data_offer,
                                   &wl_data_offer_implementation,
                                   offer,
                                   wl_data_offer_resource_destroy);

    return offer->wl_data_offer;
}

void
red_data_device_offer_selection(struct data_device* device,
                                struct data_source* source)
{
    if (!source) {
        wl_data_device_send_selection(device->wl_data_device, NULL);
        return;
    }

    struct wl_resource* offer_resource =
      red_data_offer_create(device->wl_client,
                            wl_resource_get_version(device->wl_data_device),
                            source);
    assert(offer_resource);

    wl_data_device_send_data_offer(device->wl_data_device, offer_resource);

    dll_for_each(source->mime_types, v)
      wl_data_offer_send_offer(offer_resource, v->val);

    wl_data_device_send_selection(device->wl_data_device, offer_resource);
}

void
red_data_device_send_selection_to_clients(struct redstate*  rs,
                                          struct wl_client* client)
{
    dll_for_each(rs->dds, v)
    {
        red_data_device_offer_selection(v->val, rs->selection_source);
    }
}

static void
wl_data_device_start_drag(struct wl_client*   client,
                          struct wl_resource* resource,
                          struct wl_resource* source,
                          struct wl_resource* origin,
                          struct wl_resource* icon,
                          uint32_t            serial)
{
}

static void
wl_data_device_set_selection(struct wl_client*   client,
                             struct wl_resource* resource,
                             struct wl_resource* source,
                             uint32_t            serial)
{
    struct data_device* device = wl_resource_get_user_data(resource);
    struct redstate*    rs     = device->rs;
    struct data_source* new_source =
      source ? wl_resource_get_user_data(source) : NULL;

    if (rs->selection_source && rs->selection_source != new_source) {
        wl_data_source_send_cancelled(rs->selection_source->wl_data_source);
    }

    if (new_source)
        new_source->rs = rs;
    rs->selection_source = new_source;

    if (rs->keyboard_focused_rsurf)
        red_data_device_send_selection_to_clients(
          rs, rs->keyboard_focused_rsurf->rc->wl_client);
}

static void
wl_data_device_release(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_data_device_interface wl_data_device_implementation = {
    .start_drag    = wl_data_device_start_drag,
    .set_selection = wl_data_device_set_selection,
    .release       = wl_data_device_release,
};

static void
wl_data_device_resource_destroy(struct wl_resource* resource)
{
    struct data_device* dd = wl_resource_get_user_data(resource);
    assert(dd);

    dll_remove_val(dd->rs->dds, dd);
    free(dd);
}

static void
wl_data_device_manager_create_data_source(struct wl_client*   client,
                                          struct wl_resource* resource,
                                          uint32_t            id)
{

    struct data_source* source;
    source = calloc(1, sizeof(*source));
    assert(source);

    source->mime_types     = (typeof(source->mime_types))dll_init();
    source->offers         = (typeof(source->offers))dll_init();
    source->dnd_actions    = 0;
    source->actions_set    = 0;
    source->wl_data_source = wl_resource_create(
      client, &wl_data_source_interface, wl_resource_get_version(resource), id);
    assert(source->wl_data_source);

    wl_resource_set_implementation(source->wl_data_source,
                                   &wl_data_source_implementation,
                                   source,
                                   wl_data_source_resource_destroy);
}

static void
wl_data_device_manager_get_data_device(struct wl_client*   client,
                                       struct wl_resource* resource,
                                       uint32_t            id,
                                       struct wl_resource* seat)
{
    struct redstate* rs = wl_resource_get_user_data(seat);

    struct data_device* dd;
    dd = calloc(1, sizeof(*dd));
    assert(dd);

    struct wl_resource* wl_data_device = wl_resource_create(
      client, &wl_data_device_interface, wl_resource_get_version(resource), id);
    assert(wl_data_device);

    dd->rs             = rs;
    dd->wl_data_device = wl_data_device;
    dd->wl_client      = client;
    dll_push_tail(rs->dds, dd);

    wl_resource_set_implementation(wl_data_device,
                                   &wl_data_device_implementation,
                                   dd,
                                   wl_data_device_resource_destroy);
}

static void
wl_data_device_manager_release(struct wl_client*   client,
                               struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_data_device_manager_interface
  wl_data_device_manager_implementation = {
      .create_data_source = wl_data_device_manager_create_data_source,
      .get_data_device    = wl_data_device_manager_get_data_device,
      .release            = wl_data_device_manager_release,
  };

static void
wl_global_bind_wl_data_device_manager(struct wl_client* client,
                                      void*             data,
                                      uint32_t          version,
                                      uint32_t          id)
{
    struct wl_resource* wl_data_device_manager = wl_resource_create(
      client, &wl_data_device_manager_interface, version, id);
    assert(wl_data_device_manager);
    wl_resource_set_implementation(wl_data_device_manager,
                                   &wl_data_device_manager_implementation,
                                   data,
                                   NULL);
}

static void
xdg_toplevel_decoration_destroy(struct wl_client*   client,
                                struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
xdg_toplevel_decoration_set_mode(struct wl_client*   client,
                                 struct wl_resource* resource,
                                 uint32_t            mode)
{
}
static void
xdg_toplevel_decoration_unset_mode(struct wl_client*   client,
                                   struct wl_resource* resource)
{
}

static const struct zxdg_toplevel_decoration_v1_interface
  zxdg_toplevel_decoration_v1_implementation = {
      .destroy    = xdg_toplevel_decoration_destroy,
      .set_mode   = xdg_toplevel_decoration_set_mode,
      .unset_mode = xdg_toplevel_decoration_unset_mode,
  };

static void
xdg_decoration_manager_destroy(struct wl_client*   client,
                               struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_decoration_manager_get_toplevel_decoration(struct wl_client*   client,
                                               struct wl_resource* resource,
                                               uint32_t            id,
                                               struct wl_resource* toplevel)
{
    struct wl_resource* xdg_toplevel_decoration =
      wl_resource_create(client,
                         &zxdg_toplevel_decoration_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    assert(xdg_toplevel_decoration);

    wl_resource_set_implementation(xdg_toplevel_decoration,
                                   &zxdg_toplevel_decoration_v1_implementation,
                                   wl_resource_get_user_data(toplevel),
                                   NULL);

    // we only want server side
    zxdg_toplevel_decoration_v1_send_configure(
      xdg_toplevel_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface
  zxdg_decoration_manager_v1_implementation = {
      .destroy                 = xdg_decoration_manager_destroy,
      .get_toplevel_decoration = xdg_decoration_manager_get_toplevel_decoration,
  };

static void
wl_global_bind_xdg_decoration_manager(struct wl_client* client,
                                      void*             data,
                                      uint32_t          version,
                                      uint32_t          id)
{
    struct wl_resource* xdg_decoration_manager = wl_resource_create(
      client, &zxdg_decoration_manager_v1_interface, version, id);
    assert(xdg_decoration_manager);
    wl_resource_set_implementation(xdg_decoration_manager,
                                   &zxdg_decoration_manager_v1_implementation,
                                   data,
                                   NULL);
}

static void
wl_client_destroyed(struct wl_listener* listener, void* data)
{
    struct redclient* rc = wl_container_of(listener, rc, client_destroyed);
    assert(rc);
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("destroing wl_client %d", rc->wl_client);
#endif

    dll_remove_val(rc->rs->rcs, rc);

    dll_destroy(rc->rsurfs);
    if (rc)
        free(rc);
}

static void
wl_client_created(struct wl_listener* listener, void* data)
{
    struct redstate* rs = wl_container_of(listener, rs, client_created);

    struct wl_client* wl_client = data;
    assert(wl_client);
#ifdef RED_DEBUG_TRACK_CLIENT_CREATION
    ROG("creating wl_client %d", wl_client);
#endif

    struct redclient* rc;
    rc = calloc(1, sizeof(*rc));
    assert(rc);
    rc->rs                      = rs;
    rc->rsurfs                  = (typeof(rc->rsurfs))dll_init();
    rc->wl_keyboard             = NULL;
    rc->wl_pointer              = NULL;
    rc->wl_client               = wl_client;
    rc->client_destroyed.notify = wl_client_destroyed;

    wl_client_add_destroy_listener(wl_client, &rc->client_destroyed);

    dll_push_tail(rs->rcs, rc);
}

void
wl_buffer_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface wl_buffer_implementation = {
    .destroy = wl_buffer_destroy,
};

static void
zwp_linux_buffer_resource_destroy(struct wl_resource* resource)
{
    struct dmabuf* dmabuf = wl_resource_get_user_data(resource);

    if (dmabuf->egl_img)
        gl_destroy_egl_img(
          dmabuf->rs->backend->get_egl_display(dmabuf->rs->backend->d),
          dmabuf->egl_img);

    for (int i = 0; i < dmabuf->planes_count; i++)
        close(dmabuf->planes[i].fd);

    if (dmabuf)
        free(dmabuf);
}

struct dmabuf*
red_get_dmabuf(struct wl_resource* resource)
{
    if (!resource)
        return NULL;

    if (!wl_resource_instance_of(
          resource, &wl_buffer_interface, &wl_buffer_implementation))
        return NULL;
    return wl_resource_get_user_data(resource);
}

static void
zwp_linux_buffer_params_destroy(struct wl_client*   client,
                                struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
zwp_linux_buffer_params_add(struct wl_client*   client,
                            struct wl_resource* resource,
                            int32_t             fd,
                            uint32_t            plane_idx,
                            uint32_t            offset,
                            uint32_t            stride,
                            uint32_t            modifier_hi,
                            uint32_t            modifier_lo)
{
    struct dmabuf_params* params = wl_resource_get_user_data(resource);
    assert(plane_idx < 4);

    params->planes[plane_idx].fd          = fd;
    params->planes[plane_idx].offset      = offset;
    params->planes[plane_idx].stride      = stride;
    params->planes[plane_idx].modifier_hi = modifier_hi;
    params->planes[plane_idx].modifier_lo = modifier_lo;
    params->planes_count++;
}

static struct wl_resource*
init_linux_buffer(struct wl_client*   client,
                  struct wl_resource* resource,
                  int32_t             width,
                  int32_t             height,
                  uint32_t            format,
                  uint32_t            flags,
                  int                 immed,
                  uint32_t            buffer_id)
{
    struct dmabuf_params* params = wl_resource_get_user_data(resource);

    struct wl_resource* wl_buffer = wl_buffer =
      wl_resource_create(client, &wl_buffer_interface, 1, buffer_id);
    if (!wl_buffer) {
        ROG_ERR("oom?");
        return NULL;
    }

    struct dmabuf* dmabuf;
    dmabuf = calloc(1, sizeof(*dmabuf));
    assert(dmabuf);
    dmabuf->egl_img = NULL;
    dmabuf->flags   = flags;
    dmabuf->height  = height;
    dmabuf->width   = width;
    dmabuf->format  = format;
    dmabuf->rs      = params->rs;
    for (int i = 0; i < params->planes_count; i++) {
        dmabuf->planes[i] = params->planes[i];
        dmabuf->planes_count++;
    }
    wl_resource_set_implementation(wl_buffer,
                                   &wl_buffer_implementation,
                                   dmabuf,
                                   zwp_linux_buffer_resource_destroy);

    if (!immed)
        zwp_linux_buffer_params_v1_send_created(resource, wl_buffer);

    return wl_buffer;
}

static void
zwp_linux_buffer_params_create(struct wl_client*   client,
                               struct wl_resource* resource,
                               int32_t             width,
                               int32_t             height,
                               uint32_t            format,
                               uint32_t            flags)
{
    init_linux_buffer(client, resource, width, height, format, flags, 0, 0);
}

static void
zwp_linux_buffer_params_create_immed(struct wl_client*   client,
                                     struct wl_resource* resource,
                                     uint32_t            buffer_id,
                                     int32_t             width,
                                     int32_t             height,
                                     uint32_t            format,
                                     uint32_t            flags)
{
    init_linux_buffer(
      client, resource, width, height, format, flags, 1, buffer_id);
}
static void
zwp_linux_buffer_params_set_sampling_device(struct wl_client*   client,
                                            struct wl_resource* resource,
                                            struct wl_array*    device)
{
}

static void
zwp_linux_buffer_params_resource_destroy(struct wl_resource* resource)
{
    struct dmabuf_params* params = wl_resource_get_user_data(resource);
    if (params)
        free(params);
}

static const struct zwp_linux_buffer_params_v1_interface
  zwp_linux_buffer_params_implementation = {
      .destroy             = zwp_linux_buffer_params_destroy,
      .add                 = zwp_linux_buffer_params_add,
      .create              = zwp_linux_buffer_params_create,
      .create_immed        = zwp_linux_buffer_params_create_immed,
      .set_sampling_device = zwp_linux_buffer_params_set_sampling_device,
  };

static void
zwp_linux_dmabuf_feedback_destroy(struct wl_client*   client,
                                  struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface
  zwp_linux_dmabuf_feedback_implementation = {
      .destroy = zwp_linux_dmabuf_feedback_destroy,
  };

static void
zwp_linux_dmabuf_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

struct dmabuf_params*
init_dmabuf_params()
{
    struct dmabuf_params* params;
    params = calloc(1, sizeof(*params));
    assert(params);
    params->rs = NULL;

    return params;
}

static void
zwp_linux_dmabuf_create_params(struct wl_client*   client,
                               struct wl_resource* resource,
                               uint32_t            id)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zwp_linux_dmabuf_params =
      wl_resource_create(client,
                         &zwp_linux_buffer_params_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    if (!zwp_linux_dmabuf_params) {
        ROG_ERR("oom?");
        return;
    }

    struct dmabuf_params* params = init_dmabuf_params();
    params->rs                   = rs;
    wl_resource_set_implementation(zwp_linux_dmabuf_params,
                                   &zwp_linux_buffer_params_implementation,
                                   params,
                                   zwp_linux_buffer_params_resource_destroy);
}

static void
zwp_linux_dmabuf_get_default_feedback(struct wl_client*   client,
                                      struct wl_resource* resource,
                                      uint32_t            id)
{
    struct redstate*    rs = wl_resource_get_user_data(resource);
    struct wl_resource* zwp_linux_dmabuf_feedback =
      wl_resource_create(client,
                         &zwp_linux_dmabuf_feedback_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    if (!zwp_linux_dmabuf_feedback) {
        ROG_ERR("oom?");
        return;
    }
    wl_resource_set_implementation(zwp_linux_dmabuf_feedback,
                                   &zwp_linux_dmabuf_feedback_implementation,
                                   rs,
                                   NULL);

    int    fd;
    int    entry_count = 2;
    size_t size        = 16 * entry_count;
    {
        char name[64];
        snprintf(name, sizeof(name), "/wl_red_dmabuf_feedback-%d", getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (fd < 0) {
            ROG_ERR(
              "failed creating shm for zwp_linux_dmabuf_get_default_feedback");
            return;
        }

        if (ftruncate(fd, size) == -1) {
            ROG_ERR("xkb keymap fd shm setting size failed");
            close(fd);
            return;
        }
        void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            close(fd);
            return;
        }

        struct entry
        {
            uint32_t format;
            uint32_t padding;
            uint64_t modifier;
        }* entry;
        entry             = calloc(entry_count, sizeof(*entry));
        entry[0].format   = DRM_FORMAT_XRGB8888;
        entry[0].padding  = 0;
        entry[0].modifier = 0;
        entry[1].format   = DRM_FORMAT_ARGB8888;
        entry[1].padding  = 0;
        entry[1].modifier = 0;

        memcpy(ptr, entry, size);
        free(entry);
    }

    zwp_linux_dmabuf_feedback_v1_send_format_table(
      zwp_linux_dmabuf_feedback, fd, size);
    close(fd);

    int         drm_fd  = rs->backend->get_drm_node(rs->backend->d);
    int         pass_fd = -1;
    struct stat st;
    if (fstat(drm_fd, &st) == 0)
        pass_fd = st.st_rdev;
    else {
        ROG_ERR("fstat on drm_fd failed");
        return;
    }

    struct wl_array dev_arr;
    wl_array_init(&dev_arr);
    dev_t* devp = wl_array_add(&dev_arr, sizeof(dev_t));
    *devp       = pass_fd;

    zwp_linux_dmabuf_feedback_v1_send_main_device(zwp_linux_dmabuf_feedback,
                                                  &dev_arr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(
      zwp_linux_dmabuf_feedback, &dev_arr);
    wl_array_release(&dev_arr);

    struct wl_array indices_arr;
    wl_array_init(&indices_arr);
    for (int i = 0; i < entry_count; i++) {
        uint16_t* idxp = wl_array_add(&indices_arr, sizeof(uint16_t));
        *idxp          = (uint16_t)i;
    }
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(zwp_linux_dmabuf_feedback,
                                                      &indices_arr);
    wl_array_release(&indices_arr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(zwp_linux_dmabuf_feedback,
                                                    0);

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(zwp_linux_dmabuf_feedback);
    zwp_linux_dmabuf_feedback_v1_send_done(zwp_linux_dmabuf_feedback);
}

static void
zwp_linux_dmabuf_get_surface_feedback(struct wl_client*   client,
                                      struct wl_resource* resource,
                                      uint32_t            id,
                                      struct wl_resource* surface)
{
    zwp_linux_dmabuf_get_default_feedback(client, resource, id);
}

static const struct zwp_linux_dmabuf_v1_interface
  zwp_linux_dmabuf_implementation = {
      .destroy              = zwp_linux_dmabuf_destroy,
      .create_params        = zwp_linux_dmabuf_create_params,
      .get_default_feedback = zwp_linux_dmabuf_get_default_feedback,
      .get_surface_feedback = zwp_linux_dmabuf_get_surface_feedback,
  };

static void
wl_global_bind_zwp_linux_dmabuf(struct wl_client* client,
                                void*             data,
                                uint32_t          version,
                                uint32_t          id)
{
    struct wl_resource* zwp_linux_dmabuf =
      wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!zwp_linux_dmabuf) {
        ROG_ERR("oom?");
        return;
    }
    wl_resource_set_implementation(
      zwp_linux_dmabuf, &zwp_linux_dmabuf_implementation, data, NULL);

    if (version < 4) {
        ROG_ERR("zwp_linux_dmabuf version < 4 used, implement!!");
    }
}

static void
wp_viewport_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
wp_viewport_set_source(struct wl_client*   client,
                       struct wl_resource* resource,
                       wl_fixed_t          x,
                       wl_fixed_t          y,
                       wl_fixed_t          width,
                       wl_fixed_t          height)
{
    // ROG("src: %d, %d (%dx%d)",
    //     wl_fixed_to_int(x),
    //     wl_fixed_to_int(y),
    //     wl_fixed_to_int(width),
    //     wl_fixed_to_int(height));
}
static void
wp_viewport_set_destination(struct wl_client*   client,
                            struct wl_resource* resource,
                            int32_t             width,
                            int32_t             height)
{
    // struct redstate* rs      = wl_resource_get_user_data(resource);
    // uint32_t         _width  = rs->backend->get_width(rs->backend->d);
    // uint32_t         _height = rs->backend->get_height(rs->backend->d);
    // ROG("dst: %dx%d when %dx%d", width, height, _width, _height);
}

static const struct wp_viewport_interface wp_viewport_implementation = {
    .destroy         = wp_viewport_destroy,
    .set_source      = wp_viewport_set_source,
    .set_destination = wp_viewport_set_destination,
};

static void
wp_viewporter_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wp_viewporter_get_viewport(struct wl_client*   client,
                           struct wl_resource* resource,
                           uint32_t            id,
                           struct wl_resource* surface)
{
    struct wl_resource* wp_viewport = wl_resource_create(
      client, &wp_viewport_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(wp_viewport,
                                   &wp_viewport_implementation,
                                   wl_resource_get_user_data(resource),
                                   NULL);
}

static const struct wp_viewporter_interface wp_viewporter_implementation = {
    .destroy      = wp_viewporter_destroy,
    .get_viewport = wp_viewporter_get_viewport,
};

static void
wl_global_bind_wp_viewporter(struct wl_client* client,
                             void*             data,
                             uint32_t          version,
                             uint32_t          id)
{
    struct wl_resource* wp_viewporter =
      wl_resource_create(client, &wp_viewporter_interface, version, id);
    wl_resource_set_implementation(
      wp_viewporter, &wp_viewporter_implementation, data, NULL);
}

void
zwp_relative_pointer_destroy(struct wl_client*   client,
                             struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static const struct zwp_relative_pointer_v1_interface
  zwp_relative_pointer_implementation = {
      .destroy = zwp_relative_pointer_destroy,
  };

static void
zwp_relative_pointer_resource_destroy(struct wl_resource* resource)
{
    struct redstate* rs = wl_resource_get_user_data(resource);
    assert(rs);

    dll_remove_val(rs->rel_pointers, resource);
}

static void
zwp_relative_pointer_manager_destroy(struct wl_client*   client,
                                     struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwp_relative_pointer_manager_get_relative_pointer(struct wl_client*   client,
                                                  struct wl_resource* resource,
                                                  uint32_t            id,
                                                  struct wl_resource* pointer)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zwp_relative_pointer =
      wl_resource_create(client,
                         &zwp_relative_pointer_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    wl_resource_set_implementation(zwp_relative_pointer,
                                   &zwp_relative_pointer_implementation,
                                   wl_resource_get_user_data(resource),
                                   zwp_relative_pointer_resource_destroy);

    dll_push_tail(rs->rel_pointers, zwp_relative_pointer);
}

static const struct zwp_relative_pointer_manager_v1_interface
  zwp_relative_pointer_manager_implementation = {
      .destroy              = zwp_relative_pointer_manager_destroy,
      .get_relative_pointer = zwp_relative_pointer_manager_get_relative_pointer,
  };

static void
wl_global_bind_zwp_relative_pointer(struct wl_client* client,
                                    void*             data,
                                    uint32_t          version,
                                    uint32_t          id)
{
    struct wl_resource* zwp_relative_pointer_manager = wl_resource_create(
      client, &zwp_relative_pointer_manager_v1_interface, version, id);
    wl_resource_set_implementation(zwp_relative_pointer_manager,
                                   &zwp_relative_pointer_manager_implementation,
                                   data,
                                   NULL);
}
static void
zwp_confined_pointer_destroy(struct wl_client*   client,
                             struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwp_confined_pointer_set_region(struct wl_client*   client,
                                struct wl_resource* resource,
                                struct wl_resource* region)
{
}
static const struct zwp_confined_pointer_v1_interface
  zwp_confined_pointer_implementation = {
      .destroy    = zwp_confined_pointer_destroy,
      .set_region = zwp_confined_pointer_set_region,
  };

static void
zwp_locked_pointer_destroy(struct wl_client*   client,
                           struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwp_locked_pointer_set_cursor_position_hint(struct wl_client*   client,
                                            struct wl_resource* resource,
                                            wl_fixed_t          surface_x,
                                            wl_fixed_t          surface_y)
{
}
static void
zwp_locked_pointer_set_region(struct wl_client*   client,
                              struct wl_resource* resource,
                              struct wl_resource* region)
{
}
static const struct zwp_locked_pointer_v1_interface
  zwp_locked_pointer_implementation = {
      .destroy                  = zwp_locked_pointer_destroy,
      .set_cursor_position_hint = zwp_locked_pointer_set_cursor_position_hint,
      .set_region               = zwp_locked_pointer_set_region,
  };

static void
zwp_locked_pointer_resource_destroy(struct wl_resource* resource)
{
    struct redstate* rs = wl_resource_get_user_data(resource);
    rs->cursor_locked   = 0;
}

static void
zwp_pointer_constraints_destroy(struct wl_client*   client,
                                struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwp_pointer_constraints_lock_pointer(struct wl_client*   client,
                                     struct wl_resource* resource,
                                     uint32_t            id,
                                     struct wl_resource* surface,
                                     struct wl_resource* pointer,
                                     struct wl_resource* region,
                                     uint32_t            lifetime)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zwp_locked_pointer =
      wl_resource_create(client,
                         &zwp_locked_pointer_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    wl_resource_set_implementation(zwp_locked_pointer,
                                   &zwp_locked_pointer_implementation,
                                   wl_resource_get_user_data(resource),
                                   zwp_locked_pointer_resource_destroy);

    rs->cursor_locked = 1;
    zwp_locked_pointer_v1_send_locked(zwp_locked_pointer);
}
static void
zwp_pointer_constraints_confine_pointer(struct wl_client*   client,
                                        struct wl_resource* resource,
                                        uint32_t            id,
                                        struct wl_resource* surface,
                                        struct wl_resource* pointer,
                                        struct wl_resource* region,
                                        uint32_t            lifetime)
{
    struct wl_resource* zwp_confined_pointer =
      wl_resource_create(client,
                         &zwp_confined_pointer_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    wl_resource_set_implementation(zwp_confined_pointer,
                                   &zwp_confined_pointer_implementation,
                                   wl_resource_get_user_data(resource),
                                   NULL);
    // TODO
}

static const struct zwp_pointer_constraints_v1_interface
  zwp_pointer_constraints_implementation = {
      .destroy         = zwp_pointer_constraints_destroy,
      .lock_pointer    = zwp_pointer_constraints_lock_pointer,
      .confine_pointer = zwp_pointer_constraints_confine_pointer,
  };

static void
wl_global_bind_zwp_pointer_constraints(struct wl_client* client,
                                       void*             data,
                                       uint32_t          version,
                                       uint32_t          id)
{
    struct wl_resource* zwp_pointer_constraints = wl_resource_create(
      client, &zwp_pointer_constraints_v1_interface, version, id);
    wl_resource_set_implementation(zwp_pointer_constraints,
                                   &zwp_pointer_constraints_implementation,
                                   data,
                                   NULL);
}

static void
wp_presentation_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
wp_presentation_feedback(struct wl_client*   client,
                         struct wl_resource* resource,
                         struct wl_resource* surface,
                         uint32_t            callback)
{
    struct redstate* rs = wl_resource_get_user_data(resource);
    assert(rs);

    struct wl_resource* pres_cb =
      wl_resource_create(client,
                         &wp_presentation_feedback_interface,
                         wl_resource_get_version(resource),
                         callback);
    assert(pres_cb);
    wl_resource_set_implementation(pres_cb, NULL, NULL, NULL);

    struct redsurface* rsurf = red_get_rsurf_by_wl_surf(rs, surface);
    dll_push_tail(rsurf->pending_pres_cbs, pres_cb);
}

static const struct wp_presentation_interface wp_presentation_implementation = {
    .destroy  = wp_presentation_destroy,
    .feedback = wp_presentation_feedback,
};

static void
wl_global_bind_wp_presentation(struct wl_client* client,
                               void*             data,
                               uint32_t          version,
                               uint32_t          id)
{
    struct wl_resource* wp_presentation =
      wl_resource_create(client, &wp_presentation_interface, version, id);
    wl_resource_set_implementation(
      wp_presentation, &wp_presentation_implementation, data, NULL);
}

static void
zwlr_layer_surface_set_size(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            width,
                            uint32_t            height)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    rsurf->layer_width       = width;
    rsurf->layer_height      = height;
    if (rsurf->configured)
        red_send_zwlr_layer_configure(rsurf);
}
static void
zwlr_layer_surface_set_anchor(struct wl_client*   client,
                              struct wl_resource* resource,
                              uint32_t            anchor)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    rsurf->layer_anchor      = anchor;
    if (rsurf->configured)
        red_send_zwlr_layer_configure(rsurf);
}
static void
zwlr_layer_surface_set_exclusive_zone(struct wl_client*   client,
                                      struct wl_resource* resource,
                                      int32_t             zone)
{
}
static void
zwlr_layer_surface_set_margin(struct wl_client*   client,
                              struct wl_resource* resource,
                              int32_t             top,
                              int32_t             right,
                              int32_t             bottom,
                              int32_t             left)
{
    struct redsurface* rsurf   = wl_resource_get_user_data(resource);
    rsurf->layer_margin_bottom = bottom;
    rsurf->layer_margin_top    = top;
    rsurf->layer_margin_left   = left;
    rsurf->layer_margin_right  = right;
    if (rsurf->configured)
        red_send_zwlr_layer_configure(rsurf);
}
static void
zwlr_layer_surface_set_keyboard_interactivity(struct wl_client*   client,
                                              struct wl_resource* resource,
                                              uint32_t keyboard_interactivity)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    switch (keyboard_interactivity) {
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE:
            if (rsurf->rs->keyboard_focused_rsurf == rsurf) {
                rsurf->rs->keyboard_focused_rsurf_exclusive = 0;
                red_keyboard_send_leave_and_find_new(rsurf);
            }
            break;
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE:
            red_keyboard_send_enter(rsurf);
            rsurf->rs->keyboard_focused_rsurf_exclusive = 1;
            break;
        case ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND:
            ROG_WARN("on demand keyboard interactivity not implemented!");
            break;
    }
}
static void
zwlr_layer_surface_get_popup(struct wl_client*   client,
                             struct wl_resource* resource,
                             struct wl_resource* popup)
{
}
static void
zwlr_layer_surface_ack_configure(struct wl_client*   client,
                                 struct wl_resource* resource,
                                 uint32_t            serial)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    rsurf->configured        = 1;
    request_redraw(rsurf->rs);
}
static void
zwlr_layer_surface_destroy(struct wl_client*   client,
                           struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwlr_layer_surface_set_layer(struct wl_client*   client,
                             struct wl_resource* resource,
                             uint32_t            layer)
{
}

static const struct zwlr_layer_surface_v1_interface
  zwlr_layer_surface_implementation = {
      .set_size           = zwlr_layer_surface_set_size,
      .set_anchor         = zwlr_layer_surface_set_anchor,
      .set_exclusive_zone = zwlr_layer_surface_set_exclusive_zone,
      .set_margin         = zwlr_layer_surface_set_margin,
      .set_keyboard_interactivity =
        zwlr_layer_surface_set_keyboard_interactivity,
      .get_popup     = zwlr_layer_surface_get_popup,
      .ack_configure = zwlr_layer_surface_ack_configure,
      .destroy       = zwlr_layer_surface_destroy,
      .set_layer     = zwlr_layer_surface_set_layer,
  };

// TODO store layer data not in redsurface
static void
zwlr_layer_surface_resource_destroy(struct wl_resource* resource)
{
    // struct redsurface* rsurf = wl_resource_get_user_data(resource);
    // assert(rsurf && rsurf->rs);
    // dll_for_each(rsurf->rs->layer_rsurfs, v)
    // {
    //     if (rsurf == v->val) {
    //         dll_remove(rsurf->rs->layer_rsurfs, v);
    //         break;
    //     }
    // }
    // rsurf->zwlr_layer_surface  = NULL;
    // rsurf->layer_anchor        = 0;
    // rsurf->layer_margin_bottom = 0;
    // rsurf->layer_margin_top    = 0;
    // rsurf->layer_margin_left   = 0;
    // rsurf->layer_margin_right  = 0;
}

#define RA_TOP    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
#define RA_BOTTOM ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
#define RA_LEFT   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
#define RA_RIGHT  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
int
red_send_zwlr_layer_configure(struct redsurface* rsurf)
{
    uint32_t serial = wl_display_next_serial(rsurf->rs->wl_display);

    uint32_t scale = red_get_scale(rsurf);
    uint32_t screen_width =
      rsurf->rs->backend->get_width(rsurf->rs->backend->d);
    uint32_t screen_height =
      rsurf->rs->backend->get_height(rsurf->rs->backend->d);
    screen_width /= scale;
    screen_height /= scale;

    int32_t x = 0;
    int32_t y = 0;
    int32_t w = rsurf->layer_width;
    int32_t h = rsurf->layer_height;

    switch (rsurf->layer_anchor) {
        case 0:
            x = screen_width / 2 - rsurf->layer_width / 2;
            y = screen_height / 2 - rsurf->layer_height / 2;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_TOP:
            x = screen_width / 2 - rsurf->layer_width / 2;
            y = 0;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_BOTTOM:
            x = screen_width / 2 - rsurf->layer_width / 2;
            y = screen_height - rsurf->layer_height;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_LEFT:
            x = 0;
            y = screen_height / 2 - rsurf->layer_height / 2;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_RIGHT:
            x = screen_width - rsurf->layer_width;
            y = screen_height / 2 - rsurf->layer_height / 2;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_RIGHT | RA_LEFT:
            x = 0;
            y = screen_height / 2 - rsurf->layer_height / 2;
            w = screen_width;
            h = rsurf->layer_height;
            break;
        case RA_TOP | RA_BOTTOM:
            x = screen_width / 2 - rsurf->layer_width / 2;
            y = 0;
            w = rsurf->layer_width;
            h = screen_height;
            break;
        case RA_TOP | RA_LEFT:
            x = 0;
            y = 0;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_TOP | RA_RIGHT:
            x = screen_width - rsurf->layer_width;
            y = 0;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_BOTTOM | RA_LEFT:
            x = 0;
            y = screen_height - rsurf->layer_height;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_BOTTOM | RA_RIGHT:
            x = screen_width - rsurf->layer_width;
            y = screen_height - rsurf->layer_height;
            w = rsurf->layer_width;
            h = rsurf->layer_height;
            break;
        case RA_TOP | RA_LEFT | RA_BOTTOM:
            x = 0;
            y = 0;
            w = rsurf->layer_width;
            h = screen_height;
            break;
        case RA_TOP | RA_RIGHT | RA_BOTTOM:
            x = screen_width - rsurf->layer_width;
            y = 0;
            w = rsurf->layer_width;
            h = screen_height;
            break;
        case RA_LEFT | RA_TOP | RA_RIGHT:
            x = 0;
            y = 0;
            w = screen_width;
            h = rsurf->layer_height;
            break;
        case RA_LEFT | RA_BOTTOM | RA_RIGHT:
            x = 0;
            y = screen_height - rsurf->layer_height;
            w = screen_width;
            h = rsurf->layer_height;
            break;
        case RA_LEFT | RA_BOTTOM | RA_RIGHT | RA_TOP:
            x = 0;
            y = 0;
            w = screen_width;
            h = screen_height;
            break;
        default:
            ROG_WARN("unhandled layer anchor");
            break;
    }

    if (rsurf->layer_anchor & RA_TOP)
        y += rsurf->layer_margin_top;
    if (rsurf->layer_anchor & RA_BOTTOM)
        y = max(y - rsurf->layer_margin_bottom, 0);
    if (rsurf->layer_anchor & RA_LEFT)
        x += rsurf->layer_margin_left;
    if (rsurf->layer_anchor & RA_RIGHT)
        x = max(x - rsurf->layer_margin_right, 0);

    if ((rsurf->layer_anchor & RA_BOTTOM) && (rsurf->layer_anchor & RA_TOP)) {
        y = rsurf->layer_margin_top;
        h = max(h - rsurf->layer_margin_top, 0);
        h = max(h - rsurf->layer_margin_bottom, 0);
    }

    if ((rsurf->layer_anchor & RA_LEFT) && (rsurf->layer_anchor & RA_RIGHT)) {
        x = rsurf->layer_margin_left;
        w = max(w - rsurf->layer_margin_left, 0);
        w = max(w - rsurf->layer_margin_right, 0);
    }

    rsurf->x = x;
    rsurf->y = y;
    rsurf->w = w;
    rsurf->h = h;
    zwlr_layer_surface_v1_send_configure(
      rsurf->zwlr_layer_surface, serial, rsurf->w, rsurf->h);

    return 0;
}

static void
zwlr_layer_shell_get_layer_surface(struct wl_client*   client,
                                   struct wl_resource* resource,
                                   uint32_t            id,
                                   struct wl_resource* surface,
                                   struct wl_resource* output,
                                   uint32_t            layer,
                                   const char* namespace)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zwlr_layer_surface =
      wl_resource_create(client,
                         &zwlr_layer_surface_v1_interface,
                         wl_resource_get_version(resource),
                         id);
    assert(zwlr_layer_surface);

    struct redsurface* rsurf = red_get_rsurf_by_wl_surf(rs, surface);
    assert(rsurf);
    rsurf->zwlr_layer_surface = zwlr_layer_surface;

    wl_resource_set_implementation(zwlr_layer_surface,
                                   &zwlr_layer_surface_implementation,
                                   rsurf,
                                   zwlr_layer_surface_resource_destroy);

    dll_push_tail(rs->layer_rsurfs, rsurf);
}

static void
zwlr_layer_shell_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct zwlr_layer_shell_v1_interface
  zwlr_layer_shell_implementation = {
      .get_layer_surface = zwlr_layer_shell_get_layer_surface,
      .destroy           = zwlr_layer_shell_destroy,
  };

static void
wl_global_bind_zwlr_layer_shell(struct wl_client* client,
                                void*             data,
                                uint32_t          version,
                                uint32_t          id)
{

    struct wl_resource* zwlr_layer_shell =
      wl_resource_create(client, &zwlr_layer_shell_v1_interface, version, id);
    assert(zwlr_layer_shell);

    wl_resource_set_implementation(
      zwlr_layer_shell, &zwlr_layer_shell_implementation, data, NULL);
}

static void
wl_shell_surface_pong(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            serial)
{
}
static void
wl_shell_surface_move(struct wl_client*   client,
                      struct wl_resource* resource,
                      struct wl_resource* seat,
                      uint32_t            serial)
{
}
static void
wl_shell_surface_resize(struct wl_client*   client,
                        struct wl_resource* resource,
                        struct wl_resource* seat,
                        uint32_t            serial,
                        uint32_t            edges)
{
}
static void
wl_shell_surface_set_toplevel(struct wl_client*   client,
                              struct wl_resource* resource)
{
}
static void
wl_shell_surface_set_transient(struct wl_client*   client,
                               struct wl_resource* resource,
                               struct wl_resource* parent,
                               int32_t             x,
                               int32_t             y,
                               uint32_t            flags)
{
}
static void
wl_shell_surface_set_fullscreen(struct wl_client*   client,
                                struct wl_resource* resource,
                                uint32_t            method,
                                uint32_t            framerate,
                                struct wl_resource* output)
{
}
static void
wl_shell_surface_set_popup(struct wl_client*   client,
                           struct wl_resource* resource,
                           struct wl_resource* seat,
                           uint32_t            serial,
                           struct wl_resource* parent,
                           int32_t             x,
                           int32_t             y,
                           uint32_t            flags)
{
}
static void
wl_shell_surface_set_maximized(struct wl_client*   client,
                               struct wl_resource* resource,
                               struct wl_resource* output)
{
}
static void
wl_shell_surface_set_title(struct wl_client*   client,
                           struct wl_resource* resource,
                           const char*         title)
{
}
static void
wl_shell_surface_set_class(struct wl_client*   client,
                           struct wl_resource* resource,
                           const char*         class_)
{
}

static const struct wl_shell_surface_interface
  wl_shell_surface_implementation = {
      .pong           = wl_shell_surface_pong,
      .move           = wl_shell_surface_move,
      .resize         = wl_shell_surface_resize,
      .set_toplevel   = wl_shell_surface_set_toplevel,
      .set_transient  = wl_shell_surface_set_transient,
      .set_fullscreen = wl_shell_surface_set_fullscreen,
      .set_popup      = wl_shell_surface_set_popup,
      .set_maximized  = wl_shell_surface_set_maximized,
      .set_title      = wl_shell_surface_set_title,
      .set_class      = wl_shell_surface_set_class,
  };

static void
wl_shell_get_shell_surface(struct wl_client*   client,
                           struct wl_resource* resource,
                           uint32_t            id,
                           struct wl_resource* surface)
{
    struct wl_resource* wl_shell_surf =
      wl_resource_create(client,
                         &wl_shell_surface_interface,
                         wl_resource_get_version(resource),
                         id);
    wl_resource_set_implementation(wl_shell_surf,
                                   &wl_shell_surface_implementation,
                                   wl_resource_get_user_data(resource),
                                   NULL);
}

static const struct wl_shell_interface wl_shell_implementation = {
    .get_shell_surface = wl_shell_get_shell_surface,
};

static __attribute__((unused)) void
wl_global_bind_wl_shell(struct wl_client* client,
                        void*             data,
                        uint32_t          version,
                        uint32_t          id)
{
    struct wl_resource* wl_shell =
      wl_resource_create(client, &wl_shell_interface, version, id);
    assert(wl_shell);
    wl_resource_set_implementation(
      wl_shell, &wl_shell_implementation, data, NULL);
}

static void
zwlr_foreign_toplevel_manager_stop(struct wl_client*   client,
                                   struct wl_resource* resource)
{
}
static const struct zwlr_foreign_toplevel_manager_v1_interface
  zwlr_foreign_toplevel_manager_implementation = {
      .stop = zwlr_foreign_toplevel_manager_stop,
  };

static void
wl_global_bind_zwlr_foreign_toplevel_manager(struct wl_client* client,
                                             void*             data,
                                             uint32_t          version,
                                             uint32_t          id)
{

    struct wl_resource* zwlr_foreign_toplevel_manager = wl_resource_create(
      client, &zwlr_foreign_toplevel_manager_v1_interface, version, id);
    assert(zwlr_foreign_toplevel_manager);

    wl_resource_set_implementation(
      zwlr_foreign_toplevel_manager,
      &zwlr_foreign_toplevel_manager_implementation,
      data,
      NULL);
}
static void
zxdg_output_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct zxdg_output_v1_interface zxdg_output_implementation = {
    .destroy = zxdg_output_destroy,
};

static void
zxdg_output_manager_destroy(struct wl_client*   client,
                            struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zxdg_output_manager_get_xdg_output(struct wl_client*   client,
                                   struct wl_resource* resource,
                                   uint32_t            id,
                                   struct wl_resource* output)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zxdg_output = wl_resource_create(
      client, &zxdg_output_v1_interface, wl_resource_get_version(resource), id);
    assert(zxdg_output);
    wl_resource_set_implementation(
      zxdg_output, &zxdg_output_implementation, NULL, NULL);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);

    zxdg_output_v1_send_name(zxdg_output, "DP-1");
    zxdg_output_v1_send_description(zxdg_output,
                                    "Dell Inc. - DELL S2725QS - DP-1");
    zxdg_output_v1_send_logical_size(
      zxdg_output, width / cfg.screen_scale, height / cfg.screen_scale);
    zxdg_output_v1_send_logical_position(zxdg_output, 0, 0);

    if (wl_resource_get_version(resource) < 3)
        zxdg_output_v1_send_done(zxdg_output);
    else
        wl_output_send_done(output);
}

static const struct zxdg_output_manager_v1_interface
  zxdg_output_manager_implementation = {
      .destroy        = zxdg_output_manager_destroy,
      .get_xdg_output = zxdg_output_manager_get_xdg_output,
  };

static void
wl_global_bind_zxdg_output_manager(struct wl_client* client,
                                   void*             data,
                                   uint32_t          version,
                                   uint32_t          id)
{

    struct wl_resource* zxdg_output_manager = wl_resource_create(
      client, &zxdg_output_manager_v1_interface, version, id);
    assert(zxdg_output_manager);
    wl_resource_set_implementation(
      zxdg_output_manager, &zxdg_output_manager_implementation, data, NULL);
}

// TODO use fence
static void
zwlr_screencopy_frame_copy(struct wl_client*   client,
                           struct wl_resource* resource,
                           struct wl_resource* buffer)
{
    struct redstate*  rs = wl_resource_get_user_data(resource);
    struct redbuffer* rb = rs->backend->get_current_buffer(rs->backend->d);

    struct wl_shm_buffer* shmbuf = NULL;
    struct dmabuf*        dmabuf = NULL;
    if ((shmbuf = wl_shm_buffer_get(buffer))) {
        int32_t  width  = wl_shm_buffer_get_width(shmbuf);
        int32_t  height = wl_shm_buffer_get_height(shmbuf);
        uint8_t* dst    = wl_shm_buffer_get_data(shmbuf);
        wl_shm_buffer_begin_access(shmbuf);

        CALL(glBindFramebuffer(GL_FRAMEBUFFER, rb->fbo));

        GLuint tex = 0;
        CALL(glGenTextures(1, &tex));
        CALL(glBindTexture(GL_TEXTURE_2D, tex));
        CALL(
          gl_proc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, rb->egl_image));

        CALL(glReadPixels(
          0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, dst));

        CALL(glBindTexture(GL_TEXTURE_2D, 0));
        CALL(glDeleteTextures(1, &tex));
        CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        CALL(glFinish());
        wl_shm_buffer_end_access(shmbuf);
        goto ready;
    } else if ((dmabuf = red_get_dmabuf(buffer))) {
        if (!dmabuf->egl_img)
            if (!(dmabuf->egl_img =
                    init_egl_image(rs->backend->get_egl_display(rs->backend->d),
                                   dmabuf->width,
                                   dmabuf->height,
                                   dmabuf->format,
                                   dmabuf->planes_count,
                                   dmabuf->planes)))
                goto fail;

        uint32_t src_width  = rs->backend->get_width(rs->backend->d);
        uint32_t src_height = rs->backend->get_height(rs->backend->d);

        GLuint fbo = 0;
        GLuint rbo = 0;
        if (gl_add_fb(dmabuf->egl_img, &fbo, &rbo))
            goto fail;

        CALL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));

        GLuint tex = 0;
        CALL(glGenTextures(1, &tex));
        CALL(glBindTexture(GL_TEXTURE_2D, tex));
        CALL(
          gl_proc->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, rb->egl_image));

        GLuint rfbo = 0;
        CALL(glGenFramebuffers(1, &rfbo));
        CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, rfbo));
        CALL(glFramebufferTexture2D(
          GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0));

        CALL(glBlitFramebuffer(0,
                               0,
                               src_width,
                               src_height,
                               0,
                               0,
                               dmabuf->width,
                               dmabuf->height,
                               GL_COLOR_BUFFER_BIT,
                               GL_LINEAR));

        CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, 0));
        CALL(glDeleteFramebuffers(1, &rfbo));
        CALL(glBindTexture(GL_TEXTURE_2D, 0));
        CALL(glDeleteTextures(1, &tex));
        CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        CALL(glDeleteFramebuffers(1, &fbo));
        CALL(glDeleteRenderbuffers(1, &rbo));
        CALL(glFinish());
        goto ready;
    }

    ROG_ERR("screencopy non dma or shm buffer?");

ready:
    zwlr_screencopy_frame_v1_send_flags(resource, 0);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    zwlr_screencopy_frame_v1_send_ready(
      resource,
      (uint32_t)((uint64_t)now.tv_sec >> 32),
      (uint32_t)((uint64_t)now.tv_sec & 0xffffffff),
      (uint32_t)now.tv_nsec);
    return;
fail:
    ROG_ERR("screencopy failed");
    return;
}
static void
zwlr_screencopy_frame_destroy(struct wl_client*   client,
                              struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}
static void
zwlr_screencopy_frame_copy_with_damage(struct wl_client*   client,
                                       struct wl_resource* resource,
                                       struct wl_resource* buffer)
{
    zwlr_screencopy_frame_copy(client, resource, buffer);
}

static const struct zwlr_screencopy_frame_v1_interface
  zwlr_screencopy_frame_v1_implementation = {
      .copy             = zwlr_screencopy_frame_copy,
      .destroy          = zwlr_screencopy_frame_destroy,
      .copy_with_damage = zwlr_screencopy_frame_copy_with_damage,
  };

static void
zwlr_screencopy_capture_output(struct wl_client*   client,
                               struct wl_resource* resource,
                               uint32_t            frame,
                               int32_t             overlay_cursor,
                               struct wl_resource* output)
{
    struct redstate* rs = wl_resource_get_user_data(resource);

    struct wl_resource* zwlr_screencopy_frame =
      wl_resource_create(client,
                         &zwlr_screencopy_frame_v1_interface,
                         wl_resource_get_version(resource),
                         frame);
    assert(zwlr_screencopy_frame);
    wl_resource_set_implementation(zwlr_screencopy_frame,
                                   &zwlr_screencopy_frame_v1_implementation,
                                   rs,
                                   NULL);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);

    zwlr_screencopy_frame_v1_send_buffer(
      zwlr_screencopy_frame, WL_SHM_FORMAT_ARGB8888, width, height, width * 4);

    if (wl_resource_get_version(resource) >= 3) {
        zwlr_screencopy_frame_v1_send_linux_dmabuf(
          zwlr_screencopy_frame, DRM_FORMAT_ARGB8888, width, height);

        zwlr_screencopy_frame_v1_send_buffer_done(zwlr_screencopy_frame);
    }
}

static void
zwlr_screencopy_capture_output_region(struct wl_client*   client,
                                      struct wl_resource* resource,
                                      uint32_t            frame,
                                      int32_t             overlay_cursor,
                                      struct wl_resource* output,
                                      int32_t             x,
                                      int32_t             y,
                                      int32_t             width,
                                      int32_t             height)
{
    zwlr_screencopy_capture_output(
      client, resource, frame, overlay_cursor, output);
}

static void
zwlr_screencopy_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct zwlr_screencopy_manager_v1_interface
  zwlr_screencopy_manager_v1_implementation = {
      .capture_output        = zwlr_screencopy_capture_output,
      .capture_output_region = zwlr_screencopy_capture_output_region,
      .destroy               = zwlr_screencopy_destroy,
  };

static void
wl_global_bind_zwlr_screencopy(struct wl_client* client,
                               void*             data,
                               uint32_t          version,
                               uint32_t          id)
{
    struct wl_resource* zwlr_screencopy_manager = wl_resource_create(
      client, &zwlr_screencopy_manager_v1_interface, version, id);
    assert(zwlr_screencopy_manager);

    wl_resource_set_implementation(zwlr_screencopy_manager,
                                   &zwlr_screencopy_manager_v1_implementation,
                                   data,
                                   NULL);
}

void
handle_wl_log(const char* _fmt, va_list args)
{
    // remove newline at end
    int   l   = strlen(_fmt);
    char* fmt = malloc(l + 1);
    assert(fmt);
    strcpy(fmt, _fmt);
    fmt[l - 1] = '\0';

    ROG_ERR_VARGS(fmt, args);
    free(fmt);
}

int
init_compositor(struct redstate* rs)
{
    wl_log_set_handler_server(handle_wl_log);
    rs->wl_display = wl_display_create();
    if (!rs->wl_display) {
        ROG_ERR("could not create wl_display");
        goto fail;
    }

    const char* wayland_display = wl_display_add_socket_auto(rs->wl_display);
    if (!wayland_display) {
        ROG_ERR("could not create wl_display socket");
        goto fail;
    }
    rs->wayland_display = wayland_display;
    ROG_INFO("Listening on WAYLAND_DISPLAY=%s", rs->wayland_display);
    setenv("WAYLAND_DISPLAY", rs->wayland_display, 1);

    rs->wl_event_loop = wl_display_get_event_loop(rs->wl_display);
    if (!rs->wl_event_loop) {
        ROG_ERR("could not create wl_event_loop");
        goto fail;
    }

    if (wl_display_init_shm(rs->wl_display) < 0) {
        ROG_ERR("wl_display_init_shm failed\n");
        goto fail;
    }

    rs->wl_compositor = wl_global_create(rs->wl_display,
                                         &wl_compositor_interface,
                                         7,
                                         rs,
                                         wl_global_bind_compositor);
    if (!rs->wl_compositor) {
        ROG_ERR("could not create wl_compositor");
        goto fail;
    }

    rs->xdg_wm_base = wl_global_create(rs->wl_display,
                                       &xdg_wm_base_interface,
                                       7,
                                       rs,
                                       wl_global_bind_xdg_wm_base);
    if (!rs->xdg_wm_base) {
        ROG_ERR("could not create xdg_wm_base");
        goto fail;
    }

    rs->wl_output = wl_global_create(
      rs->wl_display, &wl_output_interface, 4, rs, wl_global_bind_output);
    if (!rs->wl_output) {
        ROG_ERR("could not create wl_output");
        goto fail;
    }

    rs->wl_seat = wl_global_create(
      rs->wl_display, &wl_seat_interface, 9, rs, wl_global_bind_seat);
    if (!rs->wl_seat) {
        ROG_ERR("could not create wl_output");
        goto fail;
    }

    rs->wl_subcompositor = wl_global_create(rs->wl_display,
                                            &wl_subcompositor_interface,
                                            1,
                                            rs,
                                            wl_global_bind_wl_subcompositor);
    assert(rs->wl_subcompositor);

    rs->wl_data_device_manager =
      wl_global_create(rs->wl_display,
                       &wl_data_device_manager_interface,
                       4,
                       rs,
                       wl_global_bind_wl_data_device_manager);
    assert(rs->wl_data_device_manager);

    rs->xdg_decoration_manager =
      wl_global_create(rs->wl_display,
                       &zxdg_decoration_manager_v1_interface,
                       1,
                       rs,
                       wl_global_bind_xdg_decoration_manager);
    assert(rs->xdg_decoration_manager);

    rs->zwp_linux_dmabuf = wl_global_create(rs->wl_display,
                                            &zwp_linux_dmabuf_v1_interface,
                                            5,
                                            rs,
                                            wl_global_bind_zwp_linux_dmabuf),
    assert(rs->zwp_linux_dmabuf);

    rs->wp_viewporter = wl_global_create(rs->wl_display,
                                         &wp_viewporter_interface,
                                         1,
                                         rs,
                                         wl_global_bind_wp_viewporter);
    assert(rs->wp_viewporter);

    rs->zwp_relative_pointer_manager =
      wl_global_create(rs->wl_display,
                       &zwp_relative_pointer_manager_v1_interface,
                       1,
                       rs,
                       wl_global_bind_zwp_relative_pointer);
    assert(rs->zwp_relative_pointer_manager);

    rs->zwp_pointer_constraints =
      wl_global_create(rs->wl_display,
                       &zwp_pointer_constraints_v1_interface,
                       1,
                       rs,
                       wl_global_bind_zwp_pointer_constraints);
    assert(rs->zwp_pointer_constraints);

    rs->wp_presentation = wl_global_create(rs->wl_display,
                                           &wp_presentation_interface,
                                           2,
                                           rs,
                                           wl_global_bind_wp_presentation);
    assert(rs->wp_presentation);

    rs->zwlr_layer_shell = wl_global_create(rs->wl_display,
                                            &zwlr_layer_shell_v1_interface,
                                            3,
                                            rs,
                                            wl_global_bind_zwlr_layer_shell);
    assert(rs->zwlr_layer_shell);

    rs->zwlr_screencopy =
      wl_global_create(rs->wl_display,
                       &zwlr_screencopy_manager_v1_interface,
                       3,
                       rs,
                       wl_global_bind_zwlr_screencopy);
    assert(rs->zwlr_screencopy);

    rs->zwlr_foreign_toplevel_manager =
      wl_global_create(rs->wl_display,
                       &zwlr_foreign_toplevel_manager_v1_interface,
                       3,
                       rs,
                       wl_global_bind_zwlr_foreign_toplevel_manager);
    assert(rs->zwlr_foreign_toplevel_manager);

    rs->zxdg_output_manager =
      wl_global_create(rs->wl_display,
                       &zxdg_output_manager_v1_interface,
                       3,
                       rs,
                       wl_global_bind_zxdg_output_manager);
    assert(rs->zxdg_output_manager);

    rs->client_created.notify = wl_client_created;
    wl_display_add_client_created_listener(rs->wl_display, &rs->client_created);

    return 0;
fail:
    return 1;
}
