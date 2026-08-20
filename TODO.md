- [x]rendering
- [x]surface scaling
- [x]subsurf, popup
  - [x]subsurf hit testing + pointer events
    - [ ]subsurf ot top of subsurf hit testing breaks
- [x]wlr layers
  - [x]layer keyboard input
  - [x]layer pointer input
  - ~~[ ]layer in overlay plane?~~
- [x]toplevel focus switch animation
  - ~~[ ]toplevel open/close animation?~~
- [x]ipc
  - [x]sub for updates
- [x]windows like alt-tap with quickshell
  - [ ]optimize size & when to updatef FIs
- ~~[ ]put window in overlay plane (kind of like a float window)~~

- [/]run through valgrind (proper memory management)
  - [ ]fix possible losess
- [x]make vt recover on crash

- [x]dmabuf
- [x]refac gbm.c

- [x]drm cursor + wayland pointer
- [x]fix up wayland backend

- [x]get foot terminal on screen
- [x]figure out frames
- [x]drm.c
- [x]wayland client
- [x]backends (drm, wayland)

- [x]logging
- [x]refac signals (handles, signals.c...)
- [x]vt.c
- [x]state struct

- [x]modifiers on fbs
- [x]gl error catching

- [x]middle mouse button scroll (autoscroll)
  - [ ]horizontal
  - [ ]send values other than 1 instead of lowering delay
- [x]screenshots
- [x]screencast

- something about making a background window recive normal frame callback rate
  (when alt tabbing out of games they stop loading)

## planned features

- [ ]zoom like magnifier
- [ ]output management (name, description, refreshrate...)
  - [ ]way to choose which output connector to use

## fixes

- [ ]refac redsurface
- [ ]allow cursor plane to not be presend & fallback to software cursor
