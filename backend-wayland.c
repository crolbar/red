#include "backend-wayland-client.h"
#include "backend-wayland.h"
#include "gbm.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdlib.h>
#include <string.h>

void*
backend_wayland_init_data()
{
    struct backend_wayland* bw = NULL;
    bw                         = malloc(sizeof(*bw));
    if (!bw)
        return NULL;

    bw->wc          = NULL;
    bw->egl_display = NULL;
    bw->egl_context = NULL;
    bw->gbm_dev     = NULL;
    bw->rb0         = NULL;
    bw->rb1         = NULL;
    bw->width       = 1600;
    bw->height      = 1000;
    bw->used_rb     = 0;
    bw->drm_fd      = -1;

    return bw;
}

uint32_t
backend_wayland_get_width(void* data)
{
    struct backend_wayland* bw = data;
    return bw->width;
}

uint32_t
backend_wayland_get_height(void* data)
{
    struct backend_wayland* bw = data;
    return bw->height;
}

int
backend_wayland_init(void* data)
{
    struct redstate*        rs = data;
    struct backend_wayland* bw = rs->backend->d;

    bw->wc = init_wayland();
    if (!bw->wc)
        goto fail;

    // TODO in drm.c
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ROG_ERR("failed oppening drm device: %s", strerror(errno));
        goto fail;
    }
    bw->drm_fd = fd;

    bw->gbm_dev = init_gbm(fd);
    if (!bw->gbm_dev)
        goto fail;

    if (init_egl(bw->gbm_dev, &bw->egl_display, &bw->egl_context))
        goto fail;

    bw->rb0 = init_wl_buffer(bw);
    if (!bw->rb0)
        goto fail;

    bw->rb1 = init_wl_buffer(bw);
    if (!bw->rb1)
        goto fail;

    wl_buffer_add_listener(bw->rb0->wl_buffer, &wl_buffer_listener, bw->rb0);
    wl_buffer_add_listener(bw->rb1->wl_buffer, &wl_buffer_listener, bw->rb1);
    wl_keyboard_add_listener(bw->wc->wl_keyboard, &wl_keyboard_listener, rs);
    wl_pointer_add_listener(bw->wc->wl_pointer, &wl_pointer_listener, rs);

    xdg_toplevel_add_listener(bw->wc->xdg_toplevel, &xdg_toplevel_listener, rs);
    xdg_surface_add_listener(bw->wc->xdg_surface, &xdg_surface_listener, rs);
    xdg_toplevel_set_title(bw->wc->xdg_toplevel, "red");
    xdg_toplevel_set_app_id(bw->wc->xdg_toplevel, "red");

    wl_display_roundtrip(bw->wc->wl_display);
    return 0;
fail:
    if (bw->wc)
        free_wayland(bw->wc);
    if (bw->gbm_dev)
        gbm_device_destroy(bw->gbm_dev);
    return 1;
}

redbuffer*
backend_wayland_pull_buffer(void* data)
{
    struct backend_wayland* bw = data;

    bw->used_rb = 1 - bw->used_rb;
    if (bw->used_rb)
        return bw->rb1;
    return bw->rb0;
}

int
backend_wayland_push_buffer(void* data, redbuffer* rb)
{
    struct redstate*        rs = data;
    struct backend_wayland* bw = rs->backend->d;

    rb->free = 0;
    wl_surface_attach(bw->wc->wl_surface, rb->wl_buffer, 0, 0);
    wl_surface_damage_buffer(bw->wc->wl_surface, 0, 0, bw->width, bw->height);

    bw->is_ready_for_frame = 0;
    struct wl_callback* cb = wl_surface_frame(bw->wc->wl_surface);
    wl_callback_add_listener(cb, &wl_frame_listener, rs);

    wl_surface_commit(bw->wc->wl_surface);
    return 0;
}

int
backend_wayland_push_init_buffer(void* data)
{
    struct redstate* rs = data;
    redraw(rs);
    return 0;
}

int
backend_wayland_resize_buffer(void* d, struct redbuffer* rb)
{
    struct backend_wayland* bw = d;

    wl_buffer_destroy(rb->wl_buffer);
    glDeleteFramebuffers(1, &rb->fbo);
    glDeleteRenderbuffers(1, &rb->rbo);
    eglDestroyImage(bw->egl_display, rb->egl_image);
    gbm_bo_destroy(rb->gbm_bo);

    struct redbuffer* new_buf = init_wl_buffer(bw);
    if (!new_buf)
        return 1;

    *rb = *new_buf;
    free(new_buf);

    wl_buffer_add_listener(rb->wl_buffer, &wl_buffer_listener, rb);

    return 0;
}

int
backend_wayland_get_fd(void* d)
{
    struct backend_wayland* bw = d;

    int fd = wl_display_get_fd(bw->wc->wl_display);
    if (fd < 0) {
        ROG_ERR("falied to get wl display fd");
        return -1;
    }
    return fd;
}

int
backend_wayland_flush_events(void* d)
{
    struct backend_wayland* bw = d;

    while (wl_display_prepare_read(bw->wc->wl_display) != 0)
        wl_display_dispatch_pending(bw->wc->wl_display);
    wl_display_flush(bw->wc->wl_display);

    return 0;
}
int
backend_wayland_handle_events(void* d)
{
    struct backend_wayland* bw = d;

    wl_display_read_events(bw->wc->wl_display);
    wl_display_dispatch_pending(bw->wc->wl_display);

    return 0;
}

int
backend_wayland_is_ready_for_frame(void* d)
{
    struct backend_wayland* bw = d;
    return bw->is_ready_for_frame;
}

struct backend backend_wayland = {
    .d                  = NULL,
    .init_data          = backend_wayland_init_data,
    .init               = backend_wayland_init,
    .get_height         = backend_wayland_get_height,
    .get_width          = backend_wayland_get_width,
    .pull_buffer        = backend_wayland_pull_buffer,
    .push_buffer        = backend_wayland_push_buffer,
    .push_init_buffer   = backend_wayland_push_init_buffer,
    .resize_buffer      = backend_wayland_resize_buffer,
    .get_fd             = backend_wayland_get_fd,
    .flush_events       = backend_wayland_flush_events,
    .handle_events      = backend_wayland_handle_events,
    .is_ready_for_frame = backend_wayland_is_ready_for_frame,
};
