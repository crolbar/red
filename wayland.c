#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "linux-dmabuf-server-protocol.h"
#include "log.h"
#include "pointer-constraints-server-protocol.h"
#include "red.h"
#include "relative-pointer-server-protocol.h"
#include "render.h"
#include "viewporter-server-protocol.h"
#include "wayland.h"
#include "xdg-decoration-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include <assert.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>
#include <wayland-util.h>

int
red_send_pending_callback(struct redsurface* rsurf)
{
    if (!rsurf || !rsurf->pending_callback)
        return 0;

    uint32_t now_ms = (uint32_t)(wl_display_get_serial(rsurf->rs->wl_display));
    wl_callback_send_done(rsurf->pending_callback, now_ms);
    wl_resource_destroy(rsurf->pending_callback);
    rsurf->pending_callback = NULL;

    return 0;
}

int
red_on_frame_done(struct redstate* rs)
{
    // TODO: can we miss a pending callback when we change focus when
    // page_flip_ready == 0 ?
    if (rs->focused_rt)
        red_send_pending_callback(rs->focused_rt->rsurf);

    // if updates happened on page flip.
    // shouldn't happen much as we have one window
    redraw(rs);
    return 0;
}

static void
wl_surface_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
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

    if (rsurf->pending_buffer) {
        wl_buffer_send_release(rsurf->pending_buffer);
        rsurf->pending_buffer = NULL;
    }

    rsurf->pending_buffer = buffer;
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

    // TODO: works for now, later we should do if rsurf is not on focus
    // we should throttle it with some timer
    red_send_pending_callback(rsurf);

    struct wl_resource* cb =
      wl_resource_create(client, &wl_callback_interface, 1, callback);
    assert(cb);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);

    rsurf->pending_callback = cb;
}

static void
wl_surface_commit(struct wl_client* client, struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);
    assert(rsurf);
    assert(rsurf->rs);

    if (!rsurf->rs->focused_rt)
        return;

    // on first commit, client is not sending a buffer
    if (rsurf->xdg_surface && !rsurf->configured)
        return;

    // NOTE: redsurfaces that are on background
    // do not need redraw or frame callback
    if (rsurf != rsurf->rs->focused_rt->rsurf)
        return;

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
    ROG("destroing wl_surf: %d", rsurf);
#endif

    // remove wl_surf from rsurfs of redclient
    if (red_is_client_valid(rsurf->rs, rsurf->rc))
        dll_remove_val(rsurf->rc->rsurfs, rsurf);

    if (rsurf)
        free(rsurf);
    rsurf = NULL;
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

struct redsurface*
init_redsurface()
{
    struct redsurface* rsurf = NULL;
    rsurf                    = malloc(sizeof(*rsurf));
    if (!rsurf) {
        return NULL;
    }
    rsurf->rs               = NULL;
    rsurf->rc               = NULL;
    rsurf->configured       = 0;
    rsurf->pending_buffer   = NULL;
    rsurf->pending_callback = NULL;
    rsurf->wl_surface       = NULL;
    rsurf->xdg_toplevel     = NULL;
    rsurf->geom_x           = 0;
    rsurf->geom_y           = 0;
    rsurf->geom_width       = 0;
    rsurf->geom_height      = 0;
    rsurf->geom_configured  = 0;
    rsurf->tex              = 0;
    rsurf->tex_h            = 0;
    rsurf->tex_w            = 0;

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

    wl_resource_set_implementation(rsurf->wl_surface,
                                   &wl_surface_implementation,
                                   rsurf,
                                   wl_surface_resource_destroy);

    dll_push_tail(rc->rsurfs, rsurf);
}

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
}

static void
xdg_toplevel_set_app_id(struct wl_client*   client,
                        struct wl_resource* resource,
                        const char*         app_id)
{
    struct redtoplevel* rt = resource->data;
    assert(rt);

    rt->app_id = malloc(strlen(app_id) + 1);
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
}

static void
xdg_toplevel_unset_maximized(struct wl_client*   client,
                             struct wl_resource* resource)
{
}

static void
xdg_toplevel_set_fullscreen(struct wl_client*   client,
                            struct wl_resource* resource,
                            struct wl_resource* output)
{
}

static void
xdg_toplevel_unset_fullscreen(struct wl_client*   client,
                              struct wl_resource* resource)
{
}

static void
xdg_toplevel_set_minimized(struct wl_client*   client,
                           struct wl_resource* resource)
{
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

int
red_send_configure(struct redsurface* rsurf, int activated, int resizing)
{
    rsurf->configured = 0;

    uint32_t width  = rsurf->rs->backend->get_width(rsurf->rs->backend->d);
    uint32_t height = rsurf->rs->backend->get_height(rsurf->rs->backend->d);

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
    xdg_toplevel_send_configure(rsurf->xdg_toplevel, width, height, &states);
    wl_array_release(&states);

    assert(rsurf->xdg_surface);
    assert(rsurf->rs && rsurf->rs->wl_display);
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

    red_send_configure(rsurf, 1, 0);
}

static void
xdg_surface_get_popup(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            id,
                      struct wl_resource* parent,
                      struct wl_resource* positioner)
{
    struct wl_resource* xdg_popup = wl_resource_create(
      client, &xdg_popup_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(xdg_popup,
                                   &xdg_popup_implementation,
                                   wl_resource_get_user_data(resource),
                                   NULL);
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
    rsurf->geom_configured = 1;
    rsurf->geom_width      = width;
    rsurf->geom_height     = height;
    rsurf->geom_x          = x;
    rsurf->geom_y          = y;
}

static void
xdg_surface_ack_configure(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            serial)
{
    struct redsurface* rsurf = resource->data;
    assert(rsurf);
    rsurf->configured = 1;
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
}
static void
xdg_positioner_set_anchor_rect(struct wl_client*   client,
                               struct wl_resource* resource,
                               int32_t             x,
                               int32_t             y,
                               int32_t             width,
                               int32_t             height)
{
}
static void
xdg_positioner_set_anchor(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            anchor)
{
}
static void
xdg_positioner_set_gravity(struct wl_client*   client,
                           struct wl_resource* resource,
                           uint32_t            gravity)
{
}
static void
xdg_positioner_set_constraint_adjustment(struct wl_client*   client,
                                         struct wl_resource* resource,
                                         uint32_t constraint_adjustment)
{
}
static void
xdg_positioner_set_offset(struct wl_client*   client,
                          struct wl_resource* resource,
                          int32_t             x,
                          int32_t             y)
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
xdg_wm_base_destroy(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static void
xdg_wm_base_create_positioner(struct wl_client*   client,
                              struct wl_resource* resource,
                              uint32_t            id)
{
    struct wl_resource* xdg_positioner = wl_resource_create(
      client, &xdg_positioner_interface, wl_resource_get_version(resource), id);
    assert(xdg_positioner);
    wl_resource_set_implementation(xdg_positioner,
                                   &xdg_positioner_implementation,
                                   wl_resource_get_user_data(resource),
                                   NULL);
}

static void
xdg_wm_base_get_xdg_surface(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            id,
                            struct wl_resource* surface)
{
    struct redsurface* rsurf = surface->data;
    assert(rsurf);

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
                            300,
                            200,
                            WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "red",
                            "red",
                            WL_OUTPUT_TRANSFORM_NORMAL);

    uint32_t width  = rs->backend->get_width(rs->backend->d);
    uint32_t height = rs->backend->get_height(rs->backend->d);

    wl_output_send_mode(wl_output,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        width,
                        height,
                        60 * 1000);
    if (version >= 2)
        wl_output_send_scale(wl_output, 1);
    if (version >= 4)
        wl_output_send_name(wl_output, "red-1");
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
red_keyboard_send_enter(struct redclient* rc, struct wl_resource* wl_surface)
{
    uint32_t        serial = wl_display_next_serial(rc->rs->wl_display);
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(rc->wl_keyboard, serial, wl_surface, &keys);
    wl_array_release(&keys);
    return 0;
}
int
red_keyboard_send_leave(struct redclient* rc, struct wl_resource* wl_surface)
{
    uint32_t serial = wl_display_next_serial(rc->rs->wl_display);
    wl_keyboard_send_leave(rc->wl_keyboard, serial, wl_surface);
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
    if (surface == NULL)
        drm_hide_cursor(rs);
    else // currently not using surface to change cursor, just update.
        drm_update_cursor_plane(rs);
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
    wl_pointer_send_frame(rc->wl_pointer);
    return 0;
}

int
red_pointer_send_leave(struct redclient* rc, struct wl_resource* wl_surface)
{
    uint32_t serial = wl_display_next_serial(rc->rs->wl_display);
    wl_pointer_send_leave(rc->wl_pointer, serial, wl_surface);
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
subsurface_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
subsurface_set_position(struct wl_client*   client,
                        struct wl_resource* resource,
                        int32_t             x,
                        int32_t             y)
{
}

static void
subsurface_place_above(struct wl_client*   c,
                       struct wl_resource* r,
                       struct wl_resource* sibling)
{
}
static void
subsurface_place_below(struct wl_client*   c,
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
    .destroy      = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above  = subsurface_place_above,
    .place_below  = subsurface_place_below,
    .set_sync     = subsurface_set_sync,
    .set_desync   = subsurface_set_desync,
};

static void
subcompositor_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static void
subcompositor_get_subsurface(struct wl_client*   client,
                             struct wl_resource* resource,
                             uint32_t            id,
                             struct wl_resource* surface_resource,
                             struct wl_resource* parent_resource)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &subsurface_impl, NULL, NULL);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
    .destroy        = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void
bind_subcompositor(struct wl_client* client,
                   void*             data,
                   uint32_t          version,
                   uint32_t          id)
{
    struct wl_resource* r =
      wl_resource_create(client, &wl_subcompositor_interface, version, id);
    wl_resource_set_implementation(r, &subcompositor_impl, data, NULL);
}

static void
data_source_offer(struct wl_client*   c,
                  struct wl_resource* r,
                  const char*         mime_type)
{
}
static void
data_source_destroy(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}
static void
data_source_set_actions(struct wl_client*   c,
                        struct wl_resource* r,
                        uint32_t            dnd_actions)
{
}

static const struct wl_data_source_interface data_source_impl = {
    .offer       = data_source_offer,
    .destroy     = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void
data_device_start_drag(struct wl_client*   client,
                       struct wl_resource* resource,
                       struct wl_resource* source,
                       struct wl_resource* origin,
                       struct wl_resource* icon,
                       uint32_t            serial)
{
}

static void
data_device_set_selection(struct wl_client*   client,
                          struct wl_resource* resource,
                          struct wl_resource* source,
                          uint32_t            serial)
{
}

static void
data_device_release(struct wl_client* c, struct wl_resource* r)
{
    wl_resource_destroy(r);
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag    = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release       = data_device_release,
};

static void
data_device_manager_create_data_source(struct wl_client*   client,
                                       struct wl_resource* resource,
                                       uint32_t            id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_source_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &data_source_impl, NULL, NULL);
}

static void
data_device_manager_get_data_device(struct wl_client*   client,
                                    struct wl_resource* resource,
                                    uint32_t            id,
                                    struct wl_resource* seat)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_device_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &data_device_impl, NULL, NULL);
}

static const struct wl_data_device_manager_interface
  data_device_manager_impl = {
      .create_data_source = data_device_manager_create_data_source,
      .get_data_device    = data_device_manager_get_data_device,
  };

static void
bind_data_device_manager(struct wl_client* client,
                         void*             data,
                         uint32_t          version,
                         uint32_t          id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_data_device_manager_interface, version, id);
    wl_resource_set_implementation(r, &data_device_manager_impl, data, NULL);
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
    rc = malloc(sizeof(*rc));
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
    if (dmabuf)
        free(dmabuf);
}

struct dmabuf*
red_get_dmabuf(struct wl_resource* resource)
{
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
    dmabuf->flags  = flags;
    dmabuf->height = height;
    dmabuf->width  = width;
    dmabuf->format = format;
    dmabuf->rs     = params->rs;
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

    dll_remove_val(rs->relative_pointers, resource);
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

    dll_push_tail(rs->relative_pointers, zwp_relative_pointer);
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
                                         6,
                                         rs,
                                         wl_global_bind_compositor);
    if (!rs->wl_compositor) {
        ROG_ERR("could not create wl_compositor");
        goto fail;
    }

    rs->xdg_wm_base = wl_global_create(rs->wl_display,
                                       &xdg_wm_base_interface,
                                       6,
                                       rs,
                                       wl_global_bind_xdg_wm_base);
    if (!rs->xdg_wm_base) {
        ROG_ERR("could not create xdg_wm_base");
        goto fail;
    }

    rs->wl_output = wl_global_create(
      rs->wl_display, &wl_output_interface, 3, rs, wl_global_bind_output);
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

    rs->subcompositor_global = wl_global_create(
      rs->wl_display, &wl_subcompositor_interface, 1, rs, bind_subcompositor);
    assert(rs->subcompositor_global);

    rs->data_device_manager_global =
      wl_global_create(rs->wl_display,
                       &wl_data_device_manager_interface,
                       3,
                       rs,
                       bind_data_device_manager);
    assert(rs->data_device_manager_global);

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

    rs->client_created.notify = wl_client_created;
    wl_display_add_client_created_listener(rs->wl_display, &rs->client_created);

    return 0;
fail:
    return 1;
}
