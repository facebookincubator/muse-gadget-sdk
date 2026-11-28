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

import hashlib
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
# The check as firmware without the device model shipped it (muse_image_id.c and .h before the model, unchanged):
# what a device that runs that firmware does with a newer image.
V1 = Path(__file__).resolve().parent / "muse_image_id_v1"


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


def model_hash(model: str) -> str:
    """CMakeLists.txt's MUSE_DEVICE_MODEL_HASH for a CONFIG_MUSE_DEVICE_MODEL_ID: SHA-256's first 8 bytes."""
    return ",".join(f"0x{b:02x}" for b in hashlib.sha256(model.encode()).digest()[:8])


LITE, DEVKITC = "muse-gadget-seeed-respeaker-lite", "muse-gadget-espressif-s3-devkitc-1"


class DeviceModel(unittest.TestCase):
    """The device model in the 8 bytes the first layout left reserved: the firmware before it (V1) and
    this one, each built as an S3 Home Link board (no product code, board "none") and as the 1.75C
    gadget, check each other's images. The check runs in the firmware on the device, so an image
    with a model must pass the older firmware's check, and this firmware compares models only when
    both carry one."""

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler unavailable")
        cls.cc = cc
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exes = {}

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def build(self, firmware: str, board: str, model: str = "") -> Path:
        """firmware "v1" (before the model) or "new"; board "link" (an S3 Home Link board) or "gadget"
        (the 1.75C gadget); model: CONFIG_MUSE_DEVICE_MODEL_ID ("" for none)."""
        key = (firmware, board, model)
        if key in self.exes:
            return self.exes[key]
        d = Path(self.tmp.name) / f"b{len(self.exes)}"
        d.mkdir()
        sdk = "#define CONFIG_IDF_FIRMWARE_CHIP_ID 9\n"
        if board == "gadget":
            sdk += ('#define CONFIG_HOMEHUB_BLE_PRODUCT_CODE "W175"\n#define CONFIG_MUSE_BOARD_ID "waveshare_s3_175c"\n'
                    "#define CONFIG_MUSE_GADGET_BLE_AUDIO 1\n#define CONFIG_MUSE_GADGET_BLE_SMP 1\n")
        if model:
            self.assertEqual(firmware, "new")
            sdk += f"#define MUSE_DEVICE_MODEL_HASH {model_hash(model)}\n"
        (d / "sdkconfig.h").write_text(sdk)
        (d / "main.c").write_text(r'''
#include <stdio.h>
#include <string.h>
#include "muse_image_id.h"
/* "desc": this build's description as hex; else checks the image head in argv[1] (hex): board, then
 * (where there is one) model-only, as "BOARD MODEL" verdict numbers. */
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "desc")) {
        const unsigned char *p = (const unsigned char *)&muse_board_desc;
        for (size_t i = 0; i < sizeof(muse_board_desc); i++) printf("%02x", p[i]);
        putchar('\n');
        return 0;
    }
    unsigned char h[MUSE_IMAGE_HEAD_BYTES];
    size_t n = strlen(argv[1]) / 2;
    if (n != sizeof(h)) return 2;
    for (size_t i = 0; i < n; i++) { unsigned b; sscanf(argv[1] + 2 * i, "%2x", &b); h[i] = (unsigned char)b; }
    printf("%d", (int)muse_image_check(h, n, true, NULL));
#ifdef MUSE_IMAGE_WRONG_MODEL_CHECKED
    printf(" %d", (int)muse_image_check_model(h, n, NULL));
#endif
    putchar('\n');
    return 0;
}
''')
        comp, inc = (V1, V1) if firmware == "v1" else (COMP, COMP / "include")
        exe = d / "fw"
        r = subprocess.run([*self.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(d), "-I", str(inc),
                            *(["-DMUSE_IMAGE_WRONG_MODEL_CHECKED"] if firmware == "new" else []),
                            str(d / "main.c"), str(comp / "muse_image_id.c"), "-o", str(exe)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.exes[key] = exe
        return exe

    def image(self, firmware: str, board: str, model: str = "") -> bytes:
        """An image head carrying the description that build puts in its own image."""
        desc = subprocess.run([str(self.build(firmware, board, model)), "desc"], capture_output=True, text=True,
                              check=True).stdout.strip()
        b = bytearray(head())
        b[288:352] = bytes.fromhex(desc)
        return bytes(b)

    def check(self, firmware: str, board: str, model: str, img: bytes) -> list[int]:
        out = subprocess.run([str(self.build(firmware, board, model)), img.hex()], capture_output=True, text=True,
                             check=True).stdout.split()
        return [int(v) for v in out]

    def test_the_description_keeps_the_first_layout(self) -> None:
        new = self.image("new", "link", LITE)[288:352]
        old = self.image("v1", "link")[288:352]
        self.assertEqual(new[:5] + new[6:56], old[:5] + old[6:56])   # magic, layout 1, chip, product, board
        self.assertEqual(new[5], old[5] | 0x04)                       # one more flag: the model
        self.assertEqual(old[56:], bytes(8))                # the first layout's reserved bytes: zeros
        self.assertEqual(new[56:], hashlib.sha256(LITE.encode()).digest()[:8])
        self.assertEqual(self.image("new", "link")[288:352], old)   # without a model: byte for byte the same

    def test_rows(self) -> None:
        OK, WRONG_MODEL = 0, 7
        GADGET = "muse-gadget-waveshare-s3-175c"
        # Each row: [the board check (BLE updates, the gadgets' Wi-Fi OTA), the model-only check (a Home
        # Link build's Wi-Fi OTA, main/ota.c)]; the old firmware has the board check only.
        rows = [
            # wrong model refused
            ("new link, another model's image", ("new", "link", LITE), self.image("new", "link", DEVKITC),
             [WRONG_MODEL, WRONG_MODEL]),
            ("new gadget, another model's image", ("new", "gadget", GADGET),
             self.image("new", "gadget", "muse-gadget-other"), [WRONG_MODEL, WRONG_MODEL]),
            # old firmware takes the new image (the upgrade path)
            ("old link, new image", ("v1", "link", ""), self.image("new", "link", LITE), [OK]),
            ("old gadget, new image", ("v1", "gadget", ""), self.image("new", "gadget", GADGET), [OK]),
            # (and can't tell another model's: what the model closes from here on)
            ("old link, another model's new image", ("v1", "link", ""), self.image("new", "link", DEVKITC), [OK]),
            # new firmware, new image, same model
            ("new link, same model", ("new", "link", LITE), self.image("new", "link", LITE), [OK, OK]),
            ("new gadget, same model", ("new", "gadget", GADGET), self.image("new", "gadget", GADGET), [OK, OK]),
            # new firmware, an old image (no model)
            ("new link, old image", ("new", "link", LITE), self.image("v1", "link"), [OK, OK]),
            ("new gadget, old image", ("new", "gadget", GADGET), self.image("v1", "gadget"), [OK, OK]),
            # a new build without a model judges as before
            ("new gadget without a model, a new image", ("new", "gadget", ""),
             self.image("new", "gadget", "muse-gadget-other"), [OK, OK]),
        ]
        for name, fw, img, want in rows:
            with self.subTest(name):
                self.assertEqual(self.check(*fw, img), want)


if __name__ == "__main__":
    unittest.main()
