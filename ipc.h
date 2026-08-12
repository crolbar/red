#pragma once

#include "red.h"

#define RED_IPC_DEBUG_MSG_LOG 0

#define RED_IPC_MAX_CLIENTS 5
#define RED_IPC_MAX_MSG_LEN 24

#define RED_IPC_MSG_CFG_CURSOR_AUTOHIDE_TIME "cur_hide_time"

int
init_ipc();

int
ipc_accept_conn(struct redstate* rs);

int
ipc_update_pfds(struct redstate* rs);

int
ipc_proccess_client_msg(struct redstate* rs, int client_fd);
