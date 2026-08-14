#include <errno.h> // IWYU pragma: keep
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "actions.h"
#include "config.h"
#include "dll.h"
#include "ipc.h"
#include "log.h"
#include "red.h"

char*
handle_ipc_msg_fetch_toplevels(struct redstate* rs)
{
    // [,],\n
    size_t len = 3 + 1;
    char*  out = calloc(1, len);
    size_t i   = 0;
    out[i++]   = '[';

    int rt_idx = 0;
    dll_for_each(rs->rts, v)
    {
        // TODO: escape quotes?
        const char* app_id = v->val->app_id;
        const char* title  = v->val->title;
        const char* fmt    = "%s{"
                             "\"idx\":\"%d\","
                             "\"app_id\":\"%s\","
                             "\"title\":\"%s\","
                             "\"is_focused\":%s"
                             "}";

        int   n   = snprintf(NULL,
                         0,
                         fmt,
                         (rt_idx) ? "," : "",
                         rt_idx,
                         app_id,
                         title,
                         (v->val == rs->focused_rt) ? "true" : "false");
        char* str = calloc(1, n + 1);
        sprintf(str,
                fmt,
                (rt_idx) ? "," : "",
                rt_idx,
                app_id,
                title,
                (v->val == rs->focused_rt) ? "true" : "false");

        char* t = realloc(out, len + n);
        if (!t) {
            free(str);
            free(out);
            return NULL;
        }
        out = t;

        memcpy(out + i, str, n);
        i += n;
        len += n;

        rt_idx++;
    }

    // overwrite the nullterm
    out[i++] = ']';
    out[i++] = '\n';
    out[i]   = '\0';

    return out;
}

char*
handle_ipc_msg(struct redstate*     rs,
               struct redipcclient* ric,
               char**               msg,
               size_t               msg_len,
               int*                 should_free)
{
    assert(msg_len != 0);

    if (strcmp(msg[0], RED_IPC_MSG_CFG_CURSOR_AUTOHIDE_TIME) == 0) {
        if (msg_len < 2)
            goto found;

        char* end;
        long  n = strtol(msg[1], &end, 10);
        if (*end == '\0') {
            if (n < 0)
                goto found;
            cfg.cursor_autohide_time = n;
        }

        goto found;
    }

    if (strcmp(msg[0], RED_IPC_MSG_FETCH_TOPLEVELS) == 0) {
        *should_free = 1;
        return handle_ipc_msg_fetch_toplevels(rs);
    }
    if (strcmp(msg[0], RED_IPC_MSG_SUBSCRIBE) == 0) {
        ric->subscribed = 1;
        goto found;
    }

    for (size_t i = 0; i < redactions_len; i++) {
        if (strcmp(msg[0], redactions[i].action_type) == 0) {
            exec_action(rs, msg, msg_len);
            goto found;
        }
    }

    return "incorrect msg";
found:
    return "ok";
}

static void
ipc_destroy_client(struct redipcclient* ric)
{
    if (ric->fd != -1)
        close(ric->fd);
    free(ric);
}

static struct redipcclient*
_send_state_change_msg(struct redipcclient* ric, char* msg)
{
    int n = write(ric->fd, msg, strlen(msg));
    if (n <= 0) {
        return ric;
    }
    return NULL;
}

int
ipc_send_state_changes(struct redstate* rs)
{
    dll(struct redipcclient*) clients_to_destroy = dll_init();

    dll_for_each(rs->ipc_clients, v)
    {
        if (!v->val->subscribed)
            continue;

        if (rs->ipc_red_state_changes & RED_STATE_CHANGE_FOCUS) {
            if (_send_state_change_msg(v->val, "window focus changed\n"))
                dll_push_tail(clients_to_destroy, v->val);
        }
    }

    while (clients_to_destroy.size) {
        struct redipcclient* ric = dll_hpop(clients_to_destroy);
        dll_remove_val(rs->ipc_clients, ric);
        ipc_destroy_client(ric);
        ipc_update_pfds(rs);
    }
    return 0;
}

int
init_ipc()
{
    const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char* wayland_dis = getenv("WAYLAND_DISPLAY");
    const char* fmt         = "%s/red-%s";
    char*       ipc_path    = NULL;
    int         sock_fd     = -1;

    int n = snprintf(NULL, 0, fmt, runtime_dir, wayland_dis);
    if (n > 108) {
        ROG_ERR("path for unix sock too long");
        goto fail;
    }
    ipc_path = calloc(1, n + 1);
    sprintf(ipc_path, fmt, runtime_dir, wayland_dis);

    unlink(ipc_path);

    setenv("RED_SOCKET", ipc_path, 1);
    ROG_INFO("Listening for ipc on RED_SOCKET=%s", ipc_path);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        ROG_ERR("falied opening socked fd");
        goto fail;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, ipc_path);
    free(ipc_path);

    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        ROG_ERR("ipc bind failed");
        goto fail;
    }

    if (listen(sock_fd, RED_IPC_MAX_CLIENTS) == -1) {
        ROG_ERR("ipc listen failed: %s", strerror(errno));
        goto fail;
    }

    return sock_fd;
fail:
    if (sock_fd != -1)
        close(sock_fd);
    if (ipc_path)
        free(ipc_path);
    return -1;
}

int
ipc_update_pfds(struct redstate* rs)
{
    for (int i = 0; i < RED_IPC_MAX_CLIENTS; i++) {
        rs->pfds[__REDPFDS_SIZE + i].fd      = -1;
        rs->pfds[__REDPFDS_SIZE + i].events  = POLLIN;
        rs->pfds[__REDPFDS_SIZE + i].revents = 0;
    }

    while (rs->ipc_clients.size > RED_IPC_MAX_CLIENTS) {
        ipc_destroy_client(dll_hpop(rs->ipc_clients));
    }

    int i = 0;
    dll_for_each(rs->ipc_clients, v)
    {
        rs->pfds[__REDPFDS_SIZE + i].fd = v->val->fd;
        i++;
    }

#if RED_IPC_DEBUG_MSG_LOG == 1
    ROG("clients:")
    dll_for_each(rs->ipc_clients, v){ ROG("c: %d", v->val->fd) } ROG("")
#endif
      return 0;
}

int
ipc_accept_conn(struct redstate* rs)
{
    int client_fd = accept(rs->ipc_fd, NULL, NULL);
    if (client_fd < 0) {
        return 1;
    }

    struct redipcclient* ric = calloc(1, sizeof(*ric));
    if (!ric)
        return 1;
    ric->fd         = client_fd;
    ric->subscribed = 0;
    dll_push_tail(rs->ipc_clients, ric);
    ipc_update_pfds(rs);

    return 0;
}

char**
split_msg_by_whitespaces(char* msg, size_t* len)
{
    char*  newline = NULL;
    char*  prev_nw = msg;
    size_t msg_len = strlen(msg);

    char** msg_args     = NULL;
    size_t msg_args_len = 1;

    char* ws_counter_p = msg;
    while (*ws_counter_p) {
        if (*ws_counter_p == ' ')
            msg_args_len++;
        ws_counter_p++;
    }

    msg_args = calloc(msg_args_len, sizeof(*msg_args));
    if (!msg_args)
        return NULL;

    int i = 0;
    do {
        newline = strchr(prev_nw, ' ');
        if (newline == NULL)
            newline = msg + msg_len;

        size_t curr_arg_len = newline - prev_nw;
        char*  curr_arg     = calloc(1, curr_arg_len + 1);

        memcpy(curr_arg, prev_nw, curr_arg_len);
        curr_arg[curr_arg_len] = '\0';

        msg_args[i] = curr_arg;

        prev_nw = newline + 1; // +1 to skip whitespace
        i       = i + 1;
    } while (newline < msg + msg_len);

    *len = msg_args_len;
    return msg_args;
}

int
ipc_proccess_client_msg(struct redstate* rs, int client_fd)
{
    struct redipcclient* ric = NULL;
    dll_for_each(rs->ipc_clients, v)
    {
        if (v->val->fd == client_fd) {
            ric = v->val;
            break;
        }
    }

    char buf[RED_IPC_MAX_MSG_LEN + 1];
    memset(buf, 0, sizeof(buf));

    int n = read(client_fd, buf, RED_IPC_MAX_MSG_LEN + 1);
    if (n == 0)
        goto remove;

    if (n < 0) {
        ROG_ERR("ipc read err: %s", strerror(errno));
        goto remove;
    }

    if (n > RED_IPC_MAX_MSG_LEN) {
        ROG_WARN("ipc msg len exceeded max capacity");
        goto close;
    }

    if (buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
        n--;
    } else
        buf[n] = '\0';

#if RED_IPC_DEBUG_MSG_LOG == 1
    ROG("ipc read n bytes: %d, with buf: \"%s\"", n, buf);
#endif

    size_t len         = 0;
    int    should_free = 0;
    char** msg         = split_msg_by_whitespaces(buf, &len);
    char*  back_msg    = NULL;
    if (msg)
        back_msg = handle_ipc_msg(rs, ric, msg, len, &should_free);

    if (!msg || !back_msg)
        back_msg = "server_error";
    n = write(client_fd, back_msg, strlen(back_msg));

    if (should_free)
        free(back_msg);
    if (n <= 0)
        goto close;
    return 0;
close:
    if (client_fd != -1)
        close(client_fd);
remove:
    dll_remove_val(rs->ipc_clients, ric);
    ipc_destroy_client(ric);
    ipc_update_pfds(rs);
    return 0;
}
