#include "actions.h"
#include "compositor.h"
#include "dll.h"
#include "limits.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "xdg-shell-server-protocol.h"
#include <errno.h> // IWYU pragma: keep
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
    dll_for_each(rs->rts, v)
    {
        if (rs->focused_rt == v->val) {
            if (v->next) {
                red_focus_rt(rs, v->next->val);
                break;
            }
        }
    }
}

void
redaction_focus_prev(struct redstate* rs, char** args, size_t args_len)
{
    dll_for_each(rs->rts, v)
    {
        if (rs->focused_rt == v->val) {
            if (v->prev) {
                red_focus_rt(rs, v->prev->val);
                break;
            }
        }
    }
}

void
redaction_focus_last(struct redstate* rs, char** args, size_t args_len)
{
    if (rs->last_focused_rt)
        red_focus_rt(rs, rs->last_focused_rt);
}

void
redaction_focus_n(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1)
        return;
    char* end;
    int   n = (int)strtol(args[0], &end, 10);
    if (end == args[0] || *end != '\0' || n < INT_MIN || n > INT_MAX || n < 0) {
        ROG_ERR("invalid number in red_action_focus_n: %s", args[0]);
        return;
    }

    if (n > (int)rs->rts.size) {
        return;
    }

    int i = 0;
    dll_for_each(rs->rts, v)
    {
        if (i++ != n)
            continue;

        red_focus_rt(rs, v->val);
    }
}

void
redaction_close(struct redstate* rs, char** args, size_t args_len)
{
    if (!rs->focused_rt || !rs->focused_rt->rsurf ||
        !rs->focused_rt->rsurf->xdg_toplevel)
        return;
    xdg_toplevel_send_close(rs->focused_rt->rsurf->xdg_toplevel);
}

void
redaction_stop_renderer(struct redstate* rs, char** args, size_t args_len)
{
    if (rs->should_draw) {
        rs->should_draw = 2;
        request_redraw(rs);
    } else {
        rs->should_draw = 1;

        if (rs->focused_rt)
            if (rs->backend->is_ready_for_frame(rs->backend->d))
                red_rt_send_enter(rs, rs->focused_rt);
        request_redraw(rs);
    }
}

int
spawn_program(char** args, size_t args_len)
{
    pid_t pid = fork();

    if (pid == 0) {
        setsid();

        // stop output from spawned program
        int fd = open("/dev/null", O_WRONLY);
        if (fd == -1) {
            ROG_ERR("open: %s", strerror(errno));
            return 1;
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);

        execvp(args[0], args);

        _exit(1);
    }

    return 0;
}

void
redaction_spawn(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1) {
        ROG_ERR("called spawn action without argument");
        return;
    }

    spawn_program(args, args_len);
}

void
redaction_debug(struct redstate* rs, char** args, size_t args_len)
{
    ROG("")
    ROG("focus: %s", rs->focused_rt->app_id);
    ROG("tops:");
    dll_for_each(rs->rts, v) ROG("  top: %s", v->val->app_id)
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
    { .action_type = RED_ACTION_CLOSE, redaction_close },
    { .action_type = RED_ACTION_STOP_RENDERER, redaction_stop_renderer },
    { .action_type = RED_ACTION_FOCUS_LAST, redaction_focus_last },
    { .action_type = RED_ACTION_FOCUS_N, redaction_focus_n },
)
// clang-format on

void
exec_action(struct redstate* rs, char** action, size_t action_len)
{
    if (action_len < 1) {
        ROG_ERR("bind has no assigned action");
        return;
    }

    for (size_t i = 0; i < redactions_len; i++) {
        if (action[0] != redactions[i].action_type)
            continue;
        // found action, now exec it
        redactions[i].f(rs, ++action, --action_len);
        return;
    }

    ROG_ERR("did not found action %s", action[0]);
}
