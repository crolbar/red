#include <errno.h> // IWYU pragma: keep
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "log.h"
#include "red.h"

int
init_signals()
{
    // reap zombie children
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sa.sa_flags   = SA_NOCLDWAIT;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);

    sigset_t mask;

    sigemptyset(&mask);

    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
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
    struct signalfd_siginfo si = { 0 };
    ssize_t                 n  = read(rs->sig_fd, &si, sizeof(si));

    if (n != sizeof(si)) {
        ROG_ERR("read signalfd: %s", strerror(errno));
        return -1;
    }

    switch (si.ssi_signo) {
        case SIGINT:
            ROG_INFO("recived int");
            break;

        case SIGTERM:
            ROG_INFO("recived term");
            break;
    }

    return 0;
}
