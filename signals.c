#include <assert.h>
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <libinput.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>

#include "backend-drm.h"
#include "log.h"
#include "red.h"

int
init_signals()
{
    sigset_t mask;

    sigemptyset(&mask);

    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        ROG_ERR("sigprocmask: %s", strerror(errno));
        return -1;
    }

    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (signal_fd == -1) {
        ROG_ERR("signalfd: %s", strerror(errno));
        return -1;
    }

    return signal_fd;
}

int
handle_signal(struct redstate* rs)
{
    struct backend_drm* bd;
    // TODO: better later
    if (!rs->is_wayland_client) {
        bd = rs->backend->d;
        if (!bd->page_flip_ready) {
            bd->stop_flipping = true;
            return 0;
        }
    }

    struct signalfd_siginfo si;
    ssize_t                 n = read(rs->sig_fd, &si, sizeof(si));

    if (n != sizeof(si)) {
        perror("read signalfd");
        return -1;
    }

    switch (si.ssi_signo) {
        case SIGUSR1:
            assert(!rs->is_wayland_client);
            ROG_INFO("Releasing drm_master and vt_display");
            bd = rs->backend->d;

            if (drmDropMaster(bd->drm_fd) == -1) {
                ROG_ERR("Could not drop master: %s", strerror(errno));
                return -1;
            }

            if (ioctl(rs->tty_fd, VT_RELDISP, 1) == -1) {
                ROG_ERR("Could not ack VT release: %s", strerror(errno));
                return -1;
            }

            libinput_suspend(rs->li);
            // after resume we should get only device_add events
            struct libinput_event* ev;
            while ((ev = libinput_get_event(rs->li))) {
                libinput_event_destroy(ev);
            }

            rs->active = 0;
            break;

        case SIGUSR2:
            assert(!rs->is_wayland_client);
            ROG_INFO("Acquiring drm_master and vt_display");
            bd = rs->backend->d;

            if (drmSetMaster(bd->drm_fd) == -1) {
                ROG_ERR("Could not set master: %s", strerror(errno));
                return -1;
            }

            if (ioctl(rs->tty_fd, VT_RELDISP, VT_ACKACQ) == -1) {
                ROG_ERR("Could not ack VT acquire: %s", strerror(errno));
                return -1;
            }

            if (libinput_resume(rs->li)) {
                ROG_ERR("Could not resume libinput context");
                return -1;
            }

            bd->stop_flipping = false;
            rs->active        = 1;
            break;

        case SIGINT:
            ROG_INFO("recived int");
            break;

        case SIGTERM:
            ROG_INFO("recived term");
            break;
    }

    return 0;
}
