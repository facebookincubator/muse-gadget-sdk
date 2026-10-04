#!/bin/sh
# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
set -eu
umask 077
mkdir -p /var/lib/musegadget /run/musegadget
chmod 700 /var/lib/musegadget
exec musegadget "$@"
