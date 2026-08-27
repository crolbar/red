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
#include "utils.h"

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
        char* app_id  = NULL;
        char* title   = NULL;
        char* fi_path = NULL;
        UTIL_STR_ESCAPE(v->val->app_id, app_id);
        UTIL_STR_ESCAPE(v->val->title, title);
        UTIL_STR_ESCAPE(v->val->fi_path, fi_path);

        const char* fmt = "%s{"
                          "\"idx\":\"%d\","
                          "\"app_id\":\"%s\","
                          "\"title\":\"%s\","
                          "\"fi_path\":\"%s\","
                          "\"is_focused\":%s"
                          "}";

        int   n   = snprintf(NULL,
                         0,
                         fmt,
                         (rt_idx) ? "," : "",
                         rt_idx,
                         app_id,
                         title,
                         fi_path,
                         (v->val == rs->focused_rt) ? "true" : "false");
        char* str = calloc(1, n + 1);
        sprintf(str,
                fmt,
                (rt_idx) ? "," : "",
                rt_idx,
                app_id,
                title,
                fi_path,
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

        free(app_id);
        free(title);
        free(fi_path);

        rt_idx++;
    }

    // overwrite the nullterm
    out[i++] = ']';
    out[i++] = '\n';
    out[i]   = '\0';

    return out;
}

char*
handle_ipc_msg_add_bind(struct redstate* rs, char** msg, size_t msg_len)
{
    if (msg_len < 4)
        return "too few arguments provided to add bind";

    // 1 should always be the key
    char*        key    = msg[1];
    xkb_keysym_t keysym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
    if (keysym == XKB_KEY_NoSymbol)
        return "invalid key passed to add bind";

    // 2 is always the modifiers
    uint8_t mods  = RED_MOD_NO_MODS;
    char*   start = msg[2];
    while (*start) {
        char* end = strchr(start, '+');
        if (end)
            *end = '\0';

        if (strcmp(start, "RED_MOD_SHIFT") == 0)
            mods |= RED_MOD_SHIFT;
        else if (strcmp(start, "RED_MOD_SUPER") == 0)
            mods |= RED_MOD_SUPER;
        else if (strcmp(start, "RED_MOD_ALT") == 0)
            mods |= RED_MOD_ALT;
        else if (strcmp(start, "RED_MOD_CTRL") == 0)
            mods |= RED_MOD_CTRL;
        else if (strcmp(start, "RED_MOD_NO_MODS") == 0)
            mods = 0;
        else
            return "invalid mod passed to add bind";

        if (end == NULL)
            break;
        *end  = '+';
        start = end + 1;
    }

    // 3.. are action + args
    if (!is_valid_action(msg[3]))
        return "non valid action";
    // TODO: free
    size_t action_len = msg_len - 3;
    char** action     = calloc(action_len + 1, sizeof(*action));
    for (size_t i = 0; i < action_len; i++) {
        char* a = malloc(strlen(msg[3 + i]) + 1);
        strcpy(a, msg[3 + i]);
        action[i] = a;
    }
    action[action_len] = NULL;

    struct redbind bind = {};
    bind.key_mods_bm    = REDBIND_CREATE_BM(keysym, mods);
    bind.action         = action;
    bind.action_len     = action_len;
    // TODO: way to add to another preset?
    bind.preset_n     = cfg.sel_bind_preset;
    bind.not_repeated = 0;
    bind.wl_client    = 0;

    UTIL_ADD_BIND(rs->binds, bind, rs->binds_len, rs->binds_cap);

    return "ok";
fail:
    return "failed to add bind";
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
        return "listening\n";
    }
    if (strcmp(msg[0], RED_IPC_MSG_ADD_BIND) == 0) {
        return handle_ipc_msg_add_bind(rs, msg, msg_len);
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

#define SEND_MSG(msg)                                                          \
    if (_send_state_change_msg(v->val, (msg)))                                 \
        dll_push_tail(clients_to_destroy, v->val);

int
ipc_send_state_changes(struct redstate* rs)
{
    dll(struct redipcclient*) clients_to_destroy = dll_init();

    dll_for_each(rs->ipc_clients, v)
    {
        if (!v->val->subscribed)
            continue;

        if (rs->ipc_red_state_changes & RED_STATE_RT_CREATE) {
            SEND_MSG("new window created\n");
        }
        if (rs->ipc_red_state_changes & RED_STATE_RT_CHANGE_FOCUS) {
            SEND_MSG("window focus changed\n");
        }
        if (rs->ipc_red_state_changes & RED_STATE_RT_DESTROY) {
            SEND_MSG("window closed\n");
        }
        if (rs->ipc_red_state_changes & RED_STATE_RT_FI) {
            SEND_MSG("window frame images updated\n");
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
    for (size_t i = 0; i < len; i++)
        free(msg[i]);
    free(msg);
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
