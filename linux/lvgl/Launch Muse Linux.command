#!/bin/zsh
set -e
cd "${0:A:h}"
exec python3 launch.py
