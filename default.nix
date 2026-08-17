{
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
  version = "v0.1";

  src = ./.;

  makeFlags = ["PREFIX=$(out) BINS=${pname}"];

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
  '';

  passthru.providedSessions = [pname];
  meta = {
    homepage = "https://github.com/crolbar/red";
    description = "A non-intelligent wayland server";
    platforms = lib.platforms.linux;
    mainProgram = "red";
  };
}
