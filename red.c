#include <errno.h> // IWYU pragma: keep
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "actions.h"
#include "backend-drm.h"
#include "backend-wayland.h"
#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "input.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
#include "render.h"
#include "signals.h"
#include "time.h"
#include "vt.h"
#include "wayland.h"

struct gl_proc* gl_proc = NULL;

int
init_env_vars()
{
    for (size_t i = 0; i < cfg.env_vars_len; i++) {
        setenv(cfg.env_vars[i][0], cfg.env_vars[i][1], 1);
    }
    return 0;
}

int
init_auto_start_progs()
{
    for (size_t i = 0; i < cfg.auto_start_progs_len; i++) {
        spawn_program(cfg.auto_start_progs[i].args,
                      cfg.auto_start_progs[i].args_len);
    }
    return 0;
}

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = -1;

    struct redstate* rs;
    rs = malloc(sizeof(*rs));
    assert(rs);

    rs->sig_fd           = -1;
    rs->tty_fd           = -1;
    rs->li_fd            = -1;
    rs->backend_fd       = -1;
    rs->wl_event_loop_fd = -1;
    rs->pfds             = (struct pollfd[__REDPFDS_SIZE]){
        [RFD_LIBINPUT]   = { .fd = -1, .events = POLLIN },
        [RFD_SIGNALS]    = { .fd = -1, .events = POLLIN },
        [RFD_BACKEND]    = { .fd = -1, .events = POLLIN },
        [RFD_WAYLAND]    = { .fd = -1, .events = POLLIN },
        [RFD_CURSOR]     = { .fd = -1, .events = POLLIN },
        [RFD_REDRAWSYNC] = { .fd = -1, .events = POLLIN },
        [RFD_TICK]       = { .fd = -1, .events = POLLIN },
    };
    rs->li                = NULL;
    rs->active            = 1;
    rs->should_quit       = 0;
    rs->time_start        = time_get_now();
    rs->last_frame_time   = time_get_elapsed_sec(rs->time_start);
    rs->is_wayland_client = false;
    if (!getenv("RED_DONT_SPAWN_CLIENT"))
        if (getenv("WAYLAND_DISPLAY") ||
            strcmp(getenv("XDG_SESSION_TYPE"), "wayland") == 0) {
            rs->is_wayland_client = true;
            ROG_INFO("Spawning as wl client");
        }
    rs->backend    = (rs->is_wayland_client) ? &backend_wayland : &backend_drm;
    rs->backend->d = rs->backend->init_data();
    rs->xkb        = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(rs->xkb);
    rs->xkb_keymap_fd      = -1;
    rs->xkb_keymap_string  = NULL;
    rs->xkb_keymap_size    = 0;
    rs->xkb_mods_depressed = 0;
    rs->xkb_mods_latched   = 0;
    rs->xkb_mods_locked    = 0;
    rs->xkb_group          = 0;
    if (xkb_init_keyboard(rs)) {
        goto end;
    }

    rs->wl_display                    = NULL;
    rs->wl_event_loop                 = NULL;
    rs->wl_compositor                 = NULL;
    rs->xdg_wm_base                   = NULL;
    rs->xdg_decoration_manager        = NULL;
    rs->wl_output                     = NULL;
    rs->wl_seat                       = NULL;
    rs->zwp_linux_dmabuf              = NULL;
    rs->wp_viewporter                 = NULL;
    rs->zwp_relative_pointer_manager  = NULL;
    rs->zwp_pointer_constraints       = NULL;
    rs->zwlr_layer_shell              = NULL;
    rs->wp_presentation               = NULL;
    rs->wl_subcompositor          = NULL;
    rs->wl_data_device_manager = NULL;
    rs->client_created                = (struct wl_listener){};

    rs->needs_redraw = 1;
    rs->should_draw  = 0;

    rs->rcs                   = (typeof(rs->rcs))dll_init();
    rs->rts                   = (typeof(rs->rts))dll_init();
    rs->layer_rsurfs          = (typeof(rs->layer_rsurfs))dll_init();
    rs->focused_rt            = NULL;
    rs->last_focused_rt       = NULL;
    rs->pointer_focused_rsurf = NULL;

    rs->keyboard_focused_rsurf           = NULL;
    rs->keyboard_focused_rsurf_exclusive = 0;

    rs->cursor_gl_program       = 0;
    rs->cursor_gl_vao           = 0;
    rs->cursor_x                = 0;
    rs->cursor_y                = 0;
    rs->using_hardware_cursor   = 1;
    rs->cursor_last_scroll_time = 0;
    rs->cursor_hide_timer_fd    = timerfd_create(CLOCK_REALTIME, 0);
    rs->tick_timer_fd           = timerfd_create(CLOCK_MONOTONIC, 0);
    rs->relative_pointers       = (typeof(rs->relative_pointers))dll_init();
    rs->cursor_locked           = 0;
    rs->cursor_hidden           = 0;

    rs->program           = 0;
    rs->vao               = 0;
    rs->vbo               = 0;
    rs->texture_loc       = 0;
    rs->dimentions_loc    = 0;
    struct itimerspec its = {
        .it_value    = { .tv_sec = 1, 0 },
        .it_interval = { .tv_sec = 1, 0 },
    };
    timerfd_settime(rs->tick_timer_fd, 0, &its, NULL);

    rs->queued_rb = NULL;

    rs->dds              = (typeof(rs->dds))dll_init();
    rs->selection_source = NULL;

    if (init_gl_proc()) {
        goto end;
    }

    // backend
    if (rs->backend->init(rs)) {
        goto end;
    }

    // signals
    if ((rs->sig_fd = init_signals()) < 0) {
        goto end;
    }

    // VT
    if (!getenv("RED_DONT_SPAWN_CLIENT") && !rs->is_wayland_client)
        if ((rs->tty_fd = init_vt()) == -1) {
            goto end;
        }

    // libinput
    if (!(rs->li = init_input())) {
        ROG_ERR("failed to init libinput");
        goto end;
    }

    // gl
    {
        if (!rs->using_hardware_cursor)
            if (gl_setup_cursor_program(rs)) {
                ROG_ERR("opengl failed to setup cursor program");
                goto end;
            }

        if (gl_setup_program(rs)) {
            ROG_ERR("opengl failed to setup program");
            goto end;
        }
    }

    rs->should_draw = 1;
    // render buffers initially
    rs->backend->push_init_buffer(rs);

    init_compositor(rs);

    init_env_vars();
    init_auto_start_progs();

    if ((rs->li_fd = libinput_get_fd(rs->li)) < 0) {
        ROG_ERR("failed get libinput fd: %s", strerror(errno));
        goto end;
    }

    if ((rs->backend_fd = rs->backend->get_fd(rs->backend->d)) < 0) {
        goto end;
    }

    if ((rs->wl_event_loop_fd = wl_event_loop_get_fd(rs->wl_event_loop)) < 0) {
        ROG_ERR("failed to get event loop fd");
        goto end;
    }

    rs->pfds[RFD_LIBINPUT].fd = rs->li_fd;
    rs->pfds[RFD_SIGNALS].fd  = rs->sig_fd;
    rs->pfds[RFD_BACKEND].fd  = rs->backend_fd;
    rs->pfds[RFD_WAYLAND].fd  = rs->wl_event_loop_fd;
    rs->pfds[RFD_CURSOR].fd   = rs->cursor_hide_timer_fd;
    rs->pfds[RFD_TICK].fd     = rs->tick_timer_fd;

    ROG_INFO("Starting loop...");
    while (!rs->should_quit) {
        wl_display_flush_clients(rs->wl_display);
        rs->backend->flush_events(rs->backend->d);

        if (poll(rs->pfds, __REDPFDS_SIZE, -1) == -1) {
            ROG_ERR("poll fds error");
            goto end;
        }

        if (rs->pfds[RFD_BACKEND].revents & POLLIN || rs->is_wayland_client)
            rs->backend->handle_events(rs->backend->d);

        enum redpfds revent_pfd;
        do {
            revent_pfd = __REDPFDS_NONE;
            for (int i = 0; i < __REDPFDS_SIZE; i++) {
                if (rs->pfds[i].revents & (POLLIN | POLLERR | POLLHUP)) {
                    revent_pfd          = i;
                    rs->pfds[i].revents = 0;
                    break;
                }
            }

            switch (revent_pfd) {
                case RFD_LIBINPUT:
                    if (input_dispatch(rs))
                        goto end;
                    break;

                case RFD_SIGNALS: {
                    int prev_active = rs->active;

                    if (handle_signal(rs) == -1) {
                        goto end;
                    }

                    // redraw on aquire
                    if (rs->active && prev_active != rs->active) {
                        rs->backend->push_init_buffer(rs);
                    }
                    break;
                }

                // handled above
                case RFD_BACKEND:
                    break;
                case RFD_WAYLAND:
                    wl_event_loop_dispatch(rs->wl_event_loop, 0);
                    break;

                case RFD_CURSOR: {
                    uint64_t expirations;
                    int      n = read(rs->cursor_hide_timer_fd,
                                 &expirations,
                                 sizeof(expirations));
                    if (n != sizeof(expirations))
                        continue;
                    if (drm_hide_cursor(rs))
                        goto end;
                    break;
                }

                case RFD_REDRAWSYNC:
                    redraw_done(rs);
                    break;

                case RFD_TICK: {
                    {
                        uint64_t expirations;
                        int      n = read(
                          rs->tick_timer_fd, &expirations, sizeof(expirations));
                        if (n != sizeof(expirations))
                            continue;
                    }

                    red_on_tick(rs);
                    break;
                }

                case __REDPFDS_SIZE:
                case __REDPFDS_NONE:
                    break;
            }
        } while (revent_pfd != __REDPFDS_NONE);
    }

    ret = 0;
end:
    ret *= -1;
    ROG_WARN("Closing..");

    // if (rs->drm && rs->drm->fd != -1)
    //     close(rs->drm->fd);

    if (rs->tty_fd != -1)
        vt_stop(rs->tty_fd);

    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    if (rs->li)
        libinput_unref(rs->li);

    if (rs->time_start)
        free(rs->time_start);

    // if (drm)
    //     free(drm);
    // if (rs->wl)
    //     free(rs->wl);

    if (rs->xkb)
        xkb_destroy(rs);

    dll_destroy(rs->rcs);

    if (rs)
        free(rs);

    ROG_PRINT_CLOSE();
    return ret;
}
