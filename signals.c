#include "log.h"
#include "red.h"
#include <errno.h> // IWYU pragma: keep
#include <signal.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

void
crash_handle(int _s)
{
    signal(_s, SIG_DFL);

    ROG_WARN("crash with signal: %d detected", _s);

    char* msg = "\ncrash signal recived, stopping server. logs are available "
                "in ~/.local/state/red\n";
    int __attribute__((unused)) n = write(2, msg, strlen(msg));

    _exit(1);
}

int
init_signals()
{
    // reap zombie children
    {
        struct sigaction sa;
        sa.sa_handler = SIG_DFL;
        sa.sa_flags   = SA_NOCLDWAIT;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGCHLD, &sa, NULL);
    }

    signal(SIGSEGV, crash_handle);
    signal(SIGABRT, crash_handle);

    sigset_t mask;

    sigemptyset(&mask);

    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGPIPE);

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
        case SIGTERM:
            ROG_INFO("signal: %d recived, quitting server", si.ssi_signo);
            rs->should_quit = 1;
            break;
    }

    return 0;
}
