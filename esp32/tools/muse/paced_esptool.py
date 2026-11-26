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

"""esptool with writes paced to the UART: tools/muse/paced_esptool.py <esptool args>

Takes the same arguments as `python -m esptool`. Some USB-UART bridges (the
SenseCAP Watcher's CH342 on macOS) have no flow control and drop bytes when a
whole packet arrives faster than the UART drains it, so esptool fails with
"Failed to write to target RAM (result was 0107: Checksum error)" while
uploading its stub, or "0105: The format of the received message is invalid"
on the first flash block. This sends 64 bytes at a time, at the line rate.
"""
import os
import sys
import time

import serial

CHUNK = 64
_write = serial.Serial.write


def paced_write(self, data):
    data = bytes(data)
    n = 0
    for i in range(0, len(data), CHUNK):
        chunk = data[i:i + CHUNK]
        n += _write(self, chunk)
        self.flush()
        time.sleep(len(chunk) * 10 / self.baudrate)  # 10 bits a byte on the wire
    return n


serial.Serial.write = paced_write

import esptool  # noqa: E402  (after the patch, so esptool's port uses it)

# A read-flash dump holds NVS (identity, tokens): every file this writes is
# 0600.
os.umask(0o077)

if __name__ == "__main__":
    sys.exit(esptool._main())
