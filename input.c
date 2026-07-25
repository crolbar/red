#include "actions.h"
#include "compositor.h"
#include "config.h"
#include "log.h"
#include "red.h"
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

int
xkb_init_keyboard(struct redstate* rs)
{

    struct xkb_keymap*    keymap;
    struct xkb_rule_names names = {
        .rules   = cfg.xkb_rules,
        .model   = cfg.xkb_model,
        .layout  = cfg.xkb_layout,
        .variant = cfg.xkb_variant,
        .options = cfg.xkb_options,
    };

    keymap = xkb_keymap_new_from_names2(
      rs->xkb, &names, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        ROG_ERR("failed to create xkb keymap");
        return 1;
    }

    char* keymap_string =
      xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t keymap_size = strlen(keymap_string) + 1;
    if (keymap_size <= 0) {
        ROG_ERR("failed to create xkb keymap string");
        return 1;
    }

    char name[64];
    snprintf(name, sizeof(name), "/wl_red_xkb_keymap-%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
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

    struct xkb_state* state = xkb_state_new(keymap);
    if (!state) {
        ROG_ERR("xkb failed to create state from kemap");
        close(fd);
        return 1;
    }

    xkb_keymap_unref(keymap);

    rs->xkb_keymap_size   = keymap_size;
    rs->xkb_keymap_string = keymap_string;
    rs->xkb_keymap_fd     = fd;
    rs->xkb_state         = state;
    rs->xkb_keymap        = keymap;

    return 0;
}

int
xkb_destroy(struct redstate* rs)
{
    if (rs->xkb_keymap_fd != -1)
        close(rs->xkb_keymap_fd);
    if (rs->xkb_keymap_string)
        free(rs->xkb_keymap_string);
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

// return 0 on no bind pressed, 1 on pressed
// if 1 returned the keypress should not be send to client
int
input_handle_binds(struct redstate* rs, char* key_str, int press)
{
    for (size_t i = 0; i < cfg.binds_len; i++) {
        redbind bind = cfg.binds[i];

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

        // we don't process release event
        if (!press)
            return 0;

        exec_action(rs, bind.action, bind.action_len);
        goto press;
    }

    return 0;
press:
    return 1;
}

int
input_kb_key(struct redstate* rs, struct libinput_event_keyboard* kbe)
{
    uint32_t      evdev_key    = libinput_event_keyboard_get_key(kbe);
    int           evdev_press  = libinput_event_keyboard_get_key_state(kbe);
    xkb_keycode_t xkb_key      = evdev_key + 8;
    enum xkb_key_direction dir = (evdev_press) ? XKB_KEY_DOWN : XKB_KEY_UP;

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

    if (red_kb_send_keys(rs, kbe, evdev_key, evdev_press, mods_have_changed))
        return 1;

    return 0;
}

int
input_pointer(struct redstate*               rs,
              enum libinput_event_type       event_type,
              struct libinput_event_pointer* pe)
{
    if (event_type == LIBINPUT_EVENT_POINTER_BUTTON)
        return red_pointer_send_button(rs, pe);

    if (event_type == LIBINPUT_EVENT_POINTER_MOTION)
        return red_pointer_send_motion(rs, pe);

    if (event_type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL ||
        event_type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER)
        return red_pointer_send_scroll(
          rs, pe, event_type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER);

    return 0;
}

int
input_dispatch(struct redstate* rs)
{
    libinput_dispatch(rs->li);

    // if (rs->is_wayland_client || getenv("RED_DONT_SPAWN_CLIENT"))
    //     return 0;

    struct libinput_event* event;
    while ((event = libinput_get_event(rs->li)) != NULL) {
        enum libinput_event_type event_type = libinput_event_get_type(event);

        if (event_type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            struct libinput_event_keyboard* kbe =
              libinput_event_get_keyboard_event(event);

            if (input_kb_key(rs, kbe))
                goto fail;
        } else if (event_type == LIBINPUT_EVENT_POINTER_MOTION ||
                   event_type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL ||
                   event_type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER ||
                   event_type == LIBINPUT_EVENT_POINTER_BUTTON) {
            struct libinput_event_pointer* pe =
              libinput_event_get_pointer_event(event);

            if (input_pointer(rs, event_type, pe))
                goto fail;
        }

        else if (event_type == LIBINPUT_EVENT_DEVICE_ADDED) {
            struct libinput_device* device = libinput_event_get_device(event);

            // enable finger tap
            if (libinput_device_config_tap_get_finger_count(device) > 0) {
                libinput_device_config_tap_set_enabled(
                  device, LIBINPUT_CONFIG_TAP_ENABLED);
            }
            break;
        }

        libinput_event_destroy(event);
        libinput_dispatch(rs->li);
    }

    return 0;
fail:
    return 1;
}
