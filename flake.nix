{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = inputs: let
    system = "x86_64-linux";
    pkgs = import inputs.nixpkgs {inherit system;};

    nativeBuildInputs = with pkgs; [
      clang-tools
      wayland-scanner
      pkg-config
      wayland-protocols
    ];
    buildInputs = with pkgs; [
      libglvnd
      libgbm
      libdrm
      libinput
      libxkbcommon

      wayland
    ];
  in {
    devShells.${system}.default = pkgs.mkShell {
      inherit nativeBuildInputs buildInputs;
      shellHook = ''
        pkg-config --cflags libdrm | tr ' ' '\n' > compile_flags.txt
      '';
    };
    packages.${system}.default = pkgs.stdenv.mkDerivation rec {
      pname = "red";
      version = "v0.1";
      src = ./.;
      inherit nativeBuildInputs buildInputs;
      makeFlags = ["PREFIX=$(out) BINS=${pname}"];
    };
  };
}
