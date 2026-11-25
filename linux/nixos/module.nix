# services.musegadget — NixOS module for the Muse Gadget SDK daemon.
# Verified on a live NixOS 26.05 install (Intel J3710 mini PC):
# the daemon started under this module, paired over BLE and
# registered with the Muse service. Every line maps to something the
# stock install.sh does imperatively on Debian:
#   - systemd unit with StateDirectory/RuntimeDirectory (the shipped
#     unit already declares both, in NixOS's preferred style)
#   - [GATT] ExchangeMTU = 256  -> hardware.bluetooth.settings
#     (REQUIRED for Android pairing; without it pairing dies at the
#     Wi-Fi step with "Couldn't connect" in the app)
#   - bluetoothd --noplugin=battery -> hardware.bluetooth.disabledPlugins
#     (iPhone-specific, but a one-liner, so no reason to skip)
# The SDK token is NOT a module option on purpose: the Nix store is
# world-readable. Place it by hand once (see README.md).
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.musegadget;
in
{
  options.services.musegadget = {
    enable = lib.mkEnableOption "the Muse Gadget SDK Linux daemon (musegadget)";

    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.musegadget;
      defaultText = lib.literalExpression "pkgs.musegadget";
      description = "The musegadget package to run.";
    };

    runAs = lib.mkOption {
      type = lib.types.str;
      example = "phlux";
      description = ''
        Account Muse's commands run as (MUSEGADGET_RUN_AS). The daemon
        itself runs as root — it owns the device credentials and the
        local socket — and drops to this account per command. This
        account's sudo rights ARE the security decision, exactly as on
        Debian; NixOS just makes the decision visible in the config.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    # Pairing-only Bluetooth config (the running service never touches
    # Bluetooth, D-Bus or GLib — ble_server.py is lazily imported by
    # the `pair` subcommand only).
    hardware.bluetooth = {
      enable = true;
      settings.GATT.ExchangeMTU = 256;
      disabledPlugins = [ "battery" ];
    };

    # The CLI on PATH so `musegadget pair` / `musegadget info` can be
    # run by hand; pairing is a manual, phone-in-hand step.
    environment.systemPackages = [ cfg.package ];

    systemd.services.musegadget = {
      description = "Muse Home Link (Linux Device SDK)";
      wants = [ "network-online.target" ];
      after = [
        "network-online.target"
        "bluetooth.target"
      ];
      wantedBy = [ "multi-user.target" ];
      environment = {
        PYTHONUNBUFFERED = "1";
        MUSEGADGET_RUN_AS = cfg.runAs;
      };
      serviceConfig = {
        ExecStart = "${cfg.package}/bin/musegadget run";
        RuntimeDirectory = "musegadget";
        RuntimeDirectoryMode = "0755";
        StateDirectory = "musegadget";
        StateDirectoryMode = "0700";
        Restart = "always";
        RestartSec = 5;
        # musegadget exits 78 (EX_CONFIG) when the system trust store
        # needs fixing first — cli.py uses it precisely so the unit
        # does NOT restart on it. Without this, a trust-store refusal
        # restart-loops every RestartSec and spams the journal
        # (observed on the J3710's first boot, 2026-11-25).
        RestartPreventExitStatus = 78;
      };
    };
  };
}
