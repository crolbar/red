#include "actions.h"
#include "compositor.h"
#include "config.h"
#include "log.h"
#include "red.h"
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <libinput.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

int
init_xkb_keyboard(struct redstate* rs)
{
    struct xkb_keymap* red_keymap = NULL;
    struct xkb_state*  state      = NULL;
    {
        struct xkb_rule_names names = {
            .rules   = cfg.xkb_rules,
            .model   = cfg.xkb_model,
            .layout  = cfg.xkb_layout,
            .variant = cfg.xkb_variant,
            .options = cfg.xkb_options,
        };

        red_keymap = xkb_keymap_new_from_names2(rs->xkb,
                                                &names,
                                                XKB_KEYMAP_FORMAT_TEXT_V1,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!red_keymap) {
            ROG_ERR("failed to create xkb keymap");
            return 1;
        }

        state = xkb_state_new(red_keymap);
        if (!state) {
            ROG_ERR("xkb failed to create state from kemap");
            return 1;
        }
    }

    char*  keymap_string = NULL;
    size_t keymap_size   = 0;
    int    fd            = -1;

    {
        struct xkb_rule_names names = {
            .rules   = cfg.xkb_rules,
            .model   = cfg.xkb_model,
            .layout  = cfg.xkb_layout,
            .variant = "", // TODO fix variants
            .options = "",
        };

        struct xkb_keymap* keymap =
          xkb_keymap_new_from_names2(rs->xkb,
                                     &names,
                                     XKB_KEYMAP_FORMAT_TEXT_V1,
                                     XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) {
            ROG_ERR("failed to create xkb keymap");
            return 1;
        }

        keymap_string =
          xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
        keymap_size = strlen(keymap_string) + 1;
        if (keymap_size <= 0) {
            ROG_ERR("failed to create xkb keymap string");
            return 1;
        }

        char name[64];
        snprintf(name, sizeof(name), "/wl_red_xkb_keymap-%d", getpid());
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        shm_unlink(name);
        if (fd < 0) {
            ROG_ERR(" xkb keymap failed creating shm");
            return 1;
        }

        if (ftruncate(fd, keymap_size) == -1) {
            ROG_ERR("xkb keymap fd shm setting size failed");
            close(fd);
            return 1;
        }
        void* ptr =
          mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            ROG_ERR("xkb keymap fd shm failed to mnap failed");
            close(fd);
            return 1;
        }
        memcpy(ptr, keymap_string, keymap_size);

        xkb_keymap_unref(keymap);
    }

    rs->xkb_keymap_size   = keymap_size;
    rs->xkb_keymap_string = keymap_string;
    rs->xkb_keymap_fd     = fd;
    rs->xkb_state         = state;
    rs->xkb_keymap        = red_keymap;

    return 0;
}

int
destroy_xkb(struct redstate* rs)
{
    if (rs->xkb_keymap_fd != -1)
        close(rs->xkb_keymap_fd);
    if (rs->xkb_keymap_string)
        free(rs->xkb_keymap_string);
    xkb_state_unref(rs->xkb_state);
    xkb_keymap_unref(rs->xkb_keymap);
    xkb_context_unref(rs->xkb);
    return 0;
}

static int
li_open_restricted(const char* path, int flags, void* user_data)
{
    int fd = open(path, flags);
    if (fd < 0) {
        ROG_ERR("open err: %s", strerror(errno));
        return -1;
    }
    return fd;
}

static void
li_close_restricted(int fd, void* user_data)
{
    close(fd);
}

static struct libinput_interface li_interface = {
    .open_restricted  = li_open_restricted,
    .close_restricted = li_close_restricted,
};

struct libinput*
init_input()
{
    struct libinput* li;
    struct udev*     udev;

    udev = udev_new();
    if (!udev) {
        ROG_ERR("failed to create udev");
        return NULL;
    }
    li = libinput_udev_create_context(&li_interface, NULL, udev);
    if (!li) {
        return NULL;
    }
    udev_unref(udev);
    if (libinput_udev_assign_seat(li, "seat0") == -1) {
        return NULL;
    }
    if (libinput_dispatch(li)) {
        ROG_ERR("%s", strerror(errno));
        return NULL;
    }

    return li;
};

// return 0 on no press, 1 on press
int
input_handle_internal(struct redstate* rs, char* key_str, int press)
{
    if (rs->is_wayland_client)
        return 0;

    // currently only vt switching is handled here

    int tty_fd = rs->tty_fd;
    if (strcmp(key_str, "XF86Switch_VT_1") == 0) {
        // only on release
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 1) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_2") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 2) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_3") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 3) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_4") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 4) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_5") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 5) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_6") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 6) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_7") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 7) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_8") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 8) == -1) {
            return -1;
        }
        return 1;
    } else if (strcmp(key_str, "XF86Switch_VT_9") == 0) {
        if (press)
            return 1;
        if (ioctl(tty_fd, VT_ACTIVATE, 9) == -1) {
            return -1;
        }
        return 1;
    }

    return 0;
}

inline void
red_stop_bind_repeater(struct redstate* rs)
{
    if (rs->bind_repeater_fd != -1)
        close(rs->bind_repeater_fd);
    rs->bind_repeater_fd                = -1;
    rs->repeat_action                   = NULL;
    rs->repeat_action_len               = 0;
    rs->pfds[RFD_BIND_REPEATER].fd      = -1;
    rs->pfds[RFD_BIND_REPEATER].revents = 0;
}

inline void
red_start_bind_repeater(struct redstate* rs)
{
    int32_t           delay = cfg.kb_repeat_delay;
    int32_t           rate  = cfg.kb_repeat_rate / 2;
    struct itimerspec its   = {
          .it_value    = { .tv_sec  = delay / 1000,
                           .tv_nsec = (delay % 1000) * 1000000 },
          .it_interval = { .tv_sec = 1 / rate, .tv_nsec = (1000000000L / rate) },
    };
    rs->bind_repeater_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    timerfd_settime(rs->bind_repeater_fd, 0, &its, NULL);
    rs->pfds[RFD_BIND_REPEATER].fd      = rs->bind_repeater_fd;
    rs->pfds[RFD_BIND_REPEATER].revents = 0;
}

// return 0 on no bind pressed, 1 on pressed
// if 1 returned the keypress should not be send to client
int
input_handle_binds(struct redstate* rs, char* key_str, int press)
{
    // stop repeated action on next release event
    if (!press && rs->repeat_action) {
        red_stop_bind_repeater(rs);
    }

    redbindpreset preset = cfg.bind_presets[cfg.sel_bind_preset];
    for (size_t i = 0; i < preset.binds_len; i++) {
        redbind bind = preset.binds[i];

        // key
        if (strcmp(bind.key, key_str) != 0)
            continue;

        // mods
        {
            if (bind.mods == RED_MOD_NO_MODS)
                if (rs->xkb_mods_depressed != 0)
                    continue;

            xkb_mod_mask_t ctrl_mask =
              xkb_keymap_mod_get_mask(rs->xkb_keymap, XKB_MOD_NAME_CTRL);
            // if bind.mods has ctl then mods_depressed should also have it
            if (!(((bind.mods & RED_MOD_CTRL) != 0) ==
                  ((rs->xkb_mods_depressed & ctrl_mask) != 0)))
                continue;

            xkb_mod_mask_t shift_mask =
              xkb_keymap_mod_get_mask(rs->xkb_keymap, XKB_MOD_NAME_SHIFT);
            if (!(((bind.mods & RED_MOD_SHIFT) != 0) ==
                  ((rs->xkb_mods_depressed & shift_mask) != 0)))
                continue;

            xkb_mod_mask_t alt_mask =
              xkb_keymap_mod_get_mask(rs->xkb_keymap, XKB_MOD_NAME_MOD1);
            if (!(((bind.mods & RED_MOD_ALT) != 0) ==
                  ((rs->xkb_mods_depressed & alt_mask) != 0)))
                continue;

            xkb_mod_mask_t super_mask =
              xkb_keymap_mod_get_mask(rs->xkb_keymap, XKB_MOD_NAME_MOD4);
            if (!(((bind.mods & RED_MOD_SUPER) != 0) ==
                  ((rs->xkb_mods_depressed & super_mask) != 0)))
                continue;
        }

        // only exec action on press
        if (!press) {
            red_stop_bind_repeater(rs);
            return 1;
        }

        exec_action(rs, bind.action, bind.action_len);
        if (rs->bind_repeater_fd != -1)
            close(rs->bind_repeater_fd);
        rs->repeat_action     = bind.action;
        rs->repeat_action_len = bind.action_len;
        red_start_bind_repeater(rs);
        goto press;
    }

    return 0;
press:
    return 1;
}

int
input_kb_key(struct redstate* rs,
             uint32_t         time_msec,
             uint32_t         evdev_key,
             int              evdev_press)
{
    xkb_keycode_t          xkb_key = evdev_key + 8;
    enum xkb_key_direction dir     = (evdev_press) ? XKB_KEY_DOWN : XKB_KEY_UP;

    xkb_state_update_key(rs->xkb_state, xkb_key, dir);

    xkb_mod_mask_t mods_depressed =
      xkb_state_serialize_mods(rs->xkb_state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t mods_latched =
      xkb_state_serialize_mods(rs->xkb_state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t mods_locked =
      xkb_state_serialize_mods(rs->xkb_state, XKB_STATE_MODS_LOCKED);
    xkb_layout_index_t group =
      xkb_state_serialize_layout(rs->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

    int mods_have_changed = 0;

    if (mods_depressed != rs->xkb_mods_depressed ||
        mods_latched != rs->xkb_mods_latched ||
        mods_locked != rs->xkb_mods_locked || group != rs->xkb_group) {
        mods_have_changed      = 1;
        rs->xkb_mods_depressed = mods_depressed;
        rs->xkb_mods_latched   = mods_latched;
        rs->xkb_mods_locked    = mods_locked;
        rs->xkb_group          = group;
    }

    xkb_keysym_t key = xkb_state_key_get_one_sym(rs->xkb_state, xkb_key);
    char         key_str[64];
    xkb_keysym_get_name(key, key_str, sizeof(key_str));

    if (input_handle_internal(rs, key_str, evdev_press))
        return 0;

    if (input_handle_binds(rs, key_str, evdev_press))
        return 0;

    // forward key press to client

    if (red_kb_send_keys(
          rs, time_msec, evdev_key, evdev_press, mods_have_changed))
        return 1;

    return 0;
}

int
input_pointer_button(struct redstate* rs,
                     uint32_t         time_msec,
                     uint32_t         button,
                     int              press)
{
    if (red_pointer_send_button(rs, time_msec, button, press))
        return 1;

    red_pointer_send_frame(rs);
    return 0;
}

int
input_pointer_motion(struct redstate* rs,
                     uint32_t         time_msec,
                     uint64_t         time_usec,
                     double           dx,
                     double           dy,
                     double           udx,
                     double           udy)
{

    if (!rs->cursor_locked) {
        uint32_t width  = rs->backend->get_width(rs->backend->d);
        uint32_t height = rs->backend->get_height(rs->backend->d);
        double   x      = rs->cursor_x + dx;
        double   y      = rs->cursor_y + dy;
        rs->cursor_x    = max(min(x, (double)width), 0);
        rs->cursor_y    = max(min(y, (double)height), 0);
    }

    if (red_pointer_send_relative_motion(rs, time_usec, dx, dy, udx, udy))
        return 1;

    if (!rs->cursor_locked) {
        if (red_pointer_send_motion(rs, time_msec))
            return 1;
        red_pointer_send_frame(rs);
    }
    return 0;
}

int
input_pointer_scroll(struct redstate*                  rs,
                     uint32_t                          time_msec,
                     enum libinput_pointer_axis        axis,
                     enum libinput_pointer_axis_source source,
                     double                            value,
                     double                            value120)
{
    rs->cursor_last_scroll_time = time_msec;
    if (red_pointer_send_scroll(rs, time_msec, axis, source, value, value120))
        return 1;
    red_pointer_send_frame(rs);
    return 0;
}

int
input_dispatch(struct redstate* rs)
{
    libinput_dispatch(rs->li);

    if (rs->is_wayland_client)
        return 0;

    struct libinput_event* event;
    while ((event = libinput_get_event(rs->li)) != NULL) {
        enum libinput_event_type event_type = libinput_event_get_type(event);

        switch (event_type) {
            case LIBINPUT_EVENT_DEVICE_ADDED: {
                struct libinput_device* device =
                  libinput_event_get_device(event);

                // enable finger tap
                if (libinput_device_config_tap_get_finger_count(device) > 0) {
                    libinput_device_config_tap_set_enabled(
                      device, LIBINPUT_CONFIG_TAP_ENABLED);
                }

                if (libinput_device_config_accel_is_available(device)) {
                    const char* name = libinput_device_get_name(device);
                    for (size_t i = 0; i < cfg.mouses_len; i++) {
                        if (strcmp(name, cfg.mouses[i].name) != 0 &&
                            strcmp("*", cfg.mouses[i].name) != 0) {
                            continue;
                        }

                        libinput_device_config_accel_set_profile(
                          device,
                          cfg.mouses[i].flat_profile
                            ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
                            : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);

                        libinput_device_config_accel_set_speed(
                          device, cfg.mouses[i].speed);
                        break;
                    }
                }
                break;
            }

            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                struct libinput_event_keyboard* kbe =
                  libinput_event_get_keyboard_event(event);

                uint32_t evdev_key = libinput_event_keyboard_get_key(kbe);
                int evdev_press    = libinput_event_keyboard_get_key_state(kbe);
                uint32_t time_msec = libinput_event_keyboard_get_time(kbe);
                if (input_kb_key(rs, time_msec, evdev_key, evdev_press))
                    goto fail;
                break;
            }

            case LIBINPUT_EVENT_POINTER_MOTION: {
                struct libinput_event_pointer* pe =
                  libinput_event_get_pointer_event(event);

                double   udx = libinput_event_pointer_get_dx_unaccelerated(pe);
                double   udy = libinput_event_pointer_get_dy_unaccelerated(pe);
                double   dx  = libinput_event_pointer_get_dx(pe);
                double   dy  = libinput_event_pointer_get_dy(pe);
                uint32_t time_msec = libinput_event_pointer_get_time(pe);
                uint64_t time_usec = libinput_event_pointer_get_time_usec(pe);
                if (input_pointer_motion(
                      rs, time_msec, time_usec, dx, dy, udx, udy))
                    goto fail;
                break;
            }

            case LIBINPUT_EVENT_POINTER_BUTTON: {
                struct libinput_event_pointer* pe =
                  libinput_event_get_pointer_event(event);

                uint32_t time_msec = libinput_event_pointer_get_time(pe);
                uint32_t button    = libinput_event_pointer_get_button(pe);
                int      press = libinput_event_pointer_get_button_state(pe) ==
                            LIBINPUT_BUTTON_STATE_PRESSED;

                if (input_pointer_button(rs, time_msec, button, press))
                    goto fail;
                break;
            }

            case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
            case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
                struct libinput_event_pointer* pe =
                  libinput_event_get_pointer_event(event);
                uint32_t time_msec = libinput_event_pointer_get_time(pe);

                enum libinput_pointer_axis_source source;
                switch (event_type) {
                    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
                        source = LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;
                        break;
                    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
                        source = LIBINPUT_POINTER_AXIS_SOURCE_FINGER;
                        break;
                    case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
                        source = LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS;
                        break;
                    default:
                        break;
                }

                enum libinput_pointer_axis axes[] = {
                    LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                    LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
                };

                for (int i = 0; i < 2; i++) {
                    enum libinput_pointer_axis axis = axes[i];
                    if (!libinput_event_pointer_has_axis(pe, axis))
                        continue;

                    double value =
                      libinput_event_pointer_get_scroll_value(pe, axis);

                    double value120 = 0;
                    if (event_type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)
                        value120 = libinput_event_pointer_get_scroll_value_v120(
                          pe, axis);

                    if (input_pointer_scroll(
                          rs, time_msec, axis, source, value, value120))
                        goto fail;
                }
                break;
            }
            default:
                break;
        }

        libinput_event_destroy(event);
    }

    return 0;
fail:
    return 1;
}
