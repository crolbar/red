#pragma once

#include <linux/vt.h>

int
vt_set_mode(int fd, struct vt_mode mode);

int
vt_start(int fd);

int
vt_stop(int fd);
