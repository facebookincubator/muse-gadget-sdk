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
"""Reset the board and stream its log line by line until BENCH done.

serial_log.py PORT SECS — unlike tools/muse/monitor.py, output is flushed
as it arrives, so a host script can react to bench prompts."""
import sys
import time

import serial

port, secs = sys.argv[1], float(sys.argv[2])
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
end = time.monotonic() + secs
buf = b""
while time.monotonic() < end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        print(text, flush=True)
        if text.startswith("BENCH done"):
            sys.exit(0)
