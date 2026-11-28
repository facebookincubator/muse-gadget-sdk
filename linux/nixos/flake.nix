{
  description = "musegadget (Muse Gadget SDK Linux daemon) for NixOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # The SDK source: this repository. Only the linux/ subdirectory
    # is the Python package; the flake appends it below. To build
    # from a local checkout instead, override the input, e.g.
    #   nix build --override-input musegadget-src path:/path/to/muse-gadget-sdk
    musegadget-src = {
      url = "github:facebookincubator/muse-gadget-sdk";
      flake = false;
    };
  };

  outputs =
    { self, nixpkgs, musegadget-src, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          musegadget = pkgs.python3Packages.callPackage ./package.nix {
            musegadget-src = "${musegadget-src}/linux";
          };
          default = pkgs.python3Packages.callPackage ./package.nix {
            musegadget-src = "${musegadget-src}/linux";
          };
        }
      );

      nixosModules.musegadget =
        { pkgs, ... }:
        {
          imports = [ ./module.nix ];
          # Make pkgs.musegadget resolve to this flake's package so the
          # module's default package works without an overlay elsewhere.
          nixpkgs.overlays = [
            (final: prev: {
              musegadget = final.python3Packages.callPackage ./package.nix {
                musegadget-src = "${musegadget-src}/linux";
              };
            })
          ];
        };
      nixosModules.default = self.nixosModules.musegadget;
    };
}
