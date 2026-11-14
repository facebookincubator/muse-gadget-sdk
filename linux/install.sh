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
# Installs system packages (bringing ca-certificates up to date), a pinned and
# checksum-verified uv, and musegadget into /opt/musegadget;
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
ARCH_PACKAGES=(bluez python python-dbus python-gobject python-cryptography curl ca-certificates)
FEDORA_PACKAGES=(bluez python3 python3-dbus python3-gobject python3-cryptography curl ca-certificates)

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
  --sdk-token TOKEN Your mgst_ SDK token from gadgets.muse.ai. If omitted, an
                    interactive install prompts securely. Saved, readable
                    only by root, in $STATE_DIR/sdk_token.
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
    local id="${ID:-}" version="${VERSION_ID:-0}" major="${VERSION_ID:-0}"
    major="${major%%.*}"
    case "$id" in
        debian|raspbian)
            DISTRO_FAMILY=debian
            [ "${major:-0}" -ge 11 ] 2>/dev/null ||
                die "$PRETTY_NAME is too old. Upgrade to Debian 11 (Raspberry Pi OS Bullseye) or later." ;;
        ubuntu)
            DISTRO_FAMILY=debian
            [ "$(printf '%s\n22.04\n' "$version" | sort -V | head -1)" = 22.04 ] ||
                die "$PRETTY_NAME is too old. Upgrade to Ubuntu 22.04 or later." ;;
        arch)
            DISTRO_FAMILY=arch ;;
        fedora)
            DISTRO_FAMILY=fedora ;;
        *)
            case " ${ID_LIKE:-} " in
                *" fedora "*|*" rhel "*)
                    DISTRO_FAMILY=fedora
                    warn "$PRETTY_NAME is untested; continuing because it is Fedora-based." ;;
                *" arch "*)
                    DISTRO_FAMILY=arch
                    warn "$PRETTY_NAME is untested; continuing because it is Arch-based." ;;
                *" debian "*|*" ubuntu "*)
                    DISTRO_FAMILY=debian
                    warn "$PRETTY_NAME is untested; continuing because it is Debian-based." ;;
                *) die "$PRETTY_NAME is not supported. This installer needs Debian/Ubuntu, Arch or Fedora Linux." ;;
            esac ;;
    esac
    if [ "$DISTRO_FAMILY" = fedora ]; then
        command -v dnf >/dev/null || die "dnf not found."
        ARCH="$(uname -m)"
        case "$ARCH" in x86_64|aarch64) ;; *) die "unsupported Fedora architecture: $ARCH" ;; esac
    elif [ "$DISTRO_FAMILY" = arch ]; then
        command -v pacman >/dev/null || die "pacman not found."
        ARCH="$(uname -m)"
        case "$ARCH" in x86_64|aarch64) ;; *) die "unsupported Arch architecture: $ARCH" ;; esac
    else
        command -v apt-get >/dev/null || die "apt-get not found."
        # Many 32-bit Raspberry Pi installs run a 64-bit kernel, so ask dpkg,
        # which reports the userland architecture.
        ARCH="$(dpkg --print-architecture)"
        case "$ARCH" in armhf|arm64|amd64) ;; *) die "unsupported architecture: $ARCH" ;; esac
    fi
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
    if [ "$DISTRO_FAMILY" = fedora ]; then
        for pkg in "${FEDORA_PACKAGES[@]}"; do
            rpm -q "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
        done
    elif [ "$DISTRO_FAMILY" = arch ]; then
        for pkg in "${ARCH_PACKAGES[@]}"; do
            pacman -Qq "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
        done
    else
        for pkg in "${APT_PACKAGES[@]}"; do
            dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed' || missing+=("$pkg")
        done
    fi
    case "$SOURCE" in git+*) command -v git >/dev/null || missing+=(git) ;; esac
    if [ "${#missing[@]}" -eq 0 ]; then return; fi
    say "Installing system packages: ${missing[*]}"
    if [ "$DISTRO_FAMILY" = fedora ]; then
        as_root dnf install -y "${missing[@]}"
    elif [ "$DISTRO_FAMILY" = arch ]; then
        as_root pacman -S --needed --noconfirm "${missing[@]}"
    else
        as_root env DEBIAN_FRONTEND=noninteractive apt-get update -qq
        as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${missing[@]}"
    fi
}

# musegadget uses the system trust store, and distros remove distrusted root
# CAs in ca-certificates updates, so bring that one package up to date.
update_ca_certificates() {
    local updated=0
    if [ "$DISTRO_FAMILY" = fedora ]; then
        as_root dnf upgrade -y -q ca-certificates >/dev/null 2>&1 && updated=1
    elif [ "$DISTRO_FAMILY" = debian ]; then
        as_root env DEBIAN_FRONTEND=noninteractive apt-get update -qq >/dev/null 2>&1 &&
            as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --only-upgrade \
                ca-certificates >/dev/null 2>&1 && updated=1
    else
        # Upgrading one package without the rest is a partial upgrade on Arch.
        say "ca-certificates: left to pacman -Syu, which keeps it current on Arch."
        return
    fi
    if [ "$updated" = 1 ]; then
        say "ca-certificates is up to date."
    else
        warn "couldn't update ca-certificates; continuing with the installed version."
    fi
}

# Root runs what is under $PREFIX, so no other account may be able to change
# any of it.

# True if $1 is a real directory (not a symlink), owned by root and not
# writable by group or others.
root_only_dir() {
    local uid mode kind
    read -r uid mode kind <<<"$(as_root env LC_ALL=C stat -c '%u %a %F' -- "$1" 2>/dev/null)"
    [ "$kind" = directory ] && [ "$uid" = 0 ] && (( (8#$mode & 8#022) == 0 ))
}

# Prints the first path under $1 that an account other than root could change.
# uv makes the venv's .lock writable by everyone on purpose; it only holds a
# lock, and nothing runs or reads it.
first_unsafe_path() {
    as_root find "$1" ! -type l ! -path "$VENV/.lock" \( ! -uid 0 -o -perm /022 \) -print -quit
}

prepare_prefix() {
    local dir
    for dir in "$(dirname "$PREFIX")" "$PREFIX" "$PREFIX/bin"; do
        if as_root test -e "$dir" || as_root test -L "$dir"; then
            root_only_dir "$dir" ||
                die "$dir must be a directory that only root can change. Fix its owner and mode, or remove $PREFIX with 'bash install.sh --uninstall', then run this installer again."
        else
            as_root install -d -o 0 -g 0 -m 0755 "$dir"
        fi
    done
    # Left by an earlier run that was stopped part way.
    as_root rm -rf -- "$PREFIX"/.uv.* "$PREFIX"/.venv.*
}

# Fails if anything under $PREFIX is writable by another account.
check_prefix() {
    local unsafe
    unsafe="$(first_unsafe_path "$PREFIX")"
    [ -z "$unsafe" ] || die "$unsafe can be changed by an account other than root. Run 'bash install.sh --uninstall', then this installer again."
}

uv_target() {
    case "$ARCH" in
        amd64|x86_64) echo x86_64-unknown-linux-gnu ;;
        arm64|aarch64) echo aarch64-unknown-linux-gnu ;;
        armhf)
            # A Pi Zero or Pi 1 is ARMv6; newer boards run ARMv7 code.
            case "$(uname -m)" in
                armv6*) echo arm-unknown-linux-musleabihf ;;
                *) echo armv7-unknown-linux-gnueabihf ;;
            esac ;;
        *) die "no uv build for $ARCH." ;;
    esac
}

# SHA-256 of uv's release archive for each target, and of the uv binary in it.
uv_release() {
    case "$1" in
        x86_64-unknown-linux-gnu)
            UV_ARCHIVE_SHA256=9ec303873e00deed44d1b2b52b85ab7aa55d849588d7242298748390eaea07ef
            UV_BINARY_SHA256=83f28650272b5543e939debbc7a5578d087952399fd945e9267b1db0ccc27d33 ;;
        aarch64-unknown-linux-gnu)
            UV_ARCHIVE_SHA256=35cbad89f226a06cd8b295f946e7083e15cd6fa058e09c2e02bb388be39a8fe4
            UV_BINARY_SHA256=b5a863358779ca7f74488b9fb53c88a18851985858944807f198879383d0de49 ;;
        armv7-unknown-linux-gnueabihf)
            UV_ARCHIVE_SHA256=0390a683afc68c10c3cef45d80678015c6d097260a612ab6bca006ec68f75918
            UV_BINARY_SHA256=a2e16a8866ee99e14e0935a6fbc002504ba0eeea858d86fbd29774e80e7c1bf4 ;;
        arm-unknown-linux-musleabihf)
            UV_ARCHIVE_SHA256=0ac0dc5e822e8acc9bcd53d3854ebbeef80c806d5722ee8666b8fa49582c8ff3
            UV_BINARY_SHA256=9fde3996869c31d753441ee2e7d7cb2cb2b78dd2a7e3e1661970a40c722763fc ;;
        *) die "no uv build for $1." ;;
    esac
}

sha256_of() { as_root sha256sum -- "$1" | awk '{print $1}'; }

remove_uv_tmp() { if [ -n "${UV_TMP:-}" ]; then as_root rm -rf -- "$UV_TMP"; UV_TMP=""; fi; }

# Installs the pinned uv as a new root-owned file. An existing uv is never
# run: it is copied into a directory only root can read, and the copy is kept
# only if it is exactly the pinned binary. Otherwise uv comes from its GitHub
# release, checked before it is unpacked.
install_uv() {
    local target bin="$PREFIX/bin/uv"
    target="$(uv_target)"
    uv_release "$target"
    # Not under $TMPDIR, which another account may own.
    UV_TMP="$(as_root mktemp -d "$PREFIX/.uv.XXXXXX")"
    trap remove_uv_tmp EXIT
    if as_root test -f "$bin" && ! as_root test -L "$bin"; then
        as_root cp -- "$bin" "$UV_TMP/uv" || true
    fi
    # uvx came with the old installer; nothing here uses it.
    as_root rm -rf -- "$bin" "$PREFIX/bin/uvx"
    if ! as_root test -f "$UV_TMP/uv" || [ "$(sha256_of "$UV_TMP/uv")" != "$UV_BINARY_SHA256" ]; then
        say "Installing uv $UV_VERSION"
        as_root rm -f -- "$UV_TMP/uv"
        as_root curl -fsSL --proto '=https' --tlsv1.2 -o "$UV_TMP/uv.tar.gz" \
            "https://github.com/astral-sh/uv/releases/download/$UV_VERSION/uv-$target.tar.gz" ||
            die "couldn't download uv $UV_VERSION."
        [ "$(sha256_of "$UV_TMP/uv.tar.gz")" = "$UV_ARCHIVE_SHA256" ] ||
            die "the uv $UV_VERSION download doesn't match its checksum."
        as_root tar -xzf "$UV_TMP/uv.tar.gz" -C "$UV_TMP" --no-same-owner --no-same-permissions \
            --strip-components 1 "uv-$target/uv"
        [ "$(sha256_of "$UV_TMP/uv")" = "$UV_BINARY_SHA256" ] ||
            die "uv $UV_VERSION doesn't match its checksum."
    fi
    as_root install -o 0 -g 0 -m 0755 "$UV_TMP/uv" "$bin"
    remove_uv_tmp
}

enable_bluez() {
    if { [ "$DISTRO_FAMILY" = arch ] || [ "$DISTRO_FAMILY" = fedora ]; } && systemd_running; then
        say "Enabling the BlueZ service"
        as_root systemctl enable --now bluetooth.service
    fi
}

# Root's uv ignores uv.toml and pyproject.toml settings, which another account
# could have written in or above the current directory, and copies files
# rather than linking them to its cache.
run_uv() { as_root env UV_NO_CONFIG=1 UV_LINK_MODE=copy "$PREFIX/bin/uv" "$@"; }

data_dir() {
    "$1/bin/python" -I -c 'import musegadget, os; print(os.path.join(os.path.dirname(musegadget.__file__), "data"))'
}

install_musegadget() {
    # A new venv each time, so nothing left in an earlier one runs as root.
    # It's built beside the old one, which keeps working if this fails.
    # The system interpreter is required: dbus and gi come from apt and are
    # built for it.
    local new
    say "Creating $VENV"
    new="$(as_root mktemp -d "$PREFIX/.venv.XXXXXX")"
    as_root chmod 0755 "$new"
    run_uv venv --quiet --relocatable --allow-existing --python /usr/bin/python3 \
        --system-site-packages "$new"
    say "Installing musegadget from $SOURCE"
    run_uv pip install --quiet --python "$new/bin/python" --no-deps --reinstall "$SOURCE"
    run_uv pip install --quiet --python "$new/bin/python" --require-hashes \
        -r "$(data_dir "$new")/requirements.lock"
    as_root rm -rf -- "$VENV"
    as_root mv -T -- "$new" "$VENV"
    as_root ln -sf "$VENV/bin/musegadget" /usr/local/bin/musegadget
    DATA_DIR="$(data_dir "$VENV")"
}

# musegadget won't connect while the trust store can't be read or still has a
# distrusted root CA, so stop before installing a service that can't start.
check_trust_store() {
    local problem
    # As the service runs it: as root, without this shell's SSL_CERT_* settings
    # (and, with -I, without importing from the current directory).
    problem="$(as_root env -u SSL_CERT_FILE -u SSL_CERT_DIR -u SSL_CERT_URI "$VENV/bin/python" -I \
        -c 'from musegadget import tls; print(tls.trust_store_problem() or "")')" ||
        die "couldn't check the system trust store."
    [ -z "$problem" ] && return
    # An earlier install's service would keep restarting the new code, which refuses to run.
    if systemd_running && [ -e "$UNIT" ]; then
        as_root systemctl stop musegadget.service >/dev/null 2>&1 || true
    fi
    die "musegadget won't connect: $problem. Update your ca-certificates package, then run this installer again."
}

configure_bluez() {
    # The Muse Android app writes (MTU - 3)-byte packets and fails above 512,
    # so keep the negotiated MTU at 256, as the ESP32 firmware does.
    local result
    result="$(as_root python3 -I - "$BLUEZ_CONF" <<'EOF'
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
    result="$(as_root python3 -I - "$BLUEZ_DROPIN" "$unit_text" <<'EOF'
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

prompt_sdk_token() {
    if [ -n "$SDK_TOKEN" ] || [ "$NO_PAIR" = 1 ] || [ "$ASSUME_YES" = 1 ]; then
        return 0
    fi
    [ -r /dev/tty ] || return 0
    printf 'Muse SDK token (mgst_...; input hidden, Enter to keep/skip): ' >/dev/tty
    IFS= read -r -s SDK_TOKEN </dev/tty || SDK_TOKEN=""
    printf '\n' >/dev/tty
    if [ -n "$SDK_TOKEN" ] && ! [[ "$SDK_TOKEN" =~ ^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$ ]]; then
        die "that SDK token is not valid; copy it again from gadgets.muse.ai."
    fi
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
    as_root "$VENV/bin/musegadget" pair || warn "not paired. Run 'sudo musegadget pair' to try again."
}

summary() {
    echo
    as_root "$VENV/bin/musegadget" info
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
    SOURCE="$DEFAULT_SOURCE" RUN_AS="" SDK_TOKEN="" ASSUME_YES=0 NO_PAIR=0 UNINSTALL=0 PURGE=0 UV_TMP=""
    # Nothing the installer creates should be writable by group or others.
    umask 022
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
    if [ -d "$SOURCE" ]; then SOURCE="$(cd "$SOURCE" && pwd)"; fi

    check_system
    prompt_sdk_token
    if [ -n "$SDK_TOKEN" ] && ! [[ "$SDK_TOKEN" =~ ^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$ ]]; then
        die "that SDK token is not valid; copy it again from gadgets.muse.ai."
    fi
    choose_account
    install_packages
    update_ca_certificates
    enable_bluez
    prepare_prefix
    install_uv
    install_musegadget
    check_prefix
    check_trust_store
    configure_bluez
    save_sdk_token
    install_service
    pair
    summary
}

# Everything runs from main, so a truncated download never runs a partial script.
# Sourcing this file (as the tests do) only defines the functions.
if ! (return 0 2>/dev/null); then main "$@"; fi
