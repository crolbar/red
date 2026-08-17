{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = inputs: let
    system = "x86_64-linux";
    pkgs = import inputs.nixpkgs {inherit system;};
  in {
    devShells.${system}.default = pkgs.mkShell {
      nativeBuildInputs = with pkgs; [
        pkg-config
        clang-tools
        wayland-scanner
        wayland-protocols
        wlr-protocols
        gdb
        gperftools # libprofiler
        pprof # visualization of profiling data
        graphviz # directed graph visualization
      ];
      buildInputs = with pkgs; [
        libglvnd
        libgbm
        libdrm
        seatd
        libinput
        libxkbcommon

        wayland
      ];
      shellHook = ''
        pkg-config --cflags libdrm | tr ' ' '\n' > compile_flags.txt
      '';
    };
    packages.${system}.default = pkgs.callPackage ./default.nix {};
  };
}
