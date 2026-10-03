#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Install the Linux Device SDK.
#
#   curl -fsSL <url>/install.sh | bash
#   bash install.sh [--from SOURCE] [--run-as USER] [--sdk-token TOKEN] [--yes] [--no-pair]
#   bash install.sh --uninstall [--purge]
#
# Installs system packages, a pinned uv, and musegadget into /opt/musegadget;
# sets BlueZ's GATT MTU for the Android app; installs and starts the
# musegadget service; then opens Bluetooth pairing for the Muse app.

set -euo pipefail

UV_VERSION="0.9.9"
PREFIX="/opt/musegadget"
VENV="$PREFIX/venv"
UNIT="/etc/systemd/system/musegadget.service"
STATE_DIR="/var/lib/musegadget"
BLUEZ_CONF="/etc/bluetooth/main.conf"
BLUEZ_DROPIN="/etc/systemd/system/bluetooth.service.d/zz-musegadget.conf"
DEFAULT_SOURCE="git+https://github.com/facebookincubator/muse-gadget-sdk@main#subdirectory=linux"
APT_PACKAGES=(bluez python3 python3-dbus python3-gi python3-cryptography curl ca-certificates)

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: install.sh [options]

  --from SOURCE     Install musegadget from SOURCE: a local linux/ checkout, a
                    wheel, or a pip/uv URL. Default: $DEFAULT_SOURCE
  --run-as USER     Account whose permissions your Muse's commands run with.
                    Default: the account running this installer.
  --sdk-token TOKEN Your mgst_ SDK token from gadgets.muse.ai. Every gadget
                    needs one to pair. Saved, readable only by root, in
                    $STATE_DIR/sdk_token.
  --yes             Don't ask for confirmation.
  --no-pair         Install without opening Bluetooth pairing.
  --uninstall       Remove musegadget. Keeps the device identity and pairing
                    in $STATE_DIR unless --purge is also given.
  -h, --help        Show this help.
EOF
}

# Prompts read the terminal, not stdin, so `curl ... | bash` still works.
ask() {
    local prompt="$1" answer
    if [ "$ASSUME_YES" = 1 ]; then return 0; fi
    if [ ! -r /dev/tty ]; then die "no terminal to confirm on; rerun with --yes"; fi
    printf '%s [Y/n] ' "$prompt" >/dev/tty
    read -r answer </dev/tty || true
    case "$answer" in ""|y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

as_root() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }

systemd_running() { [ -d /run/systemd/system ]; }

# --- Checks ------------------------------------------------------------------

check_system() {
    [ "$(uname -s)" = Linux ] || die "The Linux Device SDK runs on Linux."
    [ -r /etc/os-release ] || die "cannot identify this Linux distribution (/etc/os-release missing)."
    # shellcheck disable=SC1091
    . /etc/os-release
    local id="${ID:-}" version="${VERSION_ID:-0}"
    # Debian testing and sid have no VERSION_ID; with set -u the raw expansion
    # above would abort the script, and the same local line can't reuse $version.
    local major="${version%%.*}"
    case "$id" in
        debian|raspbian)
            [ "${major:-0}" -ge 11 ] 2>/dev/null ||
                die "$PRETTY_NAME is too old. Upgrade to Debian 11 (Raspberry Pi OS Bullseye) or later." ;;
        ubuntu)
            [ "$(printf '%s\n22.04\n' "$version" | sort -V | head -1)" = 22.04 ] ||
                die "$PRETTY_NAME is too old. Upgrade to Ubuntu 22.04 or later." ;;
        *)
            case " ${ID_LIKE:-} " in
                *" debian "*|*" ubuntu "*) warn "$PRETTY_NAME is untested; continuing because it is Debian-based." ;;
                *) die "$PRETTY_NAME is not supported. This installer needs a Debian or Ubuntu based system." ;;
            esac ;;
    esac
    command -v apt-get >/dev/null || die "apt-get not found."
    # Many 32-bit Raspberry Pi installs run a 64-bit kernel, so ask dpkg,
    # which reports the userland architecture.
    ARCH="$(dpkg --print-architecture)"
    case "$ARCH" in armhf|arm64|amd64) ;; *) die "unsupported architecture: $ARCH" ;; esac
    if ! systemd_running; then
        warn "systemd is not running; the service will be installed but not started."
    fi
    if [ -z "$(ls /sys/class/bluetooth 2>/dev/null)" ]; then
        warn "no Bluetooth adapter found. You can install, but pairing with the Muse app needs Bluetooth LE."
    fi
}

choose_account() {
    if [ -z "$RUN_AS" ]; then
        if [ "$(id -u)" -ne 0 ]; then RUN_AS="$(id -un)"; else RUN_AS="${SUDO_USER:-}"; fi
    fi
    if [ -z "$RUN_AS" ] || [ "$RUN_AS" = root ]; then
        die "choose the account your Muse's commands run as with --run-as USER (running them as root is not supported)."
    fi
    id "$RUN_AS" >/dev/null 2>&1 || die "account '$RUN_AS' does not exist."

    local admin=""
    if as_root sudo -n -l -U "$RUN_AS" 2>/dev/null | grep -qE '\(ALL( : ALL)?\) (NOPASSWD: )?ALL'; then
        admin=" This account has administrator (sudo) rights, so your Muse will be able to do anything on this machine, including reading the device's own credentials."
    fi
    say "Your Muse will be able to run any command on this machine as '$RUN_AS', with that account's permissions.$admin"
    ask "Continue?" || die "cancelled. Rerun with --run-as to choose a different account."
}

# --- Install -----------------------------------------------------------------

install_packages() {
    local missing=() pkg
    for pkg in "${APT_PACKAGES[@]}"; do
        dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed' || missing+=("$pkg")
    done
    case "$SOURCE" in git+*) command -v git >/dev/null || missing+=(git) ;; esac
    if [ "${#missing[@]}" -eq 0 ]; then return; fi
    say "Installing system packages: ${missing[*]}"
    as_root env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${missing[@]}"
}

install_uv() {
    if [ -x "$PREFIX/bin/uv" ] && [ "$("$PREFIX/bin/uv" --version | awk '{print $2}')" = "$UV_VERSION" ]; then
        return
    fi
    say "Installing uv $UV_VERSION"
    as_root mkdir -p "$PREFIX/bin"
    curl -fsSL "https://astral.sh/uv/$UV_VERSION/install.sh" |
        as_root env UV_INSTALL_DIR="$PREFIX/bin" UV_NO_MODIFY_PATH=1 sh -s -- --quiet
}

install_musegadget() {
    local uv="$PREFIX/bin/uv"
    # The system interpreter is required: dbus and gi come from apt and are
    # built for it.
    if [ ! -x "$VENV/bin/python" ]; then
        say "Creating $VENV"
        as_root "$uv" venv --quiet --python /usr/bin/python3 --system-site-packages "$VENV"
    fi
    say "Installing musegadget from $SOURCE"
    as_root "$uv" pip install --quiet --python "$VENV/bin/python" --no-deps --reinstall "$SOURCE"
    local data
    data="$("$VENV/bin/python" -c 'import musegadget, os; print(os.path.join(os.path.dirname(musegadget.__file__), "data"))')"
    as_root "$uv" pip install --quiet --python "$VENV/bin/python" --require-hashes \
        -r "$data/requirements.lock"
    as_root ln -sf "$VENV/bin/musegadget" /usr/local/bin/musegadget
    DATA_DIR="$data"
}

configure_bluez() {
    # The Muse Android app writes (MTU - 3)-byte packets and fails above 512,
    # so keep the negotiated MTU at 256, as the ESP32 firmware does.
    local result
    result="$(as_root python3 - "$BLUEZ_CONF" <<'EOF'
import re, shutil, sys
path = sys.argv[1]
try:
    text = open(path).read()
except FileNotFoundError:
    text = ""
active = re.search(r"^\s*ExchangeMTU\s*=\s*(\d+)\s*$", text, re.M)
if active and int(active.group(1)) <= 256:
    print("unchanged"); sys.exit()
line = "ExchangeMTU = 256"
if active:
    text = text[:active.start()] + line + text[active.end():]
elif re.search(r"^\s*#\s*ExchangeMTU\s*=.*$", text, re.M):
    text = re.sub(r"^\s*#\s*ExchangeMTU\s*=.*$", line, text, count=1, flags=re.M)
elif re.search(r"^\[GATT\]\s*$", text, re.M):
    text = re.sub(r"^\[GATT\]\s*$", "[GATT]\n" + line, text, count=1, flags=re.M)
else:
    text = text.rstrip("\n") + ("\n\n" if text else "") + "[GATT]\n" + line + "\n"
if text != "":
    try:
        shutil.copy2(path, path + ".pre-musegadget")
    except FileNotFoundError:
        pass
open(path, "w").write(text)
print("changed")
EOF
)"
    if [ "$result" = changed ]; then
        say "Set BlueZ ExchangeMTU = 256 (backup: $BLUEZ_CONF.pre-musegadget)"
        if systemd_running; then as_root systemctl restart bluetooth; fi
    fi
    disable_bluez_battery
}

# BlueZ's battery plugin reads a connecting phone's battery level. iPhones
# answer that only over a bonded link, so BlueZ then asks the phone to bond,
# which this device refuses, and iOS pairing can drop. Gadgets never bond.
disable_bluez_battery() {
    systemd_running || return 0
    local unit_text result
    unit_text="$(systemctl cat bluetooth.service 2>/dev/null || true)"
    result="$(as_root python3 - "$BLUEZ_DROPIN" "$unit_text" <<'EOF'
import os, re, sys
path, unit_text = sys.argv[1], sys.argv[2]
# The last non-empty ExecStart= across the unit and its drop-ins is the one
# systemd runs. Leave anything quoted alone rather than risk rewriting it.
lines = re.findall(r"^ExecStart=(.*)$", unit_text, re.M)
command = next((l.strip() for l in reversed(lines) if l.strip()), "")
if not command or any(c in command for c in "\"'\\$%;"):
    print("skipped"); sys.exit()
args = command.split()
for i, arg in enumerate(args):
    inline = arg.startswith("--noplugin=")
    if inline or (arg in ("-P", "--noplugin") and i + 1 < len(args)):
        names = arg.split("=", 1)[1] if inline else args[i + 1]
        if "battery" in names.split(","):
            print("unchanged"); sys.exit()
        names = names + ",battery" if names else "battery"
        if inline:
            args[i] = "--noplugin=" + names
        else:
            args[i + 1] = names
        break
else:
    args.append("--noplugin=battery")
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "w") as f:
    f.write("# Written by the musegadget installer: never ask phones to bond.\n"
            "[Service]\nExecStart=\nExecStart=" + " ".join(args) + "\n")
print("changed")
EOF
)"
    case "$result" in
        changed)
            say "Turned off BlueZ's battery plugin so phones are never asked to bond ($BLUEZ_DROPIN)"
            as_root systemctl daemon-reload
            as_root systemctl restart bluetooth
            ;;
        skipped) say "Left BlueZ plugins as they are: could not read bluetooth.service's command line." ;;
    esac
}

save_sdk_token() {
    if [ -z "$SDK_TOKEN" ]; then
        if ! as_root test -s "$STATE_DIR/sdk_token"; then
            say "No SDK token yet. Get one at gadgets.muse.ai and rerun with --sdk-token; gadgets without one will stop pairing."
        fi
        return 0
    fi
    say "Saving your SDK token"
    as_root install -d -m 0700 "$STATE_DIR"
    printf '%s\n' "$SDK_TOKEN" | as_root install -m 0600 /dev/stdin "$STATE_DIR/sdk_token"
}

install_service() {
    say "Installing the musegadget service"
    sed "s/@RUN_AS@/$RUN_AS/" "$DATA_DIR/musegadget.service" | as_root tee "$UNIT" >/dev/null
    if systemd_running; then
        as_root systemctl daemon-reload
        as_root systemctl enable --now musegadget.service >/dev/null 2>&1
        as_root systemctl restart musegadget.service
    fi
}

pair() {
    if as_root test -s "$STATE_DIR/pairing.json"; then
        say "Already paired; the service will reconnect to your Muse."
        return
    fi
    if [ "$NO_PAIR" = 1 ]; then
        say "Skipping pairing. Run 'sudo musegadget pair' when you're ready."
        return
    fi
    cat <<EOF

Pair with your Muse:
  1. In the Muse app, turn on Settings > Devices > Developer mode.
  2. Add a device and choose the device named below.
  3. When asked for Wi-Fi, pick the network shown; no password is needed.

EOF
    as_root /usr/local/bin/musegadget pair || warn "not paired. Run 'sudo musegadget pair' to try again."
}

summary() {
    echo
    as_root /usr/local/bin/musegadget info
    cat <<EOF

Service:   sudo systemctl status musegadget
Logs:      sudo journalctl -u musegadget -f
Remove:    bash install.sh --uninstall
EOF
}

# --- Uninstall ---------------------------------------------------------------

uninstall() {
    say "Removing musegadget"
    if systemd_running && [ -f "$UNIT" ]; then
        as_root systemctl disable --now musegadget.service >/dev/null 2>&1 || true
    fi
    as_root rm -f "$UNIT" /usr/local/bin/musegadget
    if systemd_running; then as_root systemctl daemon-reload; fi
    as_root rm -rf "$PREFIX"
    if [ "$PURGE" = 1 ]; then
        as_root rm -rf "$STATE_DIR"
        say "Removed the device identity and pairing too. Remove the device in the Muse app as well."
    else
        say "Kept the device identity and pairing in $STATE_DIR (use --purge to remove them)."
    fi
    if [ -f "$BLUEZ_DROPIN" ]; then
        as_root rm -f "$BLUEZ_DROPIN"
        if systemd_running; then
            as_root systemctl daemon-reload
            as_root systemctl restart bluetooth
        fi
        say "Turned BlueZ's battery plugin back on."
    fi
    if [ -f "$BLUEZ_CONF.pre-musegadget" ]; then
        say "BlueZ settings were left as they are; the original is at $BLUEZ_CONF.pre-musegadget."
    fi
}

main() {
    SOURCE="$DEFAULT_SOURCE" RUN_AS="" SDK_TOKEN="" ASSUME_YES=0 NO_PAIR=0 UNINSTALL=0 PURGE=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --from) SOURCE="${2:?--from needs a value}"; shift 2 ;;
            --run-as) RUN_AS="${2:?--run-as needs a value}"; shift 2 ;;
            --sdk-token) SDK_TOKEN="${2:?--sdk-token needs a value}"; shift 2 ;;
            --yes|-y) ASSUME_YES=1; shift ;;
            --no-pair) NO_PAIR=1; shift ;;
            --uninstall) UNINSTALL=1; shift ;;
            --purge) PURGE=1; shift ;;
            -h|--help) usage; exit 0 ;;
            *) usage >&2; die "unknown option: $1" ;;
        esac
    done
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null || die "run this as root, or install sudo."
        sudo true || die "this installer needs sudo."
    fi
    if [ "$UNINSTALL" = 1 ]; then uninstall; return; fi
    if [ -n "$SDK_TOKEN" ] && ! [[ "$SDK_TOKEN" =~ ^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$ ]]; then
        die "that SDK token is not valid; copy it again from gadgets.muse.ai."
    fi
    if [ -d "$SOURCE" ]; then SOURCE="$(cd "$SOURCE" && pwd)"; fi

    check_system
    choose_account
    install_packages
    install_uv
    install_musegadget
    configure_bluez
    save_sdk_token
    install_service
    pair
    summary
}

# Everything runs from main, so a truncated download never runs a partial script.
main "$@"
