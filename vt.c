#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "log.h"

int
vt_set_mode(int fd, struct vt_mode mode)
{
    if (ioctl(fd, VT_SETMODE, &mode) == -1) {
        ROG_ERR("failed setting vt mode for %s: %s",
                mode.relsig != 0 ? "enable" : "disable",
                strerror(errno));
        return -1;
    }
    return 0;
}

int
vt_start(int fd)
{
    if (vt_set_mode(fd,
                    (struct vt_mode){
                      .mode   = VT_PROCESS,
                      .waitv  = 0,
                      .relsig = SIGUSR1,
                      .acqsig = SIGUSR2,
                      .frsig  = 0,
                    }) == -1) {
        return -1;
    };

    if (ioctl(fd, KDSKBMODE, K_OFF) == -1) {
        ROG_ERR("failed settintg kd keyboard to off: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == -1) {
        ROG_ERR("failed setting kd mode to graphics: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
vt_stop(int fd)
{
    if (vt_set_mode(fd,
                    (struct vt_mode){
                      .mode   = VT_AUTO,
                      .waitv  = 0,
                      .relsig = 0,
                      .acqsig = 0,
                      .frsig  = 0,
                    }) == -1) {
        return -1;
    };

    if (ioctl(fd, KDSKBMODE, K_UNICODE) == -1) {
        ROG_ERR("failed settintg kd keyboard to unicode: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, KDSETMODE, KD_TEXT) == -1) {
        ROG_ERR("failed setting kd mode to text: %s", strerror(errno));
        return -1;
    }

    return 0;
}

int
init_vt()
{
    int tty_fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    if (tty_fd < 0) {
        ROG_ERR("open tty: %s", strerror(errno));
        return -1;
    }

    if (vt_start(tty_fd) == -1) {
        return -1;
    }
    return tty_fd;
}
