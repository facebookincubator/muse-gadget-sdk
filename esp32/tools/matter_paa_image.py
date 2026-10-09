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

"""Build the paa_cert SPIFFS image from PAA certificates.

Usage: matter_paa_image.py PATH [PATH ...] [-o OUT]

Each PATH is a .der certificate or a directory of them. Needs IDF_PATH (run
ESP-IDF's export.sh first). The same certificates always give the same image.
"""

import argparse
import hashlib
import os
import sys
from pathlib import Path

ESP32 = Path(__file__).resolve().parent.parent
SIZE = 0x20000  # the paa_cert partition in partitions_matter_4mb.csv and _8mb.csv

parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
parser.add_argument("paths", nargs="+", metavar="PATH")
parser.add_argument("-o", "--output", type=Path, default=ESP32 / "components/esp32_matter_controller/paa/paa_cert.bin")
args = parser.parse_args()

sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "components", "spiffs"))
import spiffsgen  # noqa: E402

files = [f for p in map(Path, args.paths) for f in (sorted(p.glob("*.der")) if p.is_dir() else [p])]
# SPIFFS names are at most 31 characters; the trust store finds a certificate
# by its key ID, not its name.
certs = {"/" + hashlib.md5(f.read_bytes()).hexdigest()[:16] + ".der": f for f in files}

# IDF's SPIFFS defaults, which the Matter overlays keep.
config = spiffsgen.SpiffsBuildConfig(256, spiffsgen.SPIFFS_PAGE_IX_LEN, 4096, spiffsgen.SPIFFS_BLOCK_IX_LEN, 4, 32,
                                     spiffsgen.SPIFFS_OBJ_ID_LEN, spiffsgen.SPIFFS_SPAN_IX_LEN, True, True, "little",
                                     True, True, False)
fs = spiffsgen.SpiffsFS(SIZE, config)
for name in sorted(certs):
    fs.create_file(name, str(certs[name]))
image = fs.to_binary()
args.output.write_bytes(image)
print(f"{args.output}: {len(certs)} certificates, sha256 {hashlib.sha256(image).hexdigest()}")
