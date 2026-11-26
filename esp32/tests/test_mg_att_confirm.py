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
"""mg_att_confirm.c: a phone's indication confirmed (0x1E), never refused.

Without NimBLE's GATT client an incoming indication (0x1D) was answered with
an Error Response for opcode 0x1D, which Android's GATT client took for the
answer to its service discovery (0 services, the gadget unusable)."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MG = ROOT / "components/muse_gadget_ble"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "host/ble_hs.h"
#include "os/os_mbuf.h"
int __wrap_ble_att_svr_tx_error_rsp(uint16_t, uint16_t, struct os_mbuf *, uint8_t, uint16_t, uint8_t);
static uint8_t sent[8]; static int nsent, nreal, prepare_fails;
int __real_ble_att_svr_tx_error_rsp(uint16_t c, uint16_t cid, struct os_mbuf *t, uint8_t op, uint16_t h, uint8_t e)
{ (void)c; (void)cid; (void)t; (void)h; nreal++; sent[0] = 0x01; sent[1] = op; sent[2] = e; nsent = 3; return 0; }
void *ble_att_cmd_prepare(uint8_t opcode, size_t len, struct os_mbuf *txom)
{ (void)len; if (prepare_fails) return NULL; txom->data[0] = opcode; txom->len = 1; return txom->data + 1; }
int ble_att_tx(uint16_t c, uint16_t cid, struct os_mbuf *txom)
{ (void)c; (void)cid; memcpy(sent, txom->data, txom->len); nsent = txom->len; return 0; }
int main(void)
{
    struct os_mbuf m = { 0 };
    int fails = 0;
    /* An indication (0x1D) the host has no handler for: confirmed. */
    int rc = __wrap_ble_att_svr_tx_error_rsp(1, 4, &m, 0x1D, 0, 0x06);
    fails += !(rc == 0 && nsent == 1 && sent[0] == 0x1E && nreal == 0);
    /* Any other refusal goes out as NimBLE sends it. */
    rc = __wrap_ble_att_svr_tx_error_rsp(1, 4, &m, 0x12, 0x20, 0x03);
    fails += !(rc == 0 && nreal == 1 && sent[0] == 0x01 && sent[1] == 0x12 && sent[2] == 0x03);
    /* An indication refused for another reason: untouched too. */
    rc = __wrap_ble_att_svr_tx_error_rsp(1, 4, &m, 0x1D, 0x10, 0x0F);
    fails += !(nreal == 2);
    /* No room for the reply: the error's own failure code. */
    prepare_fails = 1;
    rc = __wrap_ble_att_svr_tx_error_rsp(1, 4, &m, 0x1D, 0, 0x06);
    fails += !(rc == BLE_HS_ENOMEM);
    printf(fails ? "FAIL %d\n" : "PASS att_confirm\n", fails);
    return fails != 0;
}
"""


class AttConfirmTest(unittest.TestCase):
    def test_indication_confirmed(self) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / "host").mkdir()
            (t / "os").mkdir()
            (t / "host/ble_hs.h").write_text("#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define BLE_HS_ENOMEM 6\n")
            (t / "os/os_mbuf.h").write_text("#pragma once\n#include <stdint.h>\nstruct os_mbuf { uint8_t data[8]; int len; };\n")
            (t / "h.c").write_text(HARNESS)
            exe = t / "h"
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", tmp, str(t / "h.c"),
                                str(MG / "mg_att_confirm.c"), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertIn("PASS att_confirm", r.stdout, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
