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

"""The boot guard and the gadget watchdog.

Three boots in a row that never get healthy escape: to the ROM's download mode
with a USB host on the cable, else SAFE mode. Two crash resets turn Muse off
(SAFE mode, kept in NVS) until re-enabled or a new image boots. The decision
is pure C (muse_bootguard_core.c), run here through a small driver; the rest
checks that the BLE gadget builds have the guard and that SAFE mode keeps the recovery
tiers (BLE, the USB console, the button) and a way out without a phone.
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import re
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "components" / "muse_restart"

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_bootguard.h"
#include "muse_restart.h"

/* One command a line: "boot <reason> <intentional> <usb_host> <can_download> <kept_off>",
 * "healthy", "stable", "lost" (a running boot went unreachable), "garbage" (what a power
 * loss leaves in RTC memory). Prints "mode why keep_off failed crashes lost" after each boot. */
int main(void)
{
    static muse_bootguard_rec_t rec;
    memset(&rec, 0xA5, sizeof(rec));
    char line[128];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strncmp(line, "boot ", 5)) {
            int reason, intentional, host, dl, off;
            sscanf(line + 5, "%d %d %d %d %d", &reason, &intentional, &host, &dl, &off);
            muse_bootguard_in_t in = { .reason = reason, .intentional = intentional, .usb_host = host,
                                       .can_download = dl, .kept_off = off };
            muse_bootguard_out_t out = muse_bootguard_decide(&rec, &in);
            printf("%d %d %d %u %u %u\n", out.mode, out.why, out.keep_off, (unsigned)rec.failed, (unsigned)rec.crashes,
                   (unsigned)rec.lost);
        } else if (!strncmp(line, "healthy", 7)) {
            muse_bootguard_rec_healthy(&rec);
        } else if (!strncmp(line, "lost", 4)) {
            muse_bootguard_rec_lost(&rec);
        } else if (!strncmp(line, "stable", 6)) {
            muse_bootguard_rec_stable(&rec);
        } else if (!strncmp(line, "garbage", 7)) {
            memset(&rec, 0x5A, sizeof(rec));
        } else if (!strncmp(line, "button", 6)) {
            muse_bootguard_rec_button(&rec);   /* the key hold into SAFE asked for the next boot */
        } else if (!strncmp(line, "entry ", 6)) {
            /* "entry <down> <ms since boot>": the hold into SAFE (muse_bootguard_entry_step); prints "fired <0|1>";
             * "entry reset": a new boot's watch. */
            static muse_bootguard_hold_t e;
            if (!strncmp(line + 6, "reset", 5)) {
                memset(&e, 0, sizeof(e));
                continue;
            }
            if (!strncmp(line + 6, "early", 5)) {
                printf("early %d\n", e.released_early);   /* setup mode: held at power-on, let go early */
                continue;
            }
            int down;
            unsigned ms;
            sscanf(line + 6, "%d %u", &down, &ms);
            printf("fired %d\n", muse_bootguard_entry_step(&e, down, ms));
        } else if (!strncmp(line, "keys ", 5)) {
            /* "keys <down> <ms since boot>": SAFE's key (muse_bootguard_keys_step); prints "fired <0|1|2>". */
            static muse_bootguard_keys_t k;
            int down;
            unsigned ms;
            sscanf(line + 5, "%d %u", &down, &ms);
            printf("fired %d\n", muse_bootguard_keys_step(&k, down, ms));
        } else if (!strncmp(line, "hold ", 5)) {
            /* "hold <down> <ms>": the SAFE key hold (muse_bootguard_hold_step); prints "fired <0|1>". */
            static muse_bootguard_hold_t h;
            int down;
            unsigned ms;
            sscanf(line + 5, "%d %u", &down, &ms);
            printf("fired %d\n", muse_bootguard_hold_step(&h, down, ms));
        }
    }
    return 0;
}
"""

POWERON, SW, PANIC, TASK_WDT, WDT, DEEPSLEEP = 1, 3, 4, 6, 7, 8
NORMAL, SAFE, DOWNLOAD = 0, 1, 2


class BootGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        src = Path(cls.tmp.name) / "driver.c"
        src.write_text(DRIVER)
        cls.exe = Path(cls.tmp.name) / "driver"
        r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(COMP / "include"),
                            str(src), str(COMP / "muse_bootguard_core.c"), str(COMP / "muse_restart_core.c"),
                            "-o", str(cls.exe)], text=True, capture_output=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_boots(self, *cmds: str) -> list[tuple[int, ...]]:
        r = run_child([str(self.exe)], input="".join(c + "\n" for c in cmds), text=True,
                      capture_output=True, check=True)
        return [tuple(int(x) for x in l.split()) for l in r.stdout.splitlines() if not l.startswith("fired")]

    def entries(self, *steps) -> list[int]:
        cmds = "".join("entry reset\n" if s == "reset" else f"entry {s[0]} {s[1]}\n" for s in steps)
        r = run_child([str(self.exe)], input=cmds, text=True, capture_output=True, check=True)
        return [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith("fired")]

    def early(self, *steps) -> int:
        cmds = "".join("entry reset\n" if s == "reset" else f"entry {s[0]} {s[1]}\n" for s in steps) + "entry early\n"
        r = run_child([str(self.exe)], input=cmds, text=True, capture_output=True, check=True)
        return [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith("early")][0]

    def test_setup_mode_is_the_key_held_at_power_on_and_let_go_early(self) -> None:
        # A board whose only key is its talk key takes a re-setup only in setup mode: the key pressed at power-on
        # (within the entry window, after the watch saw it up) and let go before the SAFE hold. A hold to SAFE, a
        # press begun after the window, and a key never pressed are not.
        self.assertEqual(self.early((0, 0), (1, 100), (1, 2000), (0, 2500)), 1)
        self.assertEqual(self.early((0, 0), (1, 100), (1, 9999), (0, 10099)), 1)
        self.assertEqual(self.early((0, 0), (1, 100), (1, 10100), (0, 10200)), 0)   # the SAFE hold
        self.assertEqual(self.early((0, 0), (1, 3001), (0, 3500)), 0)              # begun after the window
        self.assertEqual(self.early((1, 0), (1, 100), (0, 2000)), 0)               # held since before the watch
        self.assertEqual(self.early((0, 0), (0, 5000)), 0)

    def keys(self, *steps: tuple[int, int]) -> list[int]:
        cmds = "".join(f"keys {d} {ms}\n" for d, ms in steps)
        r = run_child([str(self.exe)], input=cmds, text=True, capture_output=True, check=True)
        return [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith("fired")]

    def holds(self, *steps: tuple[int, int]) -> list[int]:
        cmds = "".join(f"hold {d} {ms}\n" for d, ms in steps)
        r = run_child([str(self.exe)], input=cmds, text=True, capture_output=True, check=True)
        return [int(l.split()[1]) for l in r.stdout.splitlines() if l.startswith("fired")]

    @staticmethod
    def boot(reason: int, intentional: int = 0, host: int = 0, dl: int = 1, off: int = 0) -> str:
        return f"boot {reason} {intentional} {host} {dl} {off}"

    def test_power_on_is_a_fresh_start(self) -> None:
        out = self.run_boots(self.boot(POWERON), self.boot(SW), self.boot(SW))
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL])
        self.assertEqual(out[0][3:], (0, 0, 0))
        self.assertEqual(out[2][3], 2)   # two unplanned software restarts before healthy
        out = self.run_boots("garbage", self.boot(SW))   # RTC garbage after a power loss
        self.assertEqual(out[0][3:], (0, 0, 0))

    def test_a_flashers_reset_is_a_fresh_start(self) -> None:
        # board.sh boots a freshly flashed S3 with esptool's watchdog reset, which the chip reports
        # as a power-on (raw 0x01, measured on three S3 boards): a crash before it is forgotten,
        # so flashing the same build twice never adds up to two crashes.
        out = self.run_boots(self.boot(POWERON), "healthy", self.boot(PANIC), self.boot(POWERON), "healthy",
                             self.boot(PANIC), self.boot(POWERON))
        self.assertEqual([o[0] for o in out], [NORMAL] * 5)
        self.assertEqual(out[3][3:], (0, 1, 0))   # the second panic counts once: the reset forgot the first
        self.assertEqual(out[4][3:], (0, 0, 0))
        # Without the reset in between, two crashes do turn Muse off: real crash loops still count.
        out = self.run_boots(self.boot(POWERON), "healthy", self.boot(PANIC), "healthy", self.boot(PANIC))
        self.assertEqual(out[-1][0], SAFE)

    def test_three_failed_boots_escape(self) -> None:
        # Unhealthy boots (unplanned restarts out of a failure): the fourth boot escapes.
        boots = [self.boot(POWERON)] + [self.boot(SW)] * 3
        out = self.run_boots(*boots)
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL, SAFE])   # no USB host
        out = self.run_boots(*[self.boot(POWERON)] + [self.boot(SW, host=1)] * 3)
        self.assertEqual(out[-1][:2], (DOWNLOAD, 1))
        out = self.run_boots(*[self.boot(POWERON)] + [self.boot(SW, host=1, dl=0)] * 3)
        self.assertEqual(out[-1][0], SAFE)   # a chip without the download flag
        # The escape starts the count over: the next boots get three tries again.
        out = self.run_boots(*[self.boot(POWERON)] + [self.boot(SW)] * 4)
        self.assertEqual(out[-1][0], NORMAL)
        self.assertEqual(out[-1][3], 0)

    def test_healthy_and_planned_restarts_dont_count(self) -> None:
        out = self.run_boots(self.boot(POWERON), self.boot(SW), "healthy", self.boot(SW), self.boot(SW),
                             "healthy", self.boot(SW))
        self.assertEqual([o[0] for o in out], [NORMAL] * 5)
        out = self.run_boots(self.boot(POWERON), *[self.boot(SW, intentional=1)] * 5)
        self.assertEqual([o[3] for o in out], [0] * 6)   # muse_restart(): planned, never a failure
        # Turned off (deep sleep) and on again before healthy, again and again: never a failure
        out = self.run_boots(self.boot(POWERON), *[self.boot(DEEPSLEEP)] * 5)
        self.assertEqual([o[0] for o in out], [NORMAL] * 6)
        self.assertEqual([o[3] for o in out], [0] * 6)

    def test_two_crashes_turn_muse_off(self) -> None:
        out = self.run_boots(self.boot(POWERON), "healthy", self.boot(PANIC), "healthy", self.boot(TASK_WDT))
        self.assertEqual(out[1][:3], (NORMAL, 0, 0))
        self.assertEqual(out[2][:3], (SAFE, 2, 1))   # off, and kept off (NVS) by the caller
        # Kept off on later boots until re-enabled (the NVS flag the caller reads).
        out = self.run_boots(self.boot(POWERON, off=1))
        self.assertEqual(out[0][:2], (SAFE, 3))
        # Ten minutes up clears the crash count: a crash a day isn't a loop.
        out = self.run_boots(self.boot(POWERON), "stable", self.boot(PANIC), "stable", self.boot(WDT))
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL])

    def test_running_losses_escalate_across_healthy_boots(self) -> None:
        # Each boot gets healthy, then goes unreachable and restarts unmarked;
        # the failed-boot count never sees it, the lost count does: SAFE on the boot after the third.
        cycle = ["healthy", "lost", self.boot(SW)]
        out = self.run_boots(self.boot(POWERON), *cycle * 3)
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL, SAFE])
        self.assertEqual(out[-1][1], 4)          # MUSE_BOOT_WHY_LOST
        self.assertEqual([o[3] for o in out], [0, 0, 0, 0])   # never a failed boot
        # SAFE stays while the count stands (healthy alone doesn't clear it) ...
        out = self.run_boots(self.boot(POWERON), *cycle * 3, "healthy", self.boot(SW))
        self.assertEqual(out[-1][0], SAFE)
        # ... until a boot stays up 10 minutes; occasional losses with stable boots between don't add up.
        out = self.run_boots(self.boot(POWERON), *cycle * 3, "stable", self.boot(SW))
        self.assertEqual(out[-1][:2], (NORMAL, 0))
        out = self.run_boots(self.boot(POWERON), *(["healthy", "lost", self.boot(SW), "stable"] * 5), self.boot(SW))
        self.assertTrue(all(o[0] == NORMAL for o in out))
        # A power-on starts over.
        out = self.run_boots(self.boot(POWERON), *cycle * 2, "lost", self.boot(POWERON))
        self.assertEqual(out[-1][:2], (NORMAL, 0))

    def test_crash_kinds_count_alike(self) -> None:
        # Any two of panic, task, interrupt or other watchdog resets
        # within the stable time end in SAFE; two with ten minutes up between them don't.
        INT_WDT = 5
        kinds = (PANIC, TASK_WDT, INT_WDT, WDT)
        for a in kinds:
            for b in kinds:
                out = self.run_boots(self.boot(POWERON), "healthy", self.boot(a), "healthy", self.boot(b))
                self.assertEqual(out[2][:3], (SAFE, 2, 1), (a, b))
                out = self.run_boots(self.boot(POWERON), "stable", self.boot(a), "stable", self.boot(b))   # 11 min apart
                self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL], (a, b))
        # A software restart, a brownout or a plain reset between them isn't a crash.
        out = self.run_boots(self.boot(POWERON), "healthy", self.boot(PANIC), "healthy", self.boot(SW, intentional=1),
                             "healthy", self.boot(SW))
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL, NORMAL])
        self.assertEqual(out[-1][4], 1)   # the one crash, still counted

    def test_a_clean_boot_clears_the_count(self) -> None:
        # A power-on, or a boot that stays up the stable time, starts the count over: the next
        # crash alone isn't SAFE. (SAFE itself, once reached, stays until safe.off / >safe=0, a
        # new image or the key hold, kept in NVS by the caller.)
        out = self.run_boots(self.boot(POWERON), self.boot(PANIC), self.boot(POWERON), self.boot(TASK_WDT))
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL, NORMAL])
        out = self.run_boots(self.boot(POWERON), self.boot(PANIC), "stable", self.boot(TASK_WDT))
        self.assertEqual([o[0] for o in out], [NORMAL, NORMAL, NORMAL])

    def test_the_key_hold_leaves_safe(self) -> None:
        # 5 s of the key held (after it was seen up) fires once; 4.9 s doesn't; a key held since
        # before the watch (through the boot) doesn't count until it's let go; a release rearms it.
        self.assertEqual(self.holds((0, 0), (1, 100), (1, 5099), (1, 5100), (1, 9000)), [0, 0, 0, 1, 0])
        self.assertEqual(self.holds((0, 0), (1, 100), (1, 5000), (0, 5001)), [0, 0, 0, 0])   # 4.9 s
        self.assertEqual(self.holds((1, 0), (1, 6000), (1, 20000)), [0, 0, 0])               # held from boot
        self.assertEqual(self.holds((1, 0), (0, 10), (1, 20), (1, 5020)), [0, 0, 0, 1])
        self.assertEqual(self.holds((0, 0), (1, 0), (1, 5000), (0, 5050), (1, 6000), (1, 11000)), [0, 0, 1, 0, 0, 1])
        self.assertEqual(self.holds((0, 0xFFFFF000), (1, 0xFFFFF000), (1, 0x00000388)), [0, 0, 1])   # the clock wraps
        h = (COMP / "include" / "muse_bootguard.h").read_text()
        self.assertIn("#define MUSE_BOOTGUARD_SAFE_HOLD_MS 5000", h)

    def test_the_key_hold_enters_safe(self) -> None:
        # The user's way into SAFE: the SAFE key seen up, pressed within 3 s of boot, held 10 s.
        self.assertEqual(self.entries((0, 100), (1, 2900), (1, 12899), (1, 12900), (1, 13000)), [0, 0, 0, 1, 0])
        # Released before 10 s (an ordinary long talk turn right after boot): never SAFE.
        self.assertEqual(self.entries((0, 100), (1, 1000), (1, 10999), (0, 11000), (1, 11500), (1, 22000)),
                         [0, 0, 0, 0, 0, 0])
        # A press begun after 3 s doesn't count, nor any later one this boot.
        self.assertEqual(self.entries((0, 100), (1, 3001), (1, 14000), (0, 14100), (1, 14200), (1, 30000)),
                         [0, 0, 0, 0, 0, 0])
        # Held (or stuck) since before boot: never, though it stays down; only a release and a fresh press count.
        self.assertEqual(self.entries((1, 0), (1, 10000), (1, 20000)), [0, 0, 0])
        self.assertEqual(self.entries((1, 0), (0, 500), (1, 600), (1, 10600)), [0, 0, 0, 1])
        h = (COMP / "include" / "muse_bootguard.h").read_text()
        self.assertIn("#define MUSE_BOOTGUARD_ENTRY_WINDOW_MS 3000", h)
        self.assertIn("#define MUSE_BOOTGUARD_ENTRY_HOLD_MS 10000", h)

    def test_in_a_self_entered_safe_the_key_does_both(self) -> None:
        # A press begun in the window asks for the button SAFE at 10 s, and at 5 s it does NOT leave SAFE;
        # a press begun later leaves SAFE at 5 s (1 = enter, 2 = leave).
        self.assertEqual(self.keys((0, 100), (1, 2000), (1, 7100), (1, 11999), (1, 12000)), [0, 0, 0, 0, 1])
        self.assertEqual(self.keys((0, 100), (1, 4000), (1, 8999), (1, 9000)), [0, 0, 0, 2])

    def test_a_button_request_is_one_safe_boot(self) -> None:
        # The hold records the request, then a planned restart: that one boot is SAFE, cause button; the next
        # boot is the guard's own to decide again (normal here). A self-entered SAFE keeps its own cause.
        b = self.run_boots(self.boot(POWERON), "healthy", "button", self.boot(SW, intentional=1), "healthy",
                           self.boot(SW, intentional=1))
        self.assertEqual(b[1][:2], (SAFE, 5))   # MUSE_BOOT_WHY_BUTTON
        self.assertEqual(b[2][:2], (NORMAL, 0))
        c = self.run_boots(self.boot(POWERON), self.boot(PANIC), "healthy", self.boot(PANIC))
        self.assertEqual(c[-1][:2], (SAFE, 2))
        # A stuck key through two restarts: the first boot's watch never fires (no release), so the second boot
        # comes up normal, not SAFE.
        self.assertEqual(self.entries((1, 0), (1, 12000), "reset", (1, 0), (1, 12000)), [0, 0, 0, 0])
        d = self.run_boots(self.boot(POWERON), "healthy", self.boot(SW, intentional=1), "healthy",
                           self.boot(SW, intentional=1))
        self.assertEqual([x[0] for x in d], [NORMAL, NORMAL, NORMAL])
        src = (COMP / "muse_bootguard_core.c").read_text()
        self.assertIn('case MUSE_BOOT_WHY_BUTTON: return "the user held the SAFE key after boot";', src)

    def test_the_way_into_safe_is_wired(self) -> None:
        core = (COMP / "muse_bootguard.c").read_text()
        enter = core[core.index("bool muse_bootguard_enter_safe(const char *why)"):]
        enter = enter[:enter.index("\n}\n")]
        self.assertLess(enter.index("muse_bootguard_rec_button(&s_rec);"), enter.index("muse_restart(MUSE_RESTART_SAFE_ON);"))
        self.assertIn("s_why = out.mode == MUSE_BOOT_NORMAL ? MUSE_BOOT_WHY_NONE : out.why;", core)
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        start = glue[glue.index("void muse_glue_start(void) {"):]
        self.assertLess(start.index("muse_bootguard_entry_watch(board->safe_key);"), start.index("muse_input_power_init();"))
        main = (ROOT / "main" / "main.c").read_text()
        self.assertIn("button_safe_entry_watch();", main)
        btn = (ROOT / "main" / "button.c").read_text()
        self.assertIn("boot_press = muse_bootguard_entry_open();", btn)
        self.assertIn("if (held_ms >= LONG_PRESS_MS && !boot_press) {", btn)
        self.assertIn("muse_bootguard_entry_watch(button_raw_down);", btn)


class WiringTest(unittest.TestCase):


    def test_guard_on_only_in_ble_gadget_builds(self) -> None:
        # The crash count and its SAFE mode by default only in the BLE gadget builds (BLE audio or a
        # standalone profile), whose overlays set it too; other builds keep it off. Where it's on, only
        # builds with musegadgets BLE need BLE reachable to count healthy; elsewhere 30 s up is enough
        # and nothing restarts the gadget for being out of reach.
        kconfig = (COMP / "Kconfig").read_text()
        guard = kconfig[kconfig.index("config MUSE_BOOTGUARD\n"):kconfig.index("config MUSE_BOOTGUARD_BLE_REACHABLE")]
        self.assertIn("default y if MUSE_GADGET_BLE_STANDALONE || MUSE_GADGET_BLE_AUDIO\n        default n\n", guard)
        self.assertNotIn("MOSAICO", guard.split("help")[0])
        for o in (ROOT / "devices").glob("sdkconfig.*"):
            text = o.read_text()
            if "CONFIG_MUSE_GADGET_BLE_AUDIO=y" in text or "CONFIG_MUSE_GADGET_BLE_STANDALONE=y" in text:
                self.assertIn("CONFIG_MUSE_BOOTGUARD=y\n", text, o.name)
        reach = kconfig[kconfig.index("config MUSE_BOOTGUARD_BLE_REACHABLE"):kconfig.index("config MUSE_BOOTGUARD_SAFE_LINK_QUIET")]
        self.assertIn("default y if MUSE_GADGET_BLE_STANDALONE || MUSE_GADGET_BLE_AUDIO", reach)
        quiet = kconfig[kconfig.index("config MUSE_BOOTGUARD_SAFE_LINK_QUIET"):kconfig.index("config MUSE_GADGET_WDT")]
        self.assertIn("depends on MUSE_BOOTGUARD && !MUSE_GADGET_BLE_AUDIO", quiet)   # Wi-Fi Muse and Link only
        # No overlay turns the guard off.
        for o in (ROOT / "devices").glob("sdkconfig.*"):
            self.assertNotIn("CONFIG_MUSE_BOOTGUARD=n", o.read_text(), o.name)
            self.assertNotIn("# CONFIG_MUSE_BOOTGUARD is not set", o.read_text(), o.name)
        # Without BLE reachable: armed with no reachable checks and no stay-reachable timer.
        app = (ROOT / "main" / "app.c").read_text()
        run = app[app.index("\nvoid app_run(void) {"):]
        b = run[run.index("#if CONFIG_MUSE_BOOTGUARD && !CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE"):]
        b = b[:b.index("#elif")]
        self.assertIn("muse_bootguard_arm();", b)
        self.assertNotIn("set_reachable", b)
        self.assertNotIn("guard_tick", b)
        self.assertIn("#if CONFIG_MUSE_BOOTGUARD && CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE && !CONFIG_MUSE_GADGET_BLE_STANDALONE\nstatic void guard_tick", app)


    def test_download_mode_only_with_a_usb_host(self) -> None:
        # Download mode starts only with a USB host attached, never on a bare charger: the boot guard's escape is
        # firmware's only way into ROM download (the board's BOOT strap, and a power chip's own long press, stay
        # as they are). It asks for a host by its start-of-frame packets (a host sends one every millisecond once
        # it has the bus; a charger, VBUS alone, none) and sets the flag only for MUSE_BOOT_DOWNLOAD, which the
        # decision gives only with usb_host (BootGuardTest: no host, SAFE).
        src = (ROOT / "components" / "muse_restart" / "muse_bootguard.c").read_text()
        host = src[src.index("static bool usb_host_present(void)\n{"):]
        host = host[:host.index("\n}\n")]
        self.assertIn("usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);", host)
        self.assertIn("return (usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SOF) != 0;", host)
        self.assertIn("#else\n    return false;", host)   # no USB Serial/JTAG: never a host
        for vbus in ("vbus", "VBUS", "usb_connected", "muse_state_power"):
            self.assertNotIn(vbus, host)
        begin = src[src.index("muse_boot_mode_t muse_bootguard_begin(void)\n{"):]
        begin = begin[:begin.index("\n}\n")]
        self.assertEqual(begin.count("in.usb_host = "), 1)
        self.assertIn("in.usb_host = usb_host_present();", begin)
        self.assertEqual(src.count("DOWNLOAD_BOOT();"), 1)
        self.assertIn("if (out.mode == MUSE_BOOT_DOWNLOAD) {", begin[:begin.index("DOWNLOAD_BOOT();")])
        core = (ROOT / "components" / "muse_restart" / "muse_bootguard_core.c").read_text()
        self.assertIn("out.mode = in->usb_host && in->can_download ? MUSE_BOOT_DOWNLOAD : MUSE_BOOT_SAFE;", core)
        # No other firmware path writes the ROM's download flag.
        flags = ("FORCE_DOWNLOAD", "RTC_CNTL_OPTION1_REG", "chip_usb_set_persist_flags")
        for d in ("main", "components"):
            for f in (ROOT / d).rglob("*"):
                if f.suffix in (".c", ".cpp", ".h") and f.name != "muse_bootguard.c":
                    text = f.read_text(errors="ignore")
                    for flag in flags:
                        self.assertNotIn(flag, text, f)

    def test_safe_way_out_without_a_phone(self) -> None:
        # No UI in SAFE; the USB console's >safe=0 and a 5 s key hold.
        glue = (ROOT / "main" / "muse_glue.c").read_text()
        boot = glue[glue.index("static void boot_task("):]
        safe = boot[boot.index("if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {"):boot.index("MUSE_TASK_END();")]
        self.assertIn("muse_input_safe_console_start()", safe)
        self.assertIn("muse_bootguard_safe_watch(b->safe_key, NULL);", safe)
        for absent in ("display_start", "lv_", "muse_ui_", "muse_input_start("):
            self.assertNotIn(absent, safe)
        inp = (ROOT / "components" / "muse" / "muse_input.c").read_text()
        sc = inp[inp.index("static bool safe_console_line(const char *line)"):]
        sc = sc[:sc.index("\n}\n")]
        self.assertIn("muse_bootguard_console_word(line, out, sizeof(out))", sc)
        line = inp[inp.index("bool muse_input_console_line(char *line)"):]
        self.assertLess(line.index("MUSE_BOOT_SAFE"), line.index("console_command(line, true)"))
        cmd = inp[inp.index("static bool console_command(char *line, bool whole)"):]
        self.assertIn("muse_bootguard_console_word(line, safe, sizeof(safe))", cmd[:cmd.index("s_console_hook(line)")])
        tok = (ROOT / "main" / "sdk_token_console.c").read_text()
        self.assertIn("muse_bootguard_console_word(line, safe, sizeof(safe))", tok)
        core = (COMP / "muse_bootguard.c").read_text()
        word = core[core.index("bool muse_bootguard_console_word_from("):]
        word = word[:word.index("\n}\n")]
        self.assertIn('!strcmp(line, "safe=0") || !strcmp(line, "safe.off")', word)
        self.assertIn("muse_bootguard_reenable_from(trusted)", word)
        # The hold: SAFE only, then Muse on and a planned restart; outside SAFE nothing changes.
        leave = core[core.index("bool muse_bootguard_leave_safe(const char *why)"):]
        leave = leave[:leave.index("\n}\n")]
        self.assertLess(leave.index("if (s_mode != MUSE_BOOT_SAFE) {\n        return false;"), leave.index("muse_bootguard_reenable();"))
        self.assertIn("muse_restart(MUSE_RESTART_SAFE_OFF);", leave)
        watch = core[core.index("void muse_bootguard_safe_watch("):]
        self.assertIn("if (s_mode != MUSE_BOOT_SAFE || !key_down) {\n        return;", watch[:watch.index("\n}\n")])
        # Link's button: the same 5 s hold leaves SAFE there; outside SAFE it still resets setup.
        app = (ROOT / "main" / "app.c").read_text()
        lp = app[app.index("static void on_button_long_press(void) {"):]
        lp = lp[:lp.index("\n}\n")]
        self.assertLess(lp.index('if (muse_bootguard_leave_safe("button held")) return;'), lp.index("reset_setup_from_control("))
        self.assertIn("#define LONG_PRESS_MS      5000", (ROOT / "main" / "button.c").read_text())
        # Every board the gate builds has a key read without init.
        boards = ROOT / "components" / "muse" / "boards"
        for b in ("board_waveshare_s3_175c.c", "board_waveshare_c6_18.c", "board_esp_mosaico.c",
                  "board_m5stack_stopwatch.c", "board_sensecap_watcher.c"):
            src = (boards / b).read_text()
            self.assertIn(".safe_key = safe_key,", src, b)
            k = src[src.index("static bool safe_key(void)"):]
            k = k[:k.index("\n}\n")]
            for absent in ("muse_gpio_button_poll", "lv_", "s_tp"):
                self.assertNotIn(absent, k, b)
        rs = (COMP / "muse_restart_core.c").read_text()
        self.assertIn('"SAFE off"', rs)
        self.assertIn('"SAFE on"', rs)

    def test_boards_with_a_safe_key_read_it_raw(self) -> None:
        # The way into SAFE (and out of it) needs a key read without init. The BLE gadget boards have one; any board
        # that declares one reads it raw (no button poll, LVGL or touch); a board without one (or one listed here,
        # whose key a gadget can't read raw from the boot's start) recovers over USB.
        no_key = {
            "board_m5stack_core2.c": "touch buttons and PWR, which only the AXP192 sees",
            "board_m5stack_cores3.c": "PWR only, which only the AXP2101 sees",
            "board_waveshare_s3_lcd7.c": "BOOT (GPIO0) is on the RGB panel bus",
            "board_waveshare_s3_lcd128.c": "touch only",
            "board_ostb_echoear_2st.c": "touch only (a power switch, no key)",
        }
        gadgets = ("board_waveshare_s3_175c.c", "board_waveshare_c6_18.c", "board_esp_mosaico.c",
                   "board_m5stack_stopwatch.c", "board_sensecap_watcher.c")
        boards = ROOT / "components" / "muse" / "boards"
        for f in sorted(boards.glob("board_*.c")):
            src = f.read_text()
            if f.name in no_key:
                self.assertNotIn(".safe_key", src, f.name)
                continue
            if f.name in gadgets:
                self.assertIn(".safe_key = safe_key,", src, f.name)
            if ".safe_key" not in src:
                continue
            self.assertIn(".safe_key = safe_key,", src, f.name)
            k = src[src.index("static bool safe_key(void)"):]
            k = k[:k.index("\n}\n")]
            for absent in ("muse_gpio_button_poll", "lv_", "s_tp"):
                self.assertNotIn(absent, k, f.name)

    def test_safe_keeps_the_recovery_tiers(self) -> None:
        # Builds with musegadgets: BLE (setup, musegadgets, SMP) as before, under the SAFE identity.
        # Without (Wi-Fi Muse, Link only): Link stays (Wi-Fi, Noise with
        # device.health), Link setup BLE as it is (no SAFE identity hiding its service), the USB
        # console and the button.
        app = (ROOT / "main" / "app.c").read_text()
        start = app[app.index("static void start_ble_setup_server_if_needed(void) {"):]
        start = start[:start.index("\n}\n")]
        ident = start[start.index("#if CONFIG_MUSE_GADGET_BLE_AUDIO\n    bool safe_identity"):]
        self.assertIn("bool safe_identity = false;", ident[:ident.index("#endif")])
        run = app[app.index("\nvoid app_run(void) {"):]
        safe_bt = run.index("run_safe_bluetooth_only();")
        self.assertIn("#if CONFIG_MUSE_BOOTGUARD && CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_GADGET_BLE_STANDALONE",
                      run[safe_bt - 300:safe_bt])   # the only SAFE that skips Wi-Fi: builds with musegadgets
        noise = (ROOT / "main" / "noise_control.cpp").read_text()
        reg = noise[noise.index("static char *build_register_json(void) {"):]
        reg = reg[:reg.index('cJSON_AddItemToObject(params, "commands_v2", commands);')]
        q = reg[reg.index("#if CONFIG_MUSE_BOOTGUARD_SAFE_LINK_QUIET"):]
        self.assertIn("if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {", q)
        self.assertIn('strcmp(c->string, "device.health") != 0', q)
        self.assertNotIn("device.ota", noise)
        # device.health's payload is built by noise_ctrl_device_health_json() (bug.report embeds it too).
        health = noise[noise.index('extern "C" cJSON *noise_ctrl_device_health_json('):]
        self.assertIn('cJSON_AddBoolToObject(health, "safe", true);', health[:health.index("\n}\n")])
        send = noise[noise.index("static void send_device_health("):]
        self.assertIn("noise_ctrl_device_health_json(false)", send[:send.index("\n}\n")])
        disp = noise[noise.index('if (strcmp(command, "device.health") == 0) {'):]
        self.assertLess(disp.index('strcmp(command, "device.health") == 0'), disp.index("s_command_cb(command"))
        ws = app[app.index("static cJSON *on_ws_command("):]
        ws = ws[:ws.index("#if CONFIG_HOMEHUB_BLE_COMMANDS")]
        self.assertIn("if (safe_mode_refuses(command)) {", ws)
        self.assertIn('return command_error("safe_mode",', ws)
        refuse = app[app.index("static bool safe_mode_refuses(const char *command) {"):]
        refuse = refuse[:refuse.index("\n}\n")]
        self.assertIn('{ "device.list_vms", "device.set_vm", "device.reset_vm", "device.unpair" }', refuse)
        self.assertIn("if (muse_bootguard_mode() != MUSE_BOOT_SAFE) return false;", refuse)
        for f in ("main/led_status.c", "main/epaper_status.c"):
            src = (ROOT / f).read_text()
            st = src[src.index("void led_status_set_state(led_state_t state) {"):]
            self.assertIn("if (muse_bootguard_mode() == MUSE_BOOT_SAFE) state = LED_STATE_ERROR;", st[:st.index("\n}\n")], f)

    def test_safe_keeps_ble_recovery_and_the_log(self) -> None:
        # In SAFE, SMP (image upload / confirm / reset), the Nordic
        # UART's rescue (safe.off, guard?, the open bench words) and the log mirror all stay.
        ble = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        init = ble[ble.index("void mg_ble_init(const mg_ble_platform_t *platform)"):]
        init = init[:init.index("\n}\n")]
        self.assertIn("    smp_start();", init)
        self.assertIn("s_prev_vprintf = esp_log_set_vprintf(nus_vprintf);", init)
        self.assertNotIn("safe_mode", init)   # nothing in the init depends on SAFE
        table = ble[ble.index("static const struct ble_gatt_svc_def"):]
        self.assertIn("SMP_SVC_DEF,", table)
        nus = ble[ble.index("static void nus_command(char *line)\n{"):]
        nus = nus[:nus.index("\n}\n")]
        # SAFE asks nothing of its own there, beyond the release-build check (safe.off / safe=0 open in SAFE).
        self.assertEqual(nus.count("safe_mode"), 1)
        self.assertIn("mg_unlock_remote_ok(line, proven, s_platform.safe_mode)", nus)
        self.assertNotIn("muse_bootguard_mode", nus)
        self.assertIn("s_platform.console(line)", nus)
        mg = (ROOT / "main" / "mg_glue.c").read_text()
        safe = mg[mg.index("static void start_safe(void) {"):]
        safe = safe[:safe.index("\n}\n")]
        self.assertIn(".console = safe_console,", safe)
        self.assertIn("mg_ble_init(&platform);", safe)
        sc = mg[mg.index("static bool safe_console(char *line) {"):]
        sc = sc[:sc.index("\n}\n")]
        self.assertIn("muse_bootguard_console_word_from(line, out, sizeof(out), false)", sc)   # the Nordic UART's
        self.assertIn('"ota?"', sc)
        word = (COMP / "muse_bootguard.c").read_text()
        word = word[word.index("bool muse_bootguard_console_word_from("):]
        self.assertIn('!strcmp(line, "guard?")', word[:word.index("\n}\n")])

    def test_an_unproven_safe_off_leaves_safe_but_keeps_the_loss_count(self) -> None:
        # The Nordic UART's safe.off / safe=0 in SAFE (no token proof runs there) still leaves SAFE, the keep-off
        # record erased, but the running-loss count stays; the key hold (muse_bootguard_leave_safe), a proven word
        # and the USB console clear it. muse_bootguard_reenable_from() compiled from muse_bootguard.c over the core.
        bg = (COMP / "muse_bootguard.c").read_text()
        i = bg.index("bool muse_bootguard_reenable_from(bool trusted)")
        fn = bg[i:bg.index("\n}\n", i) + 3]
        src = ("#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n#include \"muse_bootguard.h\"\n"
               "#define CONFIG_MUSE_BOOTGUARD 1\n#define TAG \"t\"\n#define ESP_LOGW(t, ...) ((void)0)\n"
               "#define ESP_LOGE(t, ...) ((void)0)\ntypedef int esp_err_t; typedef int nvs_handle_t;\n"
               "#define ESP_OK 0\n#define ESP_ERR_NVS_NOT_FOUND 0x1102\n#define NVS_READWRITE 1\n"
               "#define NVS_NS \"g\"\n#define NVS_OFF_KEY \"off\"\nstatic bool kept_off_flag;\n"
               "static esp_err_t nvs_open(const char *a, int m, nvs_handle_t *h) { (void)a; (void)m; *h = 1; return 0; }\n"
               "static esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; (void)k;\n"
               "  esp_err_t r = kept_off_flag ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; kept_off_flag = false; return r; }\n"
               "static esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return 0; }\n"
               "static void nvs_close(nvs_handle_t h) { (void)h; }\n"
               "static muse_bootguard_rec_t s_rec;\n" + fn +
               "static void lose(void) { memset(&s_rec, 0, sizeof(s_rec)); muse_bootguard_rec_lost(&s_rec);\n"
               "  muse_bootguard_rec_lost(&s_rec); kept_off_flag = true; }\n"
               "int main(void) {\n"
               "  lose(); bool a = muse_bootguard_reenable_from(false);\n"
               "  printf(\"%d %d %u\\n\", a, kept_off_flag, (unsigned)s_rec.lost);\n"
               "  lose(); bool b = muse_bootguard_reenable_from(true);\n"
               "  printf(\"%d %d %u\\n\", b, kept_off_flag, (unsigned)s_rec.lost);\n"
               "  return 0;\n}\n")
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "x.c").write_text(src)
            exe = Path(d) / "x"
            cc = shlex.split(os.environ.get("CC", "cc"))
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(COMP / "include"),
                                str(Path(d) / "x.c"), str(COMP / "muse_bootguard_core.c"),
                                str(COMP / "muse_restart_core.c"), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.splitlines()
        self.assertEqual(out[0].split(), ["1", "0", "2"])   # unproven: SAFE left (record gone), the count kept
        self.assertEqual(out[1].split(), ["1", "0", "0"])   # trusted: cleared, as before
        # Who's unproven: only the Nordic UART's SAFE console; the USB console and the key hold stay trusted.
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        sc = glue[glue.index("static bool safe_console(char *line) {"):]
        sc = sc[:sc.index("\n}\n")]
        self.assertNotIn("muse_bootguard_console_word(line", sc)
        lv = bg[bg.index("bool muse_bootguard_leave_safe(const char *why)"):]
        self.assertIn("muse_bootguard_reenable();", lv[:lv.index("\n}\n")])
        # The NUS gate lets exactly these two through unproven, and only in SAFE (mg_unlock.c).
        unlock = (ROOT / "components" / "muse_gadget_ble" / "mg_unlock.c").read_text()
        self.assertIn('return safe_mode && (!strcmp(line, "safe.off") || !strcmp(line, "safe=0"));', unlock)

    def test_safe_declares_its_cause(self) -> None:
        # mgcommands.h Recovery: the key's SAFE is reason user (4); one the guard entered itself reports its
        # cause: boot_loop (1) after failed boots, crash_guard (2) after crash resets or running boots that kept
        # going unreachable, muse_guard (3) while Muse stays off from them. Compiled from mg_glue.c itself.
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        i = glue.index("static uint8_t safe_reason(muse_boot_why_t why) {")
        fn = glue[i:glue.index("\n}\n", i) + 3]
        safe = glue[glue.index("static void start_safe(void) {"):]
        safe = safe[:safe.index("\n}\n")]
        self.assertLess(safe.index("platform.recovery_reason = safe_reason(muse_bootguard_safe_why());"),
                        safe.index("mg_ble_init(&platform);"))
        src = ('#include <stdint.h>\n#include <stdio.h>\n#include "mgcommands.h"\n#include "muse_bootguard.h"\n' + fn +
               "int main(void) {\n"
               "  printf(\"%d %d %d %d %d %d\\n\", safe_reason(MUSE_BOOT_WHY_BUTTON), safe_reason(MUSE_BOOT_WHY_FAILED_BOOTS),\n"
               "         safe_reason(MUSE_BOOT_WHY_LOST), safe_reason(MUSE_BOOT_WHY_CRASHES),\n"
               "         safe_reason(MUSE_BOOT_WHY_KEPT_OFF), safe_reason(MUSE_BOOT_WHY_NONE));\n"
               "  return 0;\n}\n")
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as t:
            (Path(t) / "r.c").write_text(src)
            exe = Path(t) / "r"
            r = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT.parent / "protocols"),
                                "-I", str(COMP / "include"), str(Path(t) / "r.c"), "-o", str(exe)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            out = subprocess.run([str(exe)], capture_output=True, text=True).stdout.split()
        self.assertEqual(out, ["4", "1", "2", "2", "3", "0"])


    def test_hang_words(self) -> None:
        # The watchdogs' hardware check (the C6): a task and an interrupt watchdog reset on purpose,
        # USB console only, in every build.
        rs = (COMP / "muse_restart.c").read_text()
        hang = rs[rs.index("static void hang_task(void *arg)"):]
        hang = hang[:hang.index("bool muse_restart_test_failure(")]
        self.assertIn("portDISABLE_INTERRUPTS();", hang)
        self.assertIn("configMAX_PRIORITIES - 1, NULL, 0)", hang)   # top priority, core 0: its idle task starves
        self.assertNotIn("vTaskDelay(pdMS_TO_TICKS(50));\n    for (;;) {\n        vTaskDelay", hang)
        self.assertIn('!strcmp(line, "hang=task")', hang)
        self.assertIn('!strcmp(line, "hang=int")', hang)
        tok = (ROOT / "main" / "sdk_token_console.c").read_text()
        w = tok[tok.index('if (!strncmp(line, "hang", 4)'):]
        w = w[:w.index("return true;")]
        self.assertIn("        char out[80];\n        muse_restart_hang_word(line, out, sizeof(out));", w)
        self.assertNotIn("automation off", w)   # the USB console is open in every build
        # Never from the Nordic UART: its console and bench input don't reach Link's console.
        mg = (ROOT / "main" / "mg_glue.c").read_text()
        for fn in ("static bool ble_console(", "static int ble_bench_input("):
            body = mg[mg.index(fn):]
            body = body[:body.index("\n}\n")]
            self.assertNotIn('"hang', body, fn)
            self.assertNotIn("sdk_token_console_command", body, fn)
        self.assertNotIn('"hang', (ROOT / "components" / "muse_gadget_ble" / "mg_unlock.c").read_text())
        # The Muse boards' USB console reaches it through Link's (glue_link_console), the
        # Link-only boards' directly.


if __name__ == "__main__":
    unittest.main()
