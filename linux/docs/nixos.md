# NixOS

For NixOS systems there is a flake and a NixOS module instead of the
installer script: see [../nixos/README.md](../nixos/README.md).

In brief:

- Add the flake (`linux/nixos`) as an input, import
  `nixosModules.musegadget`, and set
  `services.musegadget = { enable = true; runAs = "<account>"; };`
- Place the SDK token by hand at `/var/lib/musegadget/sdk_token`
  (mode 600) — it is never a Nix option, because the store is
  world-readable.
- Pair with `musegadget pair` and the Muse app, the same as any
  other platform.

One NixOS-specific note: nixpkgs' CA bundle still ships a root the
daemon distrusts, so the package runs with a filtered copy of the
bundle. The nixos README's "Trust store" section explains why and
how.
