#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "xdg-decoration-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>

int
wl_send_pending_callback(struct redsurface* rsurf)
{
    if (!rsurf || !rsurf->pending_callback)
        return 0;

    uint32_t now_ms = (uint32_t)(wl_display_get_serial(rsurf->rs->wl_display));
    wl_callback_send_done(rsurf->pending_callback, now_ms);
    wl_resource_destroy(rsurf->pending_callback);
    rsurf->pending_callback = NULL;

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
    rsurf->pending_buffer    = buffer;
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
    struct redsurface*  rsurf = wl_resource_get_user_data(resource);
    struct wl_resource* cb =
      wl_resource_create(client, &wl_callback_interface, 1, callback);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);

    rsurf->pending_callback = cb;
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
wl_surface_commit(struct wl_client* client, struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);

    if (!rsurf->rs->focused_trc)
        return;

    // on first commit, client is not sending a buffer
    if (rsurf->xdg_surface && !rsurf->configured)
        return;

    // NOTE: clients that are on background
    // do not need redraw or frame callback
    if (client != rsurf->rs->focused_trc->wl_client)
        return;

    request_redraw(rsurf->rs);
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

static void
wl_surface_resource_destroy(struct wl_resource* resource)
{
    struct redsurface* rsurf = wl_resource_get_user_data(resource);

    if (rsurf->app_id)
        free(rsurf->app_id);
    free(rsurf);
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
    rsurf->rs                 = NULL;
    rsurf->configured         = 0;
    rsurf->pending_buffer     = NULL;
    rsurf->pending_callback   = NULL;
    rsurf->wl_surface         = NULL;
    rsurf->xdg_toplevel       = NULL;
    rsurf->app_id             = NULL;
    rsurf->old_pending_buffer = NULL;
    rsurf->geom_x             = 0;
    rsurf->geom_y             = 0;
    rsurf->geom_width         = 0;
    rsurf->geom_height        = 0;
    rsurf->geom_configured    = 0;

    return rsurf;
}

static void
wl_compositor_create_surface(struct wl_client*   client,
                             struct wl_resource* resource,
                             uint32_t            id)
{
    struct redstate* rs = resource->data;

    struct redsurface* rsurf = init_redsurface();
    if (!rsurf) {
        ROG_ERR("oom?");
        return;
    }
    rsurf->rs         = rs;
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
}

static void
wl_compositor_create_region(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            id)
{
    struct wl_resource* r = wl_resource_create(
      client, &wl_region_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &wl_region_implementation, NULL, NULL);
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
    struct redsurface* rsurf = resource->data;
    red_destroy_trc(rsurf->rs, rsurf);
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
    struct redsurface* rsurf = resource->data;
    rsurf->app_id            = malloc(strlen(app_id) + 1);
    strcpy(rsurf->app_id, app_id);
    ROG("app id: %s", rsurf->app_id);
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

    rsurf->app_id       = "";
    rsurf->xdg_toplevel = wl_resource_create(
      client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!rsurf->xdg_toplevel) {
        ROG_ERR("oom?");
        return;
    }

    red_create_trc(rsurf->rs, rsurf, client);

    wl_resource_set_implementation(
      rsurf->xdg_toplevel, &xdg_toplevel_implementation, rsurf, NULL);

    uint32_t width  = rsurf->rs->backend->get_width(rsurf->rs->backend->d);
    uint32_t height = rsurf->rs->backend->get_height(rsurf->rs->backend->d);

    struct wl_array states;
    wl_array_init(&states);
    // setting maximized state as it basicly tells the client to not render csd
    // also if we have one toplevel drawn at a time,
    // it might as well be maximized
    uint32_t* s = wl_array_add(&states, sizeof(uint32_t));
    *s          = XDG_TOPLEVEL_STATE_MAXIMIZED;
    xdg_toplevel_send_configure(rsurf->xdg_toplevel, width, height, &states);
    wl_array_release(&states);

    uint32_t serial = wl_display_next_serial(rsurf->rs->wl_display);
    xdg_surface_send_configure(resource, serial);
}

static void
xdg_surface_get_popup(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            id,
                      struct wl_resource* parent,
                      struct wl_resource* positioner)
{
    ROG("xdg surface get popup called");
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
    rsurf->geom_configured   = 1;
    rsurf->geom_width        = width;
    rsurf->geom_height       = height;
    rsurf->geom_x            = x;
    rsurf->geom_y            = y;
}

static void
xdg_surface_ack_configure(struct wl_client*   client,
                          struct wl_resource* resource,
                          uint32_t            serial)
{
    struct redsurface* rsurf = resource->data;
    rsurf->configured        = 1;
}

static const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy             = xdg_surface_destroy,
    .get_toplevel        = xdg_surface_get_toplevel,
    .get_popup           = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure       = xdg_surface_ack_configure,
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
    // struct wl_resource* r = wl_resource_create(
    //   client, &xdg_positioner_interface, wl_resource_get_version(resource),
    //   id);
    // wl_resource_set_implementation(r, NULL, NULL, NULL);
    ROG("wm base create positioner called");
}

static void
xdg_wm_base_get_xdg_surface(struct wl_client*   client,
                            struct wl_resource* resource,
                            uint32_t            id,
                            struct wl_resource* surface)
{
    struct redsurface* rsurf = surface->data;

    rsurf->xdg_surface = wl_resource_create(
      client, &xdg_surface_interface, wl_resource_get_version(resource), id);
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
    struct wl_resource* r =
      wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(r, &wl_output_implementation, data, NULL);

    wl_output_send_geometry(r,
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

    wl_output_send_mode(r,
                        WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        width,
                        height,
                        60 * 1000);
    if (version >= 2)
        wl_output_send_scale(r, 1);
    if (version >= 4)
        wl_output_send_name(r, "red-1");
    wl_output_send_done(r);
}

static void
wl_keyboard_release(struct wl_client* client, struct wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface wl_keyboard_implementation = {
    .release = wl_keyboard_release,
};

static void
wl_pointer_set_cursor(struct wl_client*   client,
                      struct wl_resource* resource,
                      uint32_t            serial,
                      struct wl_resource* surface,
                      int32_t             hotspot_x,
                      int32_t             hotspot_y)
{
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

static void
wl_seat_get_pointer(struct wl_client*   client,
                    struct wl_resource* resource,
                    uint32_t            id)
{
    struct redstate*    rs         = wl_resource_get_user_data(resource);
    struct wl_resource* wl_pointer = wl_resource_create(
      client, &wl_pointer_interface, wl_resource_get_version(resource), id);
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
    wl_resource_set_implementation(
      wl_keyboard, &wl_keyboard_implementation, wl_keyboard, NULL);

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
    struct wl_resource* r =
      wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(r, &wl_seat_implementation, data, NULL);
    wl_seat_send_capabilities(
      r, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
    if (version >= 2)
        wl_seat_send_name(r, "seat0");
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
    struct redsurface* surf = wl_resource_get_user_data(resource);
    surf->sub_x             = x;
    surf->sub_y             = y;
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
    struct redsurface* surf   = wl_resource_get_user_data(surface_resource);
    struct redsurface* parent = wl_resource_get_user_data(parent_resource);
    surf->parent              = parent;

    struct wl_resource* r = wl_resource_create(
      client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(r, &subsurface_impl, surf, NULL);
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
    wl_resource_set_implementation(xdg_decoration_manager,
                                   &zxdg_decoration_manager_v1_implementation,
                                   data,
                                   NULL);
}

static void
wl_client_destroyed(struct wl_listener* listener, void* data)
{
    struct redclient* rc = wl_container_of(listener, rc, client_destroyed);

    dll_remove_val(rc->rs->rcs, rc);
    // just in case client doesn't call xdg_toplevel destroy.
    // client destroy, should be called so its a safe fallback
    {
        int is_trcs = 0;
        dll_for_each(rc->rs->trcs, v)
        {
            if (v->val == rc)
                is_trcs = 1;
        }
        if (is_trcs)
            red_destroy_trc(rc->rs, rc->rsurf);
    }
    free(rc);
}

static void
wl_client_created(struct wl_listener* listener, void* data)
{
    struct redstate* rs = wl_container_of(listener, rs, client_created);

    struct wl_client* wl_client = data;

    struct redclient* rc;
    rc                          = malloc(sizeof(*rc));
    rc->rs                      = rs;
    rc->rsurf                   = NULL;
    rc->wl_keyboard             = NULL;
    rc->wl_pointer              = NULL;
    rc->wl_client               = wl_client;
    rc->client_destroyed.notify = wl_client_destroyed;

    wl_client_add_destroy_listener(wl_client, &rc->client_destroyed);
    dll_push_tail(rs->rcs, rc);
}

static void
handle_wl_log(const char* _fmt, va_list args)
{
    // remove newline at end
    int   l   = strlen(_fmt);
    char* fmt = malloc(l + 1);
    strcpy(fmt, _fmt);
    fmt[l - 1] = '\0';

    ROG_INFO_VARGS(fmt, args);
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
                                         4,
                                         rs,
                                         wl_global_bind_compositor);
    if (!rs->wl_compositor) {
        ROG_ERR("could not create wl_compositor");
        goto fail;
    }

    rs->xdg_wm_base = wl_global_create(rs->wl_display,
                                       &xdg_wm_base_interface,
                                       5,
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
      rs->wl_display, &wl_seat_interface, 5, rs, wl_global_bind_seat);
    if (!rs->wl_seat) {
        ROG_ERR("could not create wl_output");
        goto fail;
    }

    rs->subcompositor_global = wl_global_create(
      rs->wl_display, &wl_subcompositor_interface, 1, rs, bind_subcompositor);

    rs->data_device_manager_global =
      wl_global_create(rs->wl_display,
                       &wl_data_device_manager_interface,
                       3,
                       rs,
                       bind_data_device_manager);

    rs->xdg_decoration_manager =
      wl_global_create(rs->wl_display,
                       &zxdg_decoration_manager_v1_interface,
                       1,
                       rs,
                       wl_global_bind_xdg_decoration_manager);

    rs->client_created.notify = wl_client_created;
    wl_display_add_client_created_listener(rs->wl_display, &rs->client_created);

    return 0;
fail:
    return 1;
}
