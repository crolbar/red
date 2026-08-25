#pragma once

#include "red.h"

#define RED_IPC_DEBUG_MSG_LOG 0

#define RED_IPC_MAX_CLIENTS 10
#define RED_IPC_MAX_MSG_LEN 512

#define RED_IPC_MSG_SUBSCRIBE "sub"

#define RED_IPC_MSG_CFG_CURSOR_AUTOHIDE_TIME "cur_hide_time"

#define RED_IPC_MSG_FETCH_TOPLEVELS "windows"

/*

synopsys: `add_bind KEY MODS ACTION [ACTION_ARGS]`
 - `MODS` are the macro names RED_MOD_* separated by `+`
 - `ACTION` is the value of the RED_ACTION_* macro
 - `ACTION_ARGS` argumets used by the action.
  args are separated by WHITESPACES.

if bind (KEY + MODS) already exists in the current preset,
it will overwrite the old one.

examples:
 add_bind Q RED_MOD_SUPER+RED_MOD_SHIFT quit
 add_bind XF86AudioNext RED_MOD_NO_MODS spawn brokctl next

NOTE:
 `add_bind XF86AudioNext RED_MOD_NO_MODS spawn bash -c "brokctl next"`
 wont work because arg are separated by whitepases so `"brokctl next"`
 is not a single argument, its two `"brokctl`, `next"`

*/
#define RED_IPC_MSG_ADD_BIND "add_bind"

int
init_ipc();

int
ipc_accept_conn(struct redstate* rs);

int
ipc_update_pfds(struct redstate* rs);

int
ipc_proccess_client_msg(struct redstate* rs, int client_fd);

int
ipc_send_state_changes(struct redstate* rs);
