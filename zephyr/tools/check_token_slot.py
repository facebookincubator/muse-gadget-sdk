#!/usr/bin/env python3
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
"""Build check for CONFIG_MG_SDK_TOKEN_SLOT: the image must hold the token
slot's marker exactly once (the updater fills the slot it finds by the
marker, and refuses an image with none or several; AGENTS.md, SDK token
slot). Run after the link on zephyr.bin; exits 1 otherwise."""
import sys

MARKER = b"MG-SDK-TOKEN-SLOT-v1:"

data = open(sys.argv[1], "rb").read()
n = data.count(MARKER)
if n != 1:
    sys.exit(f"token slot: the marker appears {n} times in {sys.argv[1]}, not once")
print(f"token slot: marker once, at offset 0x{data.find(MARKER):x}")
