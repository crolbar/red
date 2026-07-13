#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

static int
li_open_restricted(const char* path, int flags, void* user_data)
{
    int fd = open(path, flags);
    if (fd < 0) {
        perror("open");
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
    .open_restricted = li_open_restricted,
    .close_restricted = li_close_restricted,
};

struct libinput*
init_input()
{
    struct libinput* li;
    struct udev* udev;

    udev = udev_new();
    li = libinput_udev_create_context(&li_interface, NULL, udev);
    libinput_udev_assign_seat(li, "seat0");
    libinput_dispatch(li);

    return li;
};

int
input_check_close(struct libinput* li, int tty_fd)
{
    struct libinput_event* event;
    libinput_dispatch(li);
    while ((event = libinput_get_event(li)) != NULL) {

        enum libinput_event_type event_type = libinput_event_get_type(event);

        if (event_type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            struct libinput_event_keyboard* kbe =
              libinput_event_get_keyboard_event(event);

            uint32_t key = libinput_event_keyboard_get_key(kbe);
            int press = libinput_event_keyboard_get_key_state(kbe);

            // q
            if (key == 16 && !press) {
                printf("q pressed. quiting..\n");
                return 1;
            }

            // TODO CTRL+ALT+FN
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
            // printf("key: %d (%d)\n", key, press);
        }

        libinput_event_destroy(event);
        libinput_dispatch(li);
    }
    return 0;
}
