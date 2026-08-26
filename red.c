#include <errno.h> // IWYU pragma: keep
#include <libseat.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "actions.h"
#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "drm.h"
#include "input.h"
#include "ipc.h"
#include "log.h"
#include "opengl.h"
#include "red.h"
#include "render.h"
#include "signals.h"
#include "time.h"
#include "utils.h"
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
init_red_binds(struct redstate* rs)
{
    rs->binds_len = 0;
    rs->binds_cap = 10;
    rs->binds     = calloc(rs->binds_cap, sizeof(*rs->binds));

    for (size_t i = 0; i < cfg.bind_presets_len; i++) {
        struct redbindpreset preset = cfg.bind_presets[i];
        for (size_t j = 0; j < preset.binds_len; j++) {
            struct redbindcfg cfgbind = preset.binds[j];
            xkb_keysym_t      cfgbind_keysym =
              xkb_keysym_from_name(cfgbind.key, XKB_KEYSYM_NO_FLAGS);

            struct redbind bind = {};
            bind.key_mods_bm  = REDBIND_CREATE_BM(cfgbind_keysym, cfgbind.mods);
            bind.action       = cfgbind.action;
            bind.action_len   = cfgbind.action_len;
            bind.preset_n     = i;
            bind.not_repeated = cfgbind.not_repeated;
            bind.wl_client    = cfgbind.wl_client;

            UTIL_ADD_BIND(rs->binds, bind, rs->binds_len, rs->binds_cap);
        }
    }

    return 0;
fail:
    return 1;
}

int
start_script()
{
    char* fmt = "%s/red/start.sh";
    char* cfg = getenv("XDG_CONFIG_HOME");
    if (!cfg) {
        ROG_WARN("XDG_CONFIG_HOME not set!");
        return 0;
    }
    int   n    = snprintf(NULL, 0, fmt, cfg);
    char* path = malloc(n + 1);
    if (!path) {
        ROG_ERR("oom?");
        return 1;
    }
    sprintf(path, fmt, cfg);

    struct stat sb;
    if (stat(path, &sb)) {
        ROG_INFO("Start script not found in %s, you can "
                 "use this to know when the compositor started up.",
                 path);
        free(path);
        return 0;
    } else
        ROG_INFO("Starting start script: %s", path);

    spawn_program((char*[]){ path, NULL }, 1);
    free(path);
    return 0;
}

int
main(int argc, char** argv)
{
    ROG_INIT();
    int ret = -1;

    struct redstate* rs;
    if (!(rs = calloc(1, sizeof(*rs)))) {
        goto end;
    }

    if (getenv("XDG_RUNTIME_DIR") == NULL) {
        ROG_ERR("XDG_RUNTIME_DIR not set!");
        goto end;
    }

    if ((rs->sig_fd = init_signals()) < 0) {
        ROG_ERR("failed to initialize signals");
        goto end;
    }
    rs->ls_fd            = -1;
    rs->ipc_fd           = -1;
    rs->li_fd            = -1;
    rs->backend_fd       = -1;
    rs->wl_event_loop_fd = -1;
    rs->pfds = (struct pollfd[__REDPFDS_SIZE + RED_IPC_MAX_CLIENTS]){
        [RFD_LIBINPUT]      = { .fd = -1, .events = POLLIN },
        [RFD_LIBSEAT]       = { .fd = -1, .events = POLLIN },
        [RFD_SIGNALS]       = { .fd = -1, .events = POLLIN },
        [RFD_BACKEND]       = { .fd = -1, .events = POLLIN },
        [RFD_WAYLAND]       = { .fd = -1, .events = POLLIN },
        [RFD_CURSOR]        = { .fd = -1, .events = POLLIN },
        [RFD_REDRAWSYNC]    = { .fd = -1, .events = POLLIN },
        [RFD_TICK]          = { .fd = -1, .events = POLLIN },
        [RFD_IPC]           = { .fd = -1, .events = POLLIN },
        [RFD_BIND_REPEATER] = { .fd = -1, .events = POLLIN },
        [RFD_AUTOSCROLL]    = { .fd = -1, .events = POLLIN },
    };

    rs->vt_active                        = 1;
    rs->should_quit                      = 0;
    rs->needs_redraw                     = 1;
    rs->should_draw                      = 0;
    rs->is_wayland_client                = 0;
    rs->backend                          = NULL;
    rs->li                               = NULL;
    rs->ls                               = NULL;
    rs->seat_name                        = NULL;
    rs->xkb                              = NULL;
    rs->xkb_state                        = NULL;
    rs->xkb_keymap                       = NULL;
    rs->xkb_keymap_fd                    = -1;
    rs->xkb_keymap_string                = NULL;
    rs->xkb_keymap_size                  = 0;
    rs->xkb_mods_depressed               = 0;
    rs->xkb_mods_latched                 = 0;
    rs->xkb_mods_locked                  = 0;
    rs->xkb_group                        = 0;
    rs->time_start                       = time_get_now();
    rs->last_frame_time                  = 0;
    rs->frame_latency                    = 0;
    rs->wl_display                       = NULL;
    rs->wl_event_loop                    = NULL;
    rs->wl_compositor                    = NULL;
    rs->xdg_wm_base                      = NULL;
    rs->xdg_decoration_manager           = NULL;
    rs->wl_output                        = NULL;
    rs->wl_seat                          = NULL;
    rs->zwp_linux_dmabuf                 = NULL;
    rs->wp_viewporter                    = NULL;
    rs->zwp_relative_pointer_manager     = NULL;
    rs->zwp_pointer_constraints          = NULL;
    rs->zwlr_layer_shell                 = NULL;
    rs->zwlr_screencopy                  = NULL;
    rs->zxdg_output_manager              = NULL;
    rs->zwlr_foreign_toplevel_manager    = NULL;
    rs->wp_presentation                  = NULL;
    rs->wl_subcompositor                 = NULL;
    rs->wl_data_device_manager           = NULL;
    rs->client_created                   = (struct wl_listener){};
    rs->seat_devices                     = (typeof(rs->seat_devices))dll_init();
    rs->rcs                              = (typeof(rs->rcs))dll_init();
    rs->rts                              = (typeof(rs->rts))dll_init();
    rs->layer_rsurfs                     = (typeof(rs->layer_rsurfs))dll_init();
    rs->rel_pointers                     = (typeof(rs->rel_pointers))dll_init();
    rs->dds                              = (typeof(rs->dds))dll_init();
    rs->selection_source                 = NULL;
    rs->queued_rb                        = NULL;
    rs->focused_rt                       = NULL;
    rs->last_focused_rt                  = NULL;
    rs->pointer_focused_rsurf            = NULL;
    rs->keyboard_focused_rsurf           = NULL;
    rs->keyboard_focused_rsurf_exclusive = 0;
    rs->animation_value                  = 0;
    rs->program                          = 0;
    rs->vao                              = 0;
    rs->vbo                              = 0;
    rs->texture_loc                      = 0;
    rs->dimentions_loc                   = 0;
    rs->anim_loc                         = 0;
    rs->cursor_gl_program                = 0;
    rs->cursor_gl_vao                    = 0;
    rs->cursor_x                         = 0;
    rs->cursor_y                         = 0;
    rs->using_hardware_cursor            = 1;
    rs->cursor_last_scroll_time          = 0;
    rs->cursor_locked                    = 0;
    rs->cursor_hidden                    = 0;
    rs->cursor_hide_timer_fd             = timerfd_create(CLOCK_REALTIME, 0);
    rs->tick_timer_fd                    = timerfd_create(CLOCK_MONOTONIC, 0);
    rs->ipc_red_state_changes            = 0;
    rs->ipc_clients                      = (typeof(rs->ipc_clients))dll_init();
    rs->bind_repeater_fd                 = -1;
    rs->repeat_action                    = NULL;
    rs->repeat_action_len                = 0;
    rs->autoscroll_fd                    = -1;
    rs->autoscroll_point_y               = -1;
    rs->autoscroll_direction             = 0;
    rs->autoscroll_delay                 = 0;
    rs->autoscroll_delay_changed         = 0;
    rs->overlay_rt                       = NULL;
    rs->overlay_rt_x                     = 0;
    rs->overlay_rt_y                     = 0;
    rs->overlay_rt_w                     = 0;
    rs->overlay_rt_h                     = 0;
    rs->overlay_move_mode                = 0;
    rs->overlay_move_mode_diff_x         = 0;
    rs->overlay_move_mode_diff_y         = 0;
    rs->binds                            = NULL;
    rs->binds_len                        = 0;
    rs->binds_cap                        = 0;
    rs->xkb_ctrl_mask                    = 0;
    rs->xkb_shift_mask                   = 0;
    rs->xkb_alt_mask                     = 0;
    rs->xkb_super_mask                   = 0;

    struct itimerspec its = {
        .it_value    = { .tv_sec = 1, 0 },
        .it_interval = { .tv_sec = 1, 0 },
    };
    timerfd_settime(rs->tick_timer_fd, 0, &its, NULL);

    if (ipc_update_pfds(rs)) {
        goto end;
    }

    if (init_gl_proc()) {
        ROG_ERR("failed to initialize gl ext api");
        goto end;
    }

    if (!(rs->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
        goto end;
    }
    if (init_xkb_keyboard(rs)) {
        ROG_ERR("failed to initialize xkb");
        goto end;
    }

    // backend
    // NOTE: rs->is_wayland_client is set here, USE IT ONLY BELOW
    if (init_backend(rs)) {
        ROG_ERR("failed to initialize backend");
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

    if (init_compositor(rs)) {
        ROG_ERR("failed to initialize wayland server");
        goto end;
    }

    if (!rs->is_wayland_client)
        if ((rs->li_fd = libinput_get_fd(rs->li)) < 0) {
            ROG_ERR("failed get libinput fd: %s", strerror(errno));
            goto end;
        }

    if ((rs->backend_fd = rs->backend->get_fd(rs->backend->d)) < 0) {
        ROG_ERR("failed to get backend fd");
        goto end;
    }

    if ((rs->wl_event_loop_fd = wl_event_loop_get_fd(rs->wl_event_loop)) < 0) {
        ROG_ERR("failed to get event loop fd");
        goto end;
    }

    if ((rs->ipc_fd = init_ipc()) < 0) {
        ROG_ERR("failed to open unix socket for ipc");
        goto end;
    }

    rs->pfds[RFD_LIBINPUT].fd = rs->li_fd;
    rs->pfds[RFD_LIBSEAT].fd  = rs->ls_fd;
    rs->pfds[RFD_SIGNALS].fd  = rs->sig_fd;
    rs->pfds[RFD_BACKEND].fd  = rs->backend_fd;
    rs->pfds[RFD_WAYLAND].fd  = rs->wl_event_loop_fd;
    rs->pfds[RFD_CURSOR].fd   = rs->cursor_hide_timer_fd;
    rs->pfds[RFD_TICK].fd     = rs->tick_timer_fd;
    rs->pfds[RFD_IPC].fd      = rs->ipc_fd;

    if (init_env_vars())
        goto end;
    if (init_auto_start_progs())
        goto end;
    if (init_red_binds(rs))
        goto end;
    if (start_script())
        goto end;

    ROG_INFO("Starting loop...");
    while (!rs->should_quit) {
        wl_display_flush_clients(rs->wl_display);
        rs->backend->flush_events(rs->backend->d);

        int nfds = __REDPFDS_SIZE + rs->ipc_clients.size;
        int ret  = poll(rs->pfds, nfds, -1);
        if (ret == -1) {
            ROG_ERR("poll fds error");
            goto end;
        }

        if (rs->pfds[RFD_BACKEND].revents & POLLIN || rs->is_wayland_client)
            rs->backend->handle_events(rs->backend->d);

        for (size_t i = 0; i < rs->ipc_clients.size; i++) {
            if (rs->pfds[__REDPFDS_SIZE + i].revents &
                (POLLIN | POLLERR | POLLHUP)) {
                ipc_proccess_client_msg(rs, rs->pfds[__REDPFDS_SIZE + i].fd);
            }
        }

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
                case RFD_LIBSEAT: {
                    if (libseat_dispatch(rs->ls, 0) == -1) {
                        ROG_ERR("failed to dispatch libseat: %s",
                                strerror(errno));
                    }
                    break;
                }

                case RFD_SIGNALS: {
                    if (handle_signal(rs) == -1)
                        goto end;
                    // instantly check to prevent looping
                    if (rs->should_quit == 1)
                        goto quit;
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

                case RFD_IPC: {
                    if (ipc_accept_conn(rs))
                        ROG_ERR("falied to accept ipc connection: %s",
                                strerror(errno));
                    break;
                }

                case RFD_BIND_REPEATER: {
                    {
                        uint64_t expirations;
                        int      n = read(rs->bind_repeater_fd,
                                     &expirations,
                                     sizeof(expirations));
                        if (n != sizeof(expirations))
                            continue;
                    }

                    assert(rs->repeat_action);
                    exec_action(rs, rs->repeat_action, rs->repeat_action_len);
                    break;
                }

                case RFD_AUTOSCROLL: {
                    {
                        uint64_t expirations;
                        int      n = read(
                          rs->autoscroll_fd, &expirations, sizeof(expirations));
                        if (n != sizeof(expirations))
                            continue;
                    }

                    if (rs->autoscroll_delay_changed) {
                        red_autoscroll_update_timer(rs, rs->autoscroll_delay);
                        rs->autoscroll_delay_changed = 0;
                    }

                    input_pointer_scroll(rs,
                                         time_get_now_msec(),
                                         LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                                         LIBINPUT_POINTER_AXIS_SOURCE_WHEEL,
                                         (rs->autoscroll_direction) ? -1 : 1,
                                         0);
                    break;
                }

                case __REDPFDS_SIZE:
                case __REDPFDS_NONE:
                    break;
            }
        } while (revent_pfd != __REDPFDS_NONE);

        if (rs->ipc_red_state_changes) {
            if (ipc_send_state_changes(rs))
                ROG_ERR("ipc err while sending back state changes");
            rs->ipc_red_state_changes = 0;
        }
    }

    wl_display_flush_clients(rs->wl_display);

quit:
    ret = 0;
end:
    ret *= -1;
    ROG_WARN("Closing..");

    if (rs->li)
        libinput_unref(rs->li);
    if (rs->ls)
        vt_stop(rs);
    if (rs->sig_fd != -1)
        close(rs->sig_fd);

    free(rs->time_start);

    dll_for_each(rs->seat_devices, v) free(v->val);

    wl_display_destroy_clients(rs->wl_display);
    wl_display_destroy(rs->wl_display);
    destroy_xkb(rs);
    dll_destroy(rs->rcs);
    dll_destroy(rs->rts);
    dll_destroy(rs->layer_rsurfs);
    dll_destroy(rs->dds);
    dll_destroy(rs->ipc_clients);
    dll_destroy(rs->seat_devices);
    rs->backend->destroy(rs->backend->d);
    free(rs);

    ROG_PRINT_CLOSE();
    return ret;
}
