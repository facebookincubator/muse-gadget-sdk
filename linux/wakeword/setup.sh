#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

# Optional voice prerequisites only: no pairing, credentials, or service changes.
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WHEEL=""
WHEEL_SHA256=""
TORCH_WHEEL=""
TORCH_SHA256=""
PYTHON=python3
VENV="$ROOT/.venv-wakeword"
BUILD_DIR="$ROOT/wakeword/build"
SIGNAL_LIBRARY=""
DRY_RUN=0
SYSTEM_PACKAGES=0
REUSE_VENV=0

usage() {
    printf '%s\n' \
      'Usage: setup.sh --wheel FILE.whl [options]' \
      '  --wheel FILE           Trusted, host-compatible ExecuTorch wheel (required)' \
      '  --wheel-sha256 HASH    Verify an expected SHA-256 before installing' \
      '  --torch-wheel FILE    Explicit compatible Torch wheel (needed in a fresh venv)' \
      '  --torch-sha256 HASH   Verify the supplied Torch wheel checksum' \
      '  --python COMMAND      Interpreter compatible with the wheel (default: python3)' \
      '  --venv DIRECTORY      Isolated environment (default: linux/.venv-wakeword)' \
      '  --build-dir DIRECTORY Signal-library output (default: linux/wakeword/build)' \
      '  --signal-library FILE  Use a trusted prebuilt signal library instead of compiling' \
      '  --reuse-venv           Allow package changes in an existing environment' \
      '  --install-system-packages  Install compiler/venv/PortAudio with sudo if needed' \
      '  --dry-run             Print commands; create/install/build nothing' \
      '  -h, --help            Show this help'
}

die() { printf 'Wake-word setup failed: %s\n' "$*" >&2; exit 1; }
run() {
    if (( DRY_RUN )); then
        printf 'Would run:'
        printf ' %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

while (( $# )); do
    case "$1" in
        --wheel|--wheel-sha256|--torch-wheel|--torch-sha256|--python|--venv|--build-dir|--signal-library)
            (( $# >= 2 )) || die "$1 requires a value"
            case "$1" in
                --wheel) WHEEL=$2 ;;
                --wheel-sha256) WHEEL_SHA256=${2,,} ;;
                --torch-wheel) TORCH_WHEEL=$2 ;;
                --torch-sha256) TORCH_SHA256=${2,,} ;;
                --python) PYTHON=$2 ;;
                --venv) VENV=$2 ;;
                --build-dir) BUILD_DIR=$2 ;;
                --signal-library) SIGNAL_LIBRARY=$2 ;;
            esac
            shift 2 ;;
        --reuse-venv) REUSE_VENV=1; shift ;;
        --install-system-packages) SYSTEM_PACKAGES=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

[[ $(uname -s) == Linux ]] || die 'Run this helper on the Linux gadget host.'
[[ -n $WHEEL && -f $WHEEL && $WHEEL == *.whl ]] || die 'Supply the actual trusted ExecuTorch wheel with --wheel FILE.whl.'
[[ -n $VENV && -n $BUILD_DIR && -n $PYTHON ]] || die 'Interpreter and directory values must not be empty.'
WHEEL=$(CDPATH='' cd -- "$(dirname -- "$WHEEL")" && printf '%s/%s' "$PWD" "$(basename -- "$WHEEL")")
case "$VENV" in /*) ;; *) VENV="$PWD/$VENV" ;; esac
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac
verify_checksum() {
    local file=$1 expected=$2 actual
    [[ -n $expected ]] || return 0
    [[ $expected =~ ^[0-9a-f]{64}$ ]] || die 'Expected SHA-256 must contain 64 hexadecimal characters.'
    command -v sha256sum >/dev/null || die 'sha256sum is needed to verify the supplied checksum.'
    actual=$(sha256sum -- "$file" | awk '{print $1}')
    [[ $actual == "$expected" ]] || die 'The supplied wheel checksum does not match.'
}
verify_checksum "$WHEEL" "$WHEEL_SHA256"
if [[ -n $TORCH_WHEEL ]]; then
    [[ -f $TORCH_WHEEL && $TORCH_WHEEL == *.whl ]] || die 'The supplied Torch wheel must be an existing .whl file.'
    TORCH_WHEEL=$(CDPATH='' cd -- "$(dirname -- "$TORCH_WHEEL")" && printf '%s/%s' "$PWD" "$(basename -- "$TORCH_WHEEL")")
    verify_checksum "$TORCH_WHEEL" "$TORCH_SHA256"
elif [[ -n $TORCH_SHA256 ]]; then
    die '--torch-sha256 requires --torch-wheel.'
fi
[[ ! -e $VENV || $REUSE_VENV == 1 ]] || die 'The environment already exists; choose a new --venv or explicitly allow --reuse-venv.'

if (( SYSTEM_PACKAGES )); then
    [[ -r /etc/os-release ]] || die 'Cannot identify the Linux distribution.'
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ $(id -u) == 0 ]]; then privilege=(); else privilege=(sudo); fi
    case "${ID:-}" in
        debian|ubuntu|raspbian)
            run "${privilege[@]}" apt-get update
            run "${privilege[@]}" apt-get install --no-install-recommends build-essential python3-venv python3-pip libportaudio2 ;;
        fedora)
            run "${privilege[@]}" dnf install gcc python3-pip portaudio ;;
        arch)
            run "${privilege[@]}" pacman -S --needed gcc python python-pip portaudio ;;
        *) die 'Automatic system prerequisites support Debian/Ubuntu, Fedora, or Arch; install a C compiler, Python venv/pip, and PortAudio using your distro instructions.' ;;
    esac
fi
if [[ -n $SIGNAL_LIBRARY ]]; then
    [[ -f $SIGNAL_LIBRARY ]] || die 'The supplied signal library does not exist.'
    SIGNAL_LIBRARY=$(CDPATH='' cd -- "$(dirname -- "$SIGNAL_LIBRARY")" && printf '%s/%s' "$PWD" "$(basename -- "$SIGNAL_LIBRARY")")
fi
if (( ! DRY_RUN )); then
    command -v "$PYTHON" >/dev/null || die 'The requested interpreter is unavailable; choose a Python supported by the wheel.'
    if [[ -z $SIGNAL_LIBRARY ]]; then
        command -v "${CC:-cc}" >/dev/null || die 'A C compiler is required for the small signal library; install prerequisites explicitly or supply --signal-library.'
    fi
fi

printf 'Using the supplied wheel; compatibility is checked, not inferred from its revision.\n'
if [[ ! -x $VENV/bin/python ]]; then
    run env -u PYTHONPATH "$PYTHON" -m venv "$VENV"
fi
if [[ -n $TORCH_WHEEL ]]; then
    # One resolver transaction prevents the ET wheel from replacing the explicit Torch wheel.
    run env -u PYTHONPATH "$VENV/bin/python" -m pip install "$TORCH_WHEEL" "$WHEEL" "${ROOT}[voice]"
else
    if (( DRY_RUN )); then
        printf 'A compatible Torch installation must already exist; supply --torch-wheel for a fresh environment.\n'
        run env -u PYTHONPATH "$VENV/bin/python" -m pip install --constraint "$VENV/.muse-torch-constraint.txt" "$WHEEL" "${ROOT}[voice]"
    else
        env -u PYTHONPATH "$VENV/bin/python" -c 'import torch' >/dev/null 2>&1 ||
            die 'A compatible Torch installation is missing or broken; supply --torch-wheel. The helper does not choose an unrelated latest Torch ABI.'
        constraint=$(mktemp "$VENV/.muse-torch-constraint.XXXXXX")
        trap 'rm -f -- "$constraint"' EXIT
        env -u PYTHONPATH "$VENV/bin/python" -c 'from importlib.metadata import version; print("torch==" + version("torch"))' > "$constraint"
        run env -u PYTHONPATH "$VENV/bin/python" -m pip install --constraint "$constraint" "$WHEEL" "${ROOT}[voice]"
    fi
fi
# Same version numbers can describe different compiled sources. In reused
# environments, install the exact supplied root wheels after resolving their deps.
if (( REUSE_VENV )); then
    roots=("$WHEEL")
    [[ -z $TORCH_WHEEL ]] || roots+=("$TORCH_WHEEL")
    run env -u PYTHONPATH "$VENV/bin/python" -m pip install --no-deps --force-reinstall "${roots[@]}"
fi
if [[ -z $SIGNAL_LIBRARY ]]; then
    run bash "$ROOT/wakeword/build_signal.sh" "$BUILD_DIR"
    SIGNAL_LIBRARY="$BUILD_DIR/libmuse_wakeword_signal.so"
fi
run env -u PYTHONPATH "MUSE_WAKEWORD_LIBRARY=$SIGNAL_LIBRARY" \
    "$VENV/bin/python" -m musegadget.wakeword_check --check-audio
if (( DRY_RUN )); then
    printf 'Dry run only: no environment, system packages, audio, or services were changed.\n'
else
    printf 'Wheel and local detector checks passed. Wake-word listening remains OFF.\n'
    printf 'Environment: %s\nSignal library: %s\n' "$VENV" "$SIGNAL_LIBRARY"
fi
