#include "actions.h"
#include "compositor.h"
#include "config.h"
#include "dll.h"
#include "limits.h"
#include "log.h"
#include "red.h"
#include "render.h"
#include "utils.h"
#include "wayland.h"
#include "xdg-shell-server-protocol.h"
#include <errno.h> // IWYU pragma: keep
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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
        break;
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
redaction_select_bind_preset(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1)
        return;
    char* end;
    int   n = (int)strtol(args[0], &end, 10);
    if (end == args[0] || *end != '\0' || n < INT_MIN || n > INT_MAX || n < 0) {
        ROG_ERR("invalid number in redaction_select_bind_preset: %s", args[0]);
        return;
    }
    if ((uint32_t)n > cfg.bind_presets_len) {
        ROG_ERR("number in redaction_select_bind_preset: %s, bigger than bind "
                "prsets: %d",
                args[0],
                cfg.bind_presets_len);
        return;
    }

    cfg.sel_bind_preset = n;
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

void
redaction_overlay_surface(struct redstate* rs, char** args, size_t args_len)
{
    uint32_t w = rs->backend->get_width(rs->backend->d);
    uint32_t h = rs->backend->get_height(rs->backend->d);

    // initial values of the overlay surface
    if (rs->overlay_rt_h == 0 && rs->overlay_rt_w == 0) {
        rs->overlay_rt_x = 150;
        rs->overlay_rt_y = 150;
        rs->overlay_rt_w = w / 3;
        rs->overlay_rt_h = h / 3;
    }

    // set old overlay_rt to normal dimentions
    if (rs->overlay_rt && rs->overlay_rt->rsurf) {
        rs->overlay_rt->rsurf->x = 0;
        rs->overlay_rt->rsurf->y = 0;
        red_send_toplevel_configure(
          rs->overlay_rt->rsurf, rs->overlay_rt_w, rs->overlay_rt_h, 0, 1);
    }

    if (rs->overlay_rt == rs->focused_rt) {
        rs->overlay_rt = NULL;
        return;
    }

    rs->overlay_rt = rs->focused_rt;
}

void
redaction_overlay_set_width(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1) {
        ROG_ERR("action: no argument provided to overlay_set_width");
        return;
    }

    int n = 0;
    UTIL_PARSE_INT(args[0], n);
    if (_err) {
        ROG_ERR("action: invalid argument provided to overlay_set_width: %s",
                args[0]);
        return;
    }

    rs->overlay_rt_w += n;
    rs->overlay_rt_w = max(rs->overlay_rt_w, 0);
    if (rs->overlay_rt && rs->overlay_rt->rsurf) {
        red_send_toplevel_configure(
          rs->overlay_rt->rsurf, rs->overlay_rt_w, rs->overlay_rt_h, 0, 1);
    }
}
void
redaction_overlay_set_height(struct redstate* rs, char** args, size_t args_len)
{

    if (args_len < 1) {
        ROG_ERR("action: no argument provided to overlay_set_height");
        return;
    }

    int n = 0;
    UTIL_PARSE_INT(args[0], n);
    if (_err) {
        ROG_ERR("action: invalid argument provided to overlay_set_height: %s",
                args[0]);
        return;
    }

    rs->overlay_rt_h += n;
    rs->overlay_rt_h = max(rs->overlay_rt_h, 0);
    if (rs->overlay_rt && rs->overlay_rt->rsurf) {
        red_send_toplevel_configure(
          rs->overlay_rt->rsurf, rs->overlay_rt_w, rs->overlay_rt_h, 0, 1);
    }
}
void
redaction_overlay_set_x(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1) {
        ROG_ERR("action: no argument provided to overlay_set_x");
        return;
    }

    int n = 0;
    UTIL_PARSE_INT(args[0], n);
    if (_err) {
        ROG_ERR("action: invalid argument provided to overlay_set_x: %s",
                args[0]);
        return;
    }

    rs->overlay_rt_x += n;
    rs->overlay_rt_x = max(rs->overlay_rt_x, 0);
    if (rs->overlay_rt && rs->overlay_rt->rsurf) {
        rs->overlay_rt->rsurf->x = rs->overlay_rt_x;
        request_redraw(rs);
    }
}
void
redaction_overlay_set_y(struct redstate* rs, char** args, size_t args_len)
{
    if (args_len < 1) {
        ROG_ERR("action: no argument provided to overlay_set_y");
        return;
    }

    int n = 0;
    UTIL_PARSE_INT(args[0], n);
    if (_err) {
        ROG_ERR("action: invalid argument provided to overlay_set_y: %s",
                args[0]);
        return;
    }

    rs->overlay_rt_y += n;
    rs->overlay_rt_y = max(rs->overlay_rt_y, 0);
    if (rs->overlay_rt && rs->overlay_rt->rsurf) {
        rs->overlay_rt->rsurf->y = rs->overlay_rt_y;
        request_redraw(rs);
    }
}

void
redaction_capture_focus(struct redstate* rs, char** args, size_t args_len)
{
    if (red_capture_focused_toplevel(rs))
        ROG_ERR("error while trying to capture focused toplevel");
}

void
redaction_rt_fi_update(struct redstate* rs, char** args, size_t args_len)
{
    char* fmt    = "/tmp/red-%s-%d.ppm";
    int   rt_idx = 0;
    dll_for_each(rs->rts, v)
    {
        if (!v->val->rsurf)
            continue;

        struct redtoplevel* rt     = v->val;
        char*               app_id = rt->app_id;

        if (rt->fi_path)
            free(rt->fi_path);

        int   l       = snprintf(NULL, 0, fmt, app_id, rt_idx);
        char* fi_path = calloc(1, l + 1);
        if (!fi_path)
            goto loop_end;
        sprintf(fi_path, fmt, app_id, rt_idx);
        fi_path[l] = '\0';

        if (red_capture_rsurf_to(rt->rsurf, fi_path, 640, 360)) {
            ROG_ERR("failed to capture rt: %s", app_id);
            goto loop_end;
        }

        rt->fi_path = fi_path;

    loop_end:
        rt_idx++;
    }
    rs->ipc_red_state_changes |= RED_STATE_RT_FI;
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
    { .action_type = RED_ACTION_SELECT_BIND_PRESET, redaction_select_bind_preset },
    { .action_type = RED_ACTION_OVERLAY_SURFACE, redaction_overlay_surface },
    { .action_type = RED_ACTION_OVERLAY_SET_WIDTH, redaction_overlay_set_width },
    { .action_type = RED_ACTION_OVERLAY_SET_HEIGHT, redaction_overlay_set_height },
    { .action_type = RED_ACTION_OVERLAY_SET_X, redaction_overlay_set_x },
    { .action_type = RED_ACTION_OVERLAY_SET_Y, redaction_overlay_set_y },
    { .action_type = RED_ACTION_CAPTURE_FOCUS, redaction_capture_focus },
    { .action_type = RED_ACTION_RT_FI_UPDATE, redaction_rt_fi_update },
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
        // TODO: remove this, make action_type a numebr not a string
        if (strcmp(action[0], redactions[i].action_type) != 0)
            continue;
        // found action, now exec it
        redactions[i].f(rs, ++action, --action_len);
        return;
    }

    ROG_ERR("did not found action %s", action[0]);
}
