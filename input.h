#pragma once

struct libinput*
init_input();

int
input_check_close(struct libinput* li, int tty_fd);
