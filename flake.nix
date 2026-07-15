{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = inputs: let
    system = "x86_64-linux";
    pkgs = import inputs.nixpkgs {inherit system;};
  in {
    devShells.${system}.default = pkgs.mkShell {
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

        wayland
      ];

      shellHook = ''
        pkg-config --cflags libdrm | tr ' ' '\n' > compile_flags.txt
      '';
    };
  };
}
