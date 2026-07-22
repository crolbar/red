#include "actions.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

void
redaction_quit(struct redstate* rs, char** args, size_t args_len)
{
    rs->should_quit = true;
}

void
redaction_focus_next(struct redstate* rs, char** args, size_t args_len)
{
    dll_for_each(rs->trcs, v)
    {
        if (rs->focused_trc == v->val) {
            if (v->next) {
                rs->focused_trc = v->next->val;
                request_redraw(rs);
                break;
            }
        }
    }
}

void
redaction_focus_prev(struct redstate* rs, char** args, size_t args_len)
{
    dll_for_each(rs->trcs, v)
    {
        if (rs->focused_trc == v->val) {
            if (v->prev) {
                rs->focused_trc = v->prev->val;
                request_redraw(rs);
                break;
            }
        }
    }
}

void
redaction_spawn(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1) {
        ROG_ERR("called spawn action without argument");
        return;
    }

    pid_t pid = fork();

    if (pid == 0) {
        setsid();

        // stop output from spawned program
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1) {
            ROG_ERR("open: %s", strerror(errno));
            return;
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        execvp(args[0], args);

        _exit(1);
    }
}

void
redaction_debug(struct redstate* rs, char** args, size_t args_len)
{
    ROG("tops: \n");
    dll_for_each(rs->trcs, v)
      ROG("  top: %s %d", v->val->rsurf->app_id, v->val->wl_keyboard)
}

#define ACTIONS(...)                                                           \
    redaction* redactions     = (redaction[]){ (redaction)__VA_ARGS__ };       \
    size_t     redactions_len = (sizeof((redaction[]){ __VA_ARGS__ }) /        \
                             sizeof(((redaction[]){ __VA_ARGS__ })[0]));

// clang-format off
ACTIONS(
    { .action_type = RED_ACTION_QUIT, redaction_quit },
    { .action_type = RED_ACTION_DEBUG, redaction_debug },
    { .action_type = RED_ACTION_FOCUS_PREV, redaction_focus_prev },
    { .action_type = RED_ACTION_FOCUS_NEXT, redaction_focus_next },
    { .action_type = RED_ACTION_SPAWN, redaction_spawn },
)
// clang-format on

void
exec_action(struct redstate* rs, char** action, size_t action_len)
{
    if (action_len < 1) {
        ROG_ERR("bind has no assigned action");
        return;
    }

    for (int i = 0; i < redactions_len; i++) {
        if (action[0] != redactions[i].action_type)
            continue;
        // found action, now exec it
        redactions[i].f(rs, ++action, --action_len);
        return;
    }

    ROG_ERR("did not found action %s", action[0]);
}
