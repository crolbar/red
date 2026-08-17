inputs: {
  config,
  lib,
  ...
}: let
  inherit (lib.options) mkOption mkEnableOption;
  inherit (lib) types mkIf optional;

  cfg = config.programs.red;
  pkg = inputs.self.packages.x86_64-linux.default;
in {
  options.programs.red = {
    enable = mkEnableOption "red";
    package = mkOption {
      type = types.package;
      default = pkg;
    };
  };
  config = mkIf cfg.enable {
    environment.systemPackages =
      optional (cfg.package != null) pkg;
  };
}
