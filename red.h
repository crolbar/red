#pragma once

struct redstate
{
    struct drmstate* drm;
    struct libinput* li;
    int tty_fd;
    int sig_fd;

    int rrender_fd; // read 1, and trigger render
    int wrender_fd; // write 1, to trigger render

    // TODO: use this
    int is_wayland_client; // in wayland compositor spawn as a client
    int active; // VT is active
    int should_quit;

    struct timespec* time_start;
    double last_frame_time;
    double frame_latency;

    double rect_x;
    double rect_y;
};
