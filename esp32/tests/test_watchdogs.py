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

"""Watchdogs on in the BLE gadget profiles, and the crash record.

- Every overlay that turns on CONFIG_MUSE_GADGET_BLE_AUDIO turns both
  watchdogs on: the interrupt watchdog at 1.5 s, the task watchdog at 15 s on
  both idle tasks, a panic. sdkconfig.defaults keeps them off for other boards.
- No overlay or target defaults file turns them off or loosens them, and the
  build stops a BLE gadget build whose generated sdkconfig has them off.
- A crash leaves a record in RTC memory (components/muse_restart/
  muse_crashrec*.c): the kind, the core, the pc, the uptime, the tasks running
  and, for the task watchdog, the ones that didn't reset it. The next boot logs
  it after its boot line; "guard?" repeats the last one. The record's pure part
  runs here against a harness.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "muse_restart"

WDT_ON = (
    "CONFIG_ESP_INT_WDT=y",
    "CONFIG_ESP_INT_WDT_TIMEOUT_MS=1500",
    "CONFIG_ESP_INT_WDT_CHECK_CPU1=y",
    "CONFIG_ESP_TASK_WDT_EN=y",
    "CONFIG_ESP_TASK_WDT_INIT=y",
    "CONFIG_ESP_TASK_WDT_PANIC=y",
    "CONFIG_ESP_TASK_WDT_TIMEOUT_S=15",
    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y",
    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y",
)

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "muse_crashrec.h"

static void feed(muse_crashrec_t *r, const char *const *pieces, int n)
{
    unsigned seen = 0;
    r->starved[0] = '\0';
    for (int i = 0; i < n; i++) {
        muse_crashrec_add_starved(r, &seen, pieces[i]);
    }
}

int main(void)
{
    muse_crashrec_t r;
    char line[256];
    memset(&r, 0xA5, sizeof(r));                       /* what a power-on can leave */
    printf("garbage_valid %d\n", muse_crashrec_valid(&r));
    memset(&r, 0, sizeof(r));
    printf("zero_valid %d\n", muse_crashrec_valid(&r));
    muse_crashrec_reset(&r);
    printf("reset_valid %d kind %u count %u\n", muse_crashrec_valid(&r), (unsigned)r.kind, (unsigned)r.count);

    /* the task watchdog's ISR, then the panic */
    const char *const two[] = { "Task watchdog got triggered. ...:", "\n - ", "IDLE0", " (CPU 0)",
                                "\n - ", "muse_voice", " (CPU 1)" };
    feed(&r, two, 7);
    printf("starved %s|\n", r.starved);
    r.fresh = 1;
    r.count++;
    r.kind = MUSE_CRASH_TASK_WDT;
    r.core = 0;
    r.pc = 0x4200abcd;
    r.up_us = 312456789;
    strcpy(r.running[0], "mg_face");
    strcpy(r.running[1], "IDLE1");
    muse_crashrec_seal(&r);
    printf("tw_valid %d\n", muse_crashrec_valid(&r));
    muse_crashrec_describe(&r, line, sizeof(line));
    printf("tw_line %s\n", line);
    r.pc ^= 1;                                         /* a flipped bit */
    printf("flipped_valid %d\n", muse_crashrec_valid(&r));

    /* an interrupt watchdog, a single-core chip: no task on core 1 */
    muse_crashrec_reset(&r);
    r.fresh = 1;
    r.count = 2;
    r.kind = MUSE_CRASH_INT_WDT;
    r.core = 0;
    r.pc = 0x40380010;
    r.up_us = 1500000;
    strcpy(r.running[0], "lvgl");
    muse_crashrec_seal(&r);
    muse_crashrec_describe(&r, line, sizeof(line));
    printf("iw_line %s\n", line);

    /* many starved users: truncated, never overrun */
    const char *const many[] = { "cap", "\n - ", "aaaaaaaaaaaaaaa", " (CPU 0)", "\n - ", "bbbbbbbbbbbbbbb", " (CPU 0)",
                                 "\n - ", "ccccccccccccccc", " (CPU 0)", "\n - ", "ddddddddddddddd", " (CPU 1)",
                                 "\n - ", "eeeeeeeeeeeeeee", " (CPU 1)" };
    feed(&r, many, 16);
    printf("many_len %u nul %d\n", (unsigned)strlen(r.starved), r.starved[MUSE_CRASHREC_STARVED - 1] == '\0');
    r.kind = 9;
    muse_crashrec_seal(&r);
    printf("bad_kind_valid %d\n", muse_crashrec_valid(&r));
    /* On the C6 a task watchdog panic comes as a fault (exception 4) after the
     * ISR's user handler and the TASK_WDT hint; an interrupt watchdog as exception 1
     * or with the INT_WDT hint; a real fault, an abort, a debug exception as they are. */
    printf("classify %u %u %u %u %u %u %u %u\n",
           (unsigned)muse_crashrec_classify(4, true, 0), (unsigned)muse_crashrec_classify(4, false, 6),
           (unsigned)muse_crashrec_classify(2, false, 0), (unsigned)muse_crashrec_classify(1, false, 0),
           (unsigned)muse_crashrec_classify(4, false, 5), (unsigned)muse_crashrec_classify(4, false, 0),
           (unsigned)muse_crashrec_classify(3, false, 4), (unsigned)muse_crashrec_classify(0, false, 0));
    return 0;
}
"""


def overlays():
    yield ROOT / "sdkconfig.defaults"
    yield from sorted(ROOT.glob("sdkconfig.defaults.*"))
    yield from sorted((ROOT / "devices").glob("sdkconfig.*"))
    yield from sorted((ROOT / "tools").rglob("sdkconfig.*"))


def ble_gadget_profiles():
    return [p for p in sorted((ROOT / "devices").glob("sdkconfig.*"))
            if re.search(r"(?m)^CONFIG_MUSE_GADGET_BLE_AUDIO=y$", p.read_text())]


class WatchdogConfigTest(unittest.TestCase):
    def test_ble_gadget_profiles_turn_both_on(self) -> None:
        profiles = ble_gadget_profiles()
        self.assertGreaterEqual(len(profiles), 4)
        for path in profiles:
            text = path.read_text()
            for line in WDT_ON:
                self.assertIn(line + "\n", text, path.name)
        defaults = (ROOT / "sdkconfig.defaults").read_text()
        self.assertIn("CONFIG_ESP_INT_WDT=n\n", defaults)
        self.assertIn("CONFIG_ESP_TASK_WDT_EN=n\n", defaults)

    def test_only_ble_gadget_profiles_change_the_shared_defaults(self) -> None:
        # The ATT MTU (247) and the first-boot speaker volume (50) too: every
        # other board keeps sdkconfig.muse's 256 and Kconfig's 70.
        for path in ble_gadget_profiles():
            text = path.read_text()
            self.assertIn("CONFIG_MUSE_DEFAULT_VOLUME=50\n", text, path.name)
            self.assertIn("CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247\n", text, path.name)
        self.assertIn("CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256\n", (ROOT / "devices/sdkconfig.muse").read_text())
        self.assertNotIn("ATT_PREFERRED_MTU", (ROOT / "sdkconfig.defaults").read_text())

    def test_stale_build_config_fails_a_ble_gadget_build(self) -> None:
        # cmake/validate_config.cmake on a generated sdkconfig's values: a BLE
        # gadget build with them off or looser stops; other builds go on.
        if shutil.which("cmake") is None:
            self.skipTest("cmake unavailable")
        base = ["-DCONFIG_CJSON_NESTING_LIMIT=16", "-DCONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384",
                "-DCONFIG_LWIP_TCP_WND_DEFAULT=16384", "-DCONFIG_MUSE_DEVICE_MODEL_ID=acme-lamp",
                "-DCONFIG_MUSE_DEVICE_MODEL_NAME=Acme Lamp", "-DCONFIG_FREERTOS_NUMBER_OF_CORES=2"]
        on = ["-D" + line for line in WDT_ON]
        off = ["-DCONFIG_ESP_INT_WDT=", "-DCONFIG_ESP_TASK_WDT_EN="]
        ble = ["-DCONFIG_MUSE_GADGET_BLE_AUDIO=y"]
        looser = [d for d in on if "TIMEOUT_S" not in d] + ["-DCONFIG_ESP_TASK_WDT_TIMEOUT_S=30"]
        no_cpu1 = [d for d in on if "IDLE_TASK_CPU1" not in d]
        for name, defs, ok in (("ble on", ble + on, True), ("ble off", ble + off, False),
                               ("ble looser", ble + looser, False), ("ble cpu1 unwatched", ble + no_cpu1, False),
                               ("other off", off, True), ("other on", on, True)):
            with self.subTest(name):
                ran = subprocess.run(["cmake", *base, *defs, "-P", str(ROOT / "cmake/validate_config.cmake")],
                                     capture_output=True, text=True)
                self.assertEqual(ran.returncode == 0, ok, ran.stderr)
                if not ok:
                    self.assertIn("both watchdogs on", ran.stderr)

    def test_panic_restarts(self) -> None:
        # A panic restarts the chip (the default); nothing halts it or waits for a debugger.
        for path in overlays():
            text = path.read_text()
            for bad in ("CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT=y", "CONFIG_ESP_SYSTEM_PANIC_GDBSTUB=y",
                        "CONFIG_BOOTLOADER_WDT_ENABLE=n", "CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE=y"):
                self.assertNotIn(bad, text, path.name)


class CrashRecordCoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "h.c"
            src.write_text(HARNESS)
            exe = Path(d) / "h"
            subprocess.run([cc, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Werror", "-I", str(COMP / "include"), str(src),
                            str(COMP / "muse_crashrec_core.c"), "-o", str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        cls.out = dict(line.split(" ", 1) for line in out.strip().splitlines())

    def test_watchdogs_are_never_told_as_faults(self) -> None:
        # TASK_WDT 3, INT_WDT 2, ABORT 4, FAULT 5, DEBUG 1 (muse_crash_kind_t).
        self.assertEqual(self.out["classify"], "3 3 3 2 2 5 4 1")
        dev = (COMP / "muse_crashrec.c").read_text()
        isr = dev[dev.index("void esp_task_wdt_isr_user_handler(void)"):]
        self.assertTrue(isr.split("{", 1)[1].lstrip().startswith("s_twdt_isr = true;"))
        wrap = dev[dev.index("__wrap_esp_panic_handler(panic_info_t *info)\n{"):]
        self.assertIn("muse_crashrec_classify((int)info->exception, twdt, (int)esp_reset_reason_get_hint())", wrap)
        self.assertNotIn("info->exception + 1", wrap)

    def test_only_a_written_record_passes(self) -> None:
        self.assertEqual(self.out["garbage_valid"], "0")
        self.assertEqual(self.out["zero_valid"], "0")
        self.assertEqual(self.out["reset_valid"], "1 kind 0 count 0")
        self.assertEqual(self.out["tw_valid"], "1")
        self.assertEqual(self.out["flipped_valid"], "0")
        self.assertEqual(self.out["bad_kind_valid"], "0")

    def test_task_watchdog_line_names_who_starved(self) -> None:
        self.assertEqual(self.out["starved"], "IDLE0, muse_voice|")
        self.assertEqual(self.out["tw_line"],
                         "task watchdog on core 0, 312.4 s up, pc 0x4200abcd; running mg_face / IDLE1; "
                         "didn't reset the task watchdog: IDLE0, muse_voice (crash 1 since power-on)")

    def test_interrupt_watchdog_line(self) -> None:
        self.assertEqual(self.out["iw_line"],
                         "interrupt watchdog on core 0, 1.5 s up, pc 0x40380010; running lvgl / - "
                         "(crash 2 since power-on)")

    def test_many_starved_truncated(self) -> None:
        n, nul = self.out["many_len"].split(" nul ")
        self.assertLessEqual(int(n), 63)
        self.assertEqual(nul, "1")


if __name__ == "__main__":
    unittest.main()
