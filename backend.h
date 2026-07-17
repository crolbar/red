#pragma once

// current backends: drm and wayland-client
struct backend
{
    void (*init)();
};
