#include <errno.h> // IWYU pragma: keep
#include <libinput.h>
#include <poll.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "actions.h"
#include "backend-drm.h"
#include "backend-wayland.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "input.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
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

    rs->sig_fd            = -1;
    rs->tty_fd            = -1;
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

    rs->wl_display             = NULL;
    rs->wl_event_loop          = NULL;
    rs->wl_compositor          = NULL;
    rs->xdg_wm_base            = NULL;
    rs->xdg_decoration_manager = NULL;
    rs->wl_output              = NULL;
    rs->wl_seat                = NULL;
    rs->zwp_linux_dmabuf       = NULL;
    rs->needs_redraw           = 1;
    rs->should_draw            = 1;

    rs->rcs        = (typeof(rs->rcs))dll_init();
    rs->rts        = (typeof(rs->rts))dll_init();
    rs->focused_rt = NULL;

    rs->cursor_gl_program       = 0;
    rs->cursor_gl_vao           = 0;
    rs->cursor_x                = 0;
    rs->cursor_y                = 0;
    rs->using_hardware_cursor   = 0;
    rs->cursor_last_motion_time = 0;
    rs->cursor_last_scroll_time = 0;
    rs->cursor_hide_timer       = timerfd_create(CLOCK_REALTIME, 0);
    rs->relative_pointers       = (typeof(rs->relative_pointers))dll_init();
    rs->cursor_locked           = 0;
    rs->cursor_hidden           = 0;

    rs->program     = 0;
    rs->vao         = 0;
    rs->vbo         = 0;
    rs->texture_loc = 0;

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

    // render buffers initially
    rs->backend->push_init_buffer(rs);

    init_compositor(rs);

    init_env_vars();
    init_auto_start_progs();

    // loop
    {
        size_t        fds_size = 5;
        struct pollfd fds[fds_size];

        int li_fd = libinput_get_fd(rs->li);
        if (li_fd < 0) {
            ROG_ERR("failed get libinput fd: %s", strerror(errno));
            goto end;
        }

        int backend_fd = rs->backend->get_fd(rs->backend->d);
        if (backend_fd < 0) {
            goto end;
        }

        int wl_event_loop_fd = wl_event_loop_get_fd(rs->wl_event_loop);
        if (wl_event_loop_fd < 0) {
            ROG_ERR("failed to get event loop fd");
            goto end;
        }

        fds[0].fd     = li_fd;
        fds[0].events = POLLIN;
        fds[1].fd     = rs->sig_fd;
        fds[1].events = POLLIN;
        fds[2].fd     = backend_fd;
        fds[2].events = POLLIN;
        fds[3].fd     = wl_event_loop_fd;
        fds[3].events = POLLIN;
        fds[4].fd     = rs->cursor_hide_timer;
        fds[4].events = POLLIN;

        ROG_INFO("Starting loop...");
        while (!rs->should_quit) {
            wl_display_flush_clients(rs->wl_display);
            rs->backend->flush_events(rs->backend->d);

            if (poll(fds, fds_size, -1) == -1) {
                ROG_ERR("poll fds error");
                goto end;
            }

            // backend events
            if (fds[2].revents & POLLIN || rs->is_wayland_client) {
                rs->backend->handle_events(rs->backend->d);
            }

            // wayland events
            if (fds[3].revents & POLLIN) {
                wl_event_loop_dispatch(rs->wl_event_loop, 0);
            }

            // signal
            if (fds[1].revents & POLLIN) {
                int prev_active = rs->active;

                if (handle_signal(rs) == -1) {
                    goto end;
                }

                // redraw on aquire
                if (rs->active && prev_active != rs->active) {
                    rs->backend->push_init_buffer(rs);
                }
            }

            // input event
            if (fds[0].revents & POLLIN) {
                if (input_dispatch(rs))
                    goto end;
            }

            if (fds[4].revents & POLLIN) {
                uint64_t expirations;
                int      n = read(
                  rs->cursor_hide_timer, &expirations, sizeof(expirations));
                if (n != sizeof(expirations))
                    continue;
                if (drm_hide_cursor(rs))
                    goto end;
            }
        }
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
