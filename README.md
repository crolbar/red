# Notes

- configuring the server happens in `./config.c` or through the ipc
- startup script that runs when the compositor starts up can be placed in
  `~/.config/red/start.sh`
- cursor can be changed by replacing `./red_cursor.c`. more info in `./config.c`
- ipc socket is under `RED_SOCKET`
- valid ipc messages can be seen in `./ipc.h`. also all actions in `./actions.h`
  are valid and can be used through the ipc

# Building

### Dependancies (runtime)

- `seatd`
- `wayland`
- `libdrm`
- `libgbm`
- `libglvnd`
- `libinput`
- `libxkbcommon`

## Manual

```
make release
```

- executable is `./red`

### Dependancies

- `make`
- `gcc`
- `pkg-config`
- `wayland-protocols`
- `wlr-protocols`

## Nix

Package derivation is available in `./default.nix` exposed in the flake's
`packages.x86_64-linux.default` output.

```
nix build
```

to use it

# "Alt+Tab" like Menu

`rt_switcher.qml` with `quickshell` is used to create the window.

- `socat` would be a dependancy that is used in the quickshell window.

`rt_switcher.qml` should be placed in
`/etc/xdg/quickshell/rt_switcher/shell.qml` to work with the default config,\
as it uses `qs -c rt_switcher` to spawn it.

- On Nix this path is created automaticly.

to open the menu the quickshell ipc can be used with
`qs -c rt_switcher ipc call main toggle`.

> [!NOTE]
> the frame images are created in `/tmp` with file names like `red-foot-1.ppm`

# Resources

- wayland: `https://gitlab.freedesktop.org/wayland/wayland`,
  `https://wayland.freedesktop.org/docs/html/index.html`
- dma-buf
  `https://www.kernel.org/doc/html/next/userspace-api/dma-buf-alloc-exchange.html`
- drm `https://www.kernel.org/doc/html/latest/gpu/drm-kms.html`,
  `https://commandlinux.com/man-page/man7/drm-kms/`
- vt switching `https://github.com/kennylevinsen/seatd`
- gbm `https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/gbm/main/gbm.c`
- egl `https://registry.khronos.org/EGL/sdk/docs/man/`
- GL `https://registry.khronos.org/OpenGL-Refpages/es2.0/`
