#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND.

# Optional Hey Muse prerequisites only: no pairing, credentials, or service
# changes, and the microphone stays closed.
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
PYTHON=python3
VENV="$ROOT/.venv-wakeword"
BUILD_DIR="$ROOT/wakeword/build"
DEPS=""
LIBRARY=""
DRY_RUN=0
SYSTEM_PACKAGES=0
REUSE_VENV=0

usage() {
    printf '%s\n' \
      'Usage: setup.sh [options]' \
      '  --python COMMAND      Interpreter for the environment (default: python3)' \
      '  --venv DIRECTORY      Isolated environment (default: linux/.venv-wakeword)' \
      '  --build-dir DIRECTORY Native library output (default: linux/wakeword/build)' \
      '  --deps DIRECTORY      Use these registry archives instead of downloading them' \
      '  --library FILE        Use an already built libmuse_wakeword.so instead of building' \
      '  --reuse-venv          Allow package changes in an existing environment' \
      '  --install-system-packages  Install a compiler, unzip, venv and PortAudio with sudo' \
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
        --python|--venv|--build-dir|--deps|--library)
            (( $# >= 2 )) || die "$1 requires a value"
            case "$1" in
                --python) PYTHON=$2 ;;
                --venv) VENV=$2 ;;
                --build-dir) BUILD_DIR=$2 ;;
                --deps) DEPS=$2 ;;
                --library) LIBRARY=$2 ;;
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
[[ -n $VENV && -n $BUILD_DIR && -n $PYTHON ]] || die 'Interpreter and directory values must not be empty.'
case "$VENV" in /*) ;; *) VENV="$PWD/$VENV" ;; esac
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac
[[ ! -e $VENV || $REUSE_VENV == 1 ]] || die 'The environment already exists; choose a new --venv or explicitly allow --reuse-venv.'
if [[ -n $LIBRARY ]]; then
    [[ -f $LIBRARY ]] || die 'The supplied library does not exist.'
    LIBRARY=$(CDPATH='' cd -- "$(dirname -- "$LIBRARY")" && printf '%s/%s' "$PWD" "$(basename -- "$LIBRARY")")
fi
if [[ -n $DEPS ]]; then
    [[ -d $DEPS ]] || die 'The --deps directory does not exist.'
fi

if (( SYSTEM_PACKAGES )); then
    [[ -r /etc/os-release ]] || die 'Cannot identify the Linux distribution.'
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ $(id -u) == 0 ]]; then privilege=(); else privilege=(sudo); fi
    case "${ID:-}" in
        debian|ubuntu|raspbian)
            run "${privilege[@]}" apt-get update
            run "${privilege[@]}" apt-get install --no-install-recommends build-essential unzip curl python3-venv python3-pip libportaudio2 ;;
        fedora)
            run "${privilege[@]}" dnf install gcc gcc-c++ unzip curl python3-pip portaudio ;;
        arch)
            run "${privilege[@]}" pacman -S --needed gcc unzip curl python python-pip portaudio ;;
        *) die 'Automatic system prerequisites support Debian/Ubuntu, Fedora, or Arch; install a C/C++ compiler, unzip, curl, Python venv/pip, and PortAudio using your distro instructions.' ;;
    esac
fi
if (( ! DRY_RUN )); then
    command -v "$PYTHON" >/dev/null || die 'The requested interpreter is unavailable.'
    if [[ -z $LIBRARY ]]; then
        tools=("${CC:-cc}" "${CXX:-c++}" unzip sha256sum)
        [[ -n $DEPS ]] || tools+=(curl)
        for tool in "${tools[@]}"; do
            command -v "$tool" >/dev/null || die "$tool is required to build the library; install prerequisites explicitly or supply --library."
        done
    fi
fi

if [[ ! -x $VENV/bin/python ]]; then
    run env -u PYTHONPATH "$PYTHON" -m venv "$VENV"
fi
run env -u PYTHONPATH "$VENV/bin/python" -m pip install "${ROOT}[voice]"
if [[ -z $LIBRARY ]]; then
    build=(bash "$ROOT/wakeword/build.sh" --out "$BUILD_DIR")
    [[ -z $DEPS ]] || build+=(--deps "$DEPS")
    run "${build[@]}"
    LIBRARY="$BUILD_DIR/libmuse_wakeword.so"
fi
# The environment keeps its own copy, where musegadget.wakeword looks first
# (after MUSE_WAKEWORD_LIBRARY); the check finds it the same way.
INSTALLED="$VENV/lib/musegadget/libmuse_wakeword.so"
run install -D -m 0644 "$LIBRARY" "$INSTALLED"
run env -u PYTHONPATH -u MUSE_WAKEWORD_LIBRARY \
    "$VENV/bin/python" -m musegadget.wakeword_check --check-audio
if (( DRY_RUN )); then
    printf 'Dry run only: no environment, system packages, audio, or services were changed.\n'
else
    printf 'Local detector checks passed. Wake-word listening remains OFF.\n'
    printf 'Environment: %s\nLibrary: %s\n' "$VENV" "$INSTALLED"
fi
