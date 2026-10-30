#!/bin/sh
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

# Builds tinyhost: the gadget's own decoder (tinyh264) on the host, for score.py.
set -e
here=$(cd "$(dirname "$0")" && pwd)
tiny="$here/../../../../esp32/components/tinyh264/src"
cc -O2 -w -I"$tiny" -o "$here/tinyhost" "$here/tinyhost.c" "$tiny"/*.c
