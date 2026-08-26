{
  commit,
  lib,
  stdenv,
  pkg-config,
  clang-tools,
  wayland-scanner,
  wayland-protocols,
  wlr-protocols,
  libglvnd,
  libgbm,
  libdrm,
  seatd,
  libinput,
  libxkbcommon,
  wayland,
}:
stdenv.mkDerivation rec {
  pname = "red";
  version = commit;

  src = ./.;

  makeFlags = ["PREFIX=$(out) CFLAGS_REDCTL=\"-DVERSION=\\\"${commit}\\\"\""];

  nativeBuildInputs = [
    pkg-config
    clang-tools
    wayland-scanner
    wayland-protocols
    wlr-protocols
  ];

  buildInputs = [
    seatd
    libdrm
    libgbm
    libglvnd
    wayland
    libinput
    libxkbcommon
  ];

  postInstall = ''
    install -Dm644 red.desktop -t $out/share/wayland-sessions
    install -Dm644 rt_switcher.qml "$out/etc/xdg/quickshell/rt_switcher/shell.qml"
  '';

  passthru.providedSessions = [pname];
  meta = {
    homepage = "https://github.com/crolbar/red";
    description = "A non-intelligent wayland server";
    platforms = lib.platforms.linux;
    mainProgram = "red";
  };
}
