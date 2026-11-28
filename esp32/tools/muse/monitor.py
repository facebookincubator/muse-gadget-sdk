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

"""Reset a board and print its serial log: tools/muse/monitor.py <port> [secs]"""
import os
import sys
import time

import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import redact  # noqa: E402  (tools/redact.py: never print a secret the board's output carries)

s = serial.Serial(sys.argv[1], 115200, timeout=0.2)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # reset
end = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 8)
out = b''
while time.time() < end:
    out += s.read(4096)
sys.stdout.write(redact.redact_text(out.decode('utf-8', 'replace')))
