#include "log.h"
#include "red.h"
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <string.h>
#include <unistd.h>

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

int
input_check_close(struct redstate* rs)
{
    struct libinput_event* event;
    libinput_dispatch(rs->li);
    int tty_fd = rs->tty_fd;

    while ((event = libinput_get_event(rs->li)) != NULL) {
        enum libinput_event_type event_type = libinput_event_get_type(event);

        if (event_type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            struct libinput_event_keyboard* kbe =
              libinput_event_get_keyboard_event(event);

            uint32_t key   = libinput_event_keyboard_get_key(kbe);
            int      press = libinput_event_keyboard_get_key_state(kbe);

            if (key == KEY_Q && !press) {
                ROG_INFO("detected 'Q'");
                rs->should_quit = 1;
            }

            // TODO CTRL+ALT+FN
            if (!rs->is_wayland_client) {
                if (key == KEY_F1 && !press) {
                    if (ioctl(tty_fd, VT_ACTIVATE, 1) == -1) {
                        return -1;
                    }
                }
                if (key == KEY_F2 && !press) {
                    if (ioctl(tty_fd, VT_ACTIVATE, 2) == -1) {
                        return -1;
                    }
                }
                if (key == KEY_F3 && !press) {
                    if (ioctl(tty_fd, VT_ACTIVATE, 3) == -1) {
                        return -1;
                    }
                }
            }
            // ROG("key: %d (%d)", key, press);
        }

        if (event_type == LIBINPUT_EVENT_POINTER_MOTION) {
            struct libinput_event_pointer* me =
              libinput_event_get_pointer_event(event);

            double dx = libinput_event_pointer_get_dx_unaccelerated(me);
            double dy = libinput_event_pointer_get_dy_unaccelerated(me);

            rs->rect_x += dx * 0.4;
            rs->rect_y += dy * 0.4;

            int width  = rs->backend->get_width(rs->backend->d);
            int height = rs->backend->get_height(rs->backend->d);
            if (rs->rect_x > width)
                rs->rect_x = width;
            if (rs->rect_x < 0)
                rs->rect_x = 0;

            if (rs->rect_y > height)
                rs->rect_y = height;
            if (rs->rect_y < 0)
                rs->rect_y = 0;
        }

        libinput_event_destroy(event);
        libinput_dispatch(rs->li);
    }
    return 0;
}
