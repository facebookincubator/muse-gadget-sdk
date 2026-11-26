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

"""components/muse_image_id: the board description every image carries in
ESP-IDF's custom app description section, and muse_image_check(), which the
updaters (main/ota.c's same-board opt-in, mg_smp_esp.c) run on an image's
first bytes before writing anything."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "muse_image_id"


def head(chip: int = 9, product: str | None = "W175", board: str = "waveshare_s3_175c", flags: int = 3,
         version: str = "1.4.2+gabc", magic: int = 0x4442474D, raw_product: bytes | None = None) -> bytes:
    """An app image's first 352 bytes: header (chip id at 12), esp_app_desc_t
    at 32 (version at 48), the board description at 288."""
    b = bytearray(352)
    b[0] = 0xE9
    b[12:14] = struct.pack("<H", chip)
    b[32:36] = struct.pack("<I", 0xABCD5432)
    b[48:48 + len(version)] = version.encode()
    if product is not None:
        prod = raw_product if raw_product is not None else product.encode().ljust(16, b"\0")
        b[288:352] = struct.pack("<IBBH", magic, 1, flags, chip) + prod + board.encode().ljust(32, b"\0") + bytes(8)
    return bytes(b)


class ImageCheck(unittest.TestCase):
    def run_checks(self, cases: list[tuple[str, bytes, bool]]) -> list[str]:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            self.skipTest("C compiler unavailable")
        with tempfile.TemporaryDirectory() as name:
            tmp = Path(name)
            (tmp / "sdkconfig.h").write_text(
                "#define CONFIG_IDF_FIRMWARE_CHIP_ID 9\n"
                "#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE \"W175\"\n"
                "#define CONFIG_MUSE_BOARD_ID \"waveshare_s3_175c\"\n"
                "#define CONFIG_MUSE_GADGET_BLE_AUDIO 1\n#define CONFIG_MUSE_GADGET_BLE_SMP 1\n")
            arrays, calls = [], []
            for i, (_, data, board) in enumerate(cases):
                arrays.append(f"static const unsigned char h{i}[] = {{{','.join(str(x) for x in data) or '0'}}};")
                calls.append(f"    show(h{i}, {len(data)}, {int(board)});")
            src = tmp / "check.c"
            src.write_text('#include <stdio.h>\n#include "muse_image_id.h"\n' + "\n".join(arrays) + """
static void show(const unsigned char *h, size_t n, int board) {
    muse_image_info_t info;
    muse_image_verdict_t v = muse_image_check(h, n, board, &info);
    char who[112];
    muse_image_describe(&info, who, sizeof(who));
    printf("%d|%s|%s\\n", (int)v, muse_image_verdict_text(v), who);
}
int main(void) {
    printf("self %u %s %s %u\\n", muse_board_desc.chip_id, muse_board_desc.product_code,
           muse_board_desc.board_id, muse_board_desc.flags);
""" + "\n".join(calls) + "\n    return 0;\n}\n")
            exe = tmp / "check"
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(tmp), "-I",
                                str(COMP / "include"), str(src), str(COMP / "muse_image_id.c"), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(out[0], "self 9 W175 waveshare_s3_175c 3")
        return out[1:]

    def test_verdicts(self) -> None:
        cases = [
            ("this board's", head(), True),
            ("this board's, chip only", head(product="", board="none", flags=0), False),
            ("C5 Home Link", head(chip=23, product="", board="none", flags=0), True),
            ("S3 Home Link", head(product="", board="none", flags=0), True),
            ("another product", head(product="SCW1", board="sensecap_watcher"), True),
            ("another profile, same code", head(board="waveshare_s3_175"), True),
            ("older build", head(product=None), True),
            ("older build, chip only", head(product=None), False),
            ("no BLE update", head(flags=1), True),
            ("short", head()[:351], True),
            ("not an app", b"\0" + head()[1:], True),
            ("bad magic", head(magic=0x12345678), True),
            ("unterminated product", head(raw_product=b"W175" + b"X" * 12), True),
            ("product prefix", head(product="W17"), True),
            ("hostile text", head(version="\x1b[31mred", product="W\x01", board="x"), True),
        ]
        out = self.run_checks(cases)
        got = {name: line for (name, _, _), line in zip(cases, out)}
        self.assertEqual(got["this board's"], "0|this board's|chip 9, W175 waveshare_s3_175c, version 1.4.2+gabc")
        self.assertTrue(got["this board's, chip only"].startswith("0|"))
        self.assertEqual(got["C5 Home Link"], "3|built for another chip|chip 23, (no product code) none, version 1.4.2+gabc")
        self.assertTrue(got["S3 Home Link"].startswith("5|built for another board|chip 9, (no product code) none"))
        self.assertTrue(got["another product"].startswith("5|built for another board|chip 9, SCW1 sensecap_watcher"))
        self.assertTrue(got["another profile, same code"].startswith("5|"))
        self.assertTrue(got["older build"].startswith("4|no board description"))
        self.assertTrue(got["older build"].endswith("|chip 9, no board description, version 1.4.2+gabc"))
        self.assertTrue(got["older build, chip only"].startswith("0|"))
        self.assertTrue(got["no BLE update"].startswith("6|it has no Bluetooth update"))
        self.assertTrue(got["short"].startswith("1|"))
        self.assertTrue(got["not an app"].startswith("2|"))
        self.assertTrue(got["bad magic"].startswith("4|"))
        self.assertTrue(got["unterminated product"].startswith("4|"))
        self.assertTrue(got["product prefix"].startswith("5|"))
        self.assertEqual(got["hostile text"], "5|built for another board|chip 9, W? x, version ?[31mred")


if __name__ == "__main__":
    unittest.main()
