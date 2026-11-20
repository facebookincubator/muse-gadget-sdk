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

"""The AXP2101 power key: muse_pmu.c against a fake register file, and the
boards that talk on PWR, whose talk can outlast the PMU's 10 s hold-off."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
TESTS = ROOT / "tests"


def cc():
    return shlex.split(os.environ.get("CC", "cc"))


def functions(source, name):
    """Every definition of a static function, in order (a board can have one
    per #if branch)."""
    found = []
    for match in re.finditer(r"^static (?:unsigned|int|void) " + name + r"\([^)]*\)\n\{", source, re.M):
        depth, end = 1, source.index("{", match.start()) + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        found.append(source[match.start():end])
    return found


def defines(source, pattern):
    return "\n".join(re.findall(r"^#define (?:" + pattern + r")\b.*", source, re.M))


class MusePmu(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_pmu"
        built = subprocess.run([*cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(TESTS / "pmu_fakes"),
                                "-I", str(MUSE), str(TESTS / "muse_pmu_harness.c"), "-o", str(cls.binary)],
                               capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_init_sets_10s_hold_off_and_leaves_key_shutdowns(self):
        self.run_case(0)

    def test_key_shutdown_changes_only_its_bit(self):
        self.run_case(1)

    def test_i2c_errors_are_returned(self):
        self.run_case(2)

    def test_no_pmu_is_invalid_state_and_quiet(self):
        self.run_case(3)

    def test_16s_shutdown_is_off_only_while_held(self):
        self.run_case(4)

    def test_16s_shutdown_errors(self):
        self.run_case(5)


class PowerKeyTalkBoardsInit(unittest.TestCase):
    """The boards that talk on PWR turn its shutdowns on at boot (a reset in a
    held talk may have left them off); the others leave the PMU's alone."""

    def init_of(self, name):
        source = (MUSE / "boards" / name).read_text()
        start = source.index("static esp_err_t init(void)\n{")
        depth, end = 1, source.index("{", start) + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]

    def test_pwr_talk_boards_turn_key_shutdown_on(self):
        for name in ("board_m5stack_cores3.c", "board_waveshare_s3_216.c"):
            init = self.init_of(name)
            self.assertIn("muse_pmu_set_key_shutdown(true)", init, name)
            self.assertLess(init.index("muse_pmu_init("), init.index("muse_pmu_set_key_shutdown(true)"), name)
        init = self.init_of("board_waveshare_s3_175c.c")
        # The 1.75 shares the file and talks on BOOT: only the 1.75C's branch.
        call = init.index("muse_pmu_set_key_shutdown(true)")
        self.assertIn("#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175", init[:call])
        self.assertNotIn("#endif", init[init.rindex("#if !CONFIG_MUSE_BOARD_WAVESHARE_S3_175", 0, call):call])

    def test_other_axp2101_boards_leave_it(self):
        pwr_talk = {"board_m5stack_cores3.c", "board_waveshare_s3_175c.c", "board_waveshare_s3_216.c"}
        others = [path.name for path in sorted((MUSE / "boards").glob("board_*.c"))
                  if "muse_pmu_init(" in path.read_text() and path.name not in pwr_talk]
        self.assertIn("board_waveshare_c6_18.c", others)   # the glob found the AXP2101 boards
        for name in others:
            self.assertNotIn("muse_pmu_set_key_shutdown", (MUSE / "boards" / name).read_text(), name)


# Fakes for the boards' poll_buttons(): GPIO buttons and the PMU's latched key
# edges, one queued per poll, and every muse_pmu_set_key_shutdown() call.
FAKES = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#define ESP_OK 0
#define ESP_FAIL -1
typedef int esp_err_t;
#define FAKE static __attribute__((unused))   /* not every board calls every fake */
typedef struct { unsigned next; } muse_gpio_button_t;
FAKE unsigned muse_gpio_button_poll(muse_gpio_button_t *b) { unsigned e = b->next; b->next = 0; return e; }
static unsigned s_pmu_key;
FAKE unsigned muse_pmu_poll_key(void) { unsigned e = s_pmu_key; s_pmu_key = 0; return e; }
static int calls;
static bool shutdown_on = true;
static esp_err_t set_err;
FAKE esp_err_t muse_pmu_set_key_shutdown(bool on) {
    ++calls;
    if (set_err) return set_err;
    shutdown_on = on;
    return ESP_OK;
}
'''


class PowerKeyTalkBoards(unittest.TestCase):
    """Held, a PWR that talks keeps the PMU from powering off; released, the
    hold-to-off works again. Other keys, and boards that don't talk on PWR,
    leave it alone."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        header = (MUSE / "muse_board.h").read_text()
        pmu = (MUSE / "muse_pmu.h").read_text()
        cls.flags = defines(header, r"MUSE_BTN_\w+") + "\n" + defines(pmu, r"MUSE_PMU_KEY_\w+") + "\n"

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build_and_run(self, name, code):
        path = Path(self.tmp.name) / (name + ".c")
        path.write_text(FAKES + self.flags + code)
        built = subprocess.run([*cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path),
                                "-o", str(path.with_suffix(""))], capture_output=True, text=True)
        self.assertEqual(built.returncode, 0, built.stderr)
        result = subprocess.run([str(path.with_suffix(""))], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def board(self, name):
        return (MUSE / "boards" / name).read_text()

    def test_waveshare_175c_pwr(self):
        source = self.board("board_waveshare_s3_175c.c")
        polls = functions(source, "poll_buttons")
        pwr = next(f for f in polls if "s_pwr" in f)
        self.build_and_run("s3_175c", r'''
static muse_gpio_button_t s_pwr, s_boot;
''' + pwr + r'''
int main(void) {
    s_pwr.next = MUSE_BTN_TALK_PRESS;
    assert(poll_buttons() == MUSE_BTN_TALK_PRESS);
    assert(calls == 1 && !shutdown_on);
    for (int i = 0; i < 1500; i++) assert(poll_buttons() == 0);   /* held 15 s, past MAX_SECS */
    assert(calls == 1 && !shutdown_on);
    s_pwr.next = MUSE_BTN_TALK_RELEASE;
    assert(poll_buttons() == MUSE_BTN_TALK_RELEASE);
    assert(calls == 2 && shutdown_on);
    s_boot.next = MUSE_BTN_TALK_PRESS;   /* BOOT is aux */
    assert(poll_buttons() == MUSE_BTN_AUX_PRESS);
    s_boot.next = MUSE_BTN_TALK_RELEASE;
    assert(poll_buttons() == MUSE_BTN_AUX_RELEASE && calls == 2);
    /* Held through boot, PWR's first edge is a release: on, as it was. */
    s_pwr.next = MUSE_BTN_TALK_RELEASE;
    assert(poll_buttons() == MUSE_BTN_TALK_RELEASE && calls == 3 && shutdown_on);
    /* The PMU not answering doesn't stop the talk. */
    set_err = ESP_FAIL;
    s_pwr.next = MUSE_BTN_TALK_PRESS;
    assert(poll_buttons() == MUSE_BTN_TALK_PRESS && calls == 4);
    return 0;
}
''')

    def test_waveshare_175_boot_talks_and_pwr_is_left_to_the_pmu(self):
        source = self.board("board_waveshare_s3_175c.c")
        polls = functions(source, "poll_buttons")
        pmu = next(f for f in polls if "muse_pmu_poll_key" in f)
        self.build_and_run("s3_175", defines(source, r"PMU_KEY_EVERY") + r'''
static muse_gpio_button_t s_boot;
''' + pmu + r'''
int main(void) {
    s_boot.next = MUSE_BTN_TALK_PRESS;
    assert(poll_buttons() == MUSE_BTN_TALK_PRESS);
    s_pmu_key = MUSE_PMU_KEY_PRESS;   /* PWR is aux; a long hold still powers off */
    unsigned ev = 0;
    for (int i = 0; i < 4; i++) ev |= poll_buttons();
    assert(ev == MUSE_BTN_AUX_PRESS);
    s_pmu_key = MUSE_PMU_KEY_RELEASE;
    ev = 0;
    for (int i = 0; i < 4; i++) ev |= poll_buttons();
    assert(ev == MUSE_BTN_AUX_RELEASE);
    assert(calls == 0);
    return 0;
}
''')

    def test_m5stack_cores3_pwr(self):
        source = self.board("board_m5stack_cores3.c")
        (poll,) = functions(source, "poll_buttons")
        self.build_and_run("cores3", defines(source, r"PMU_KEY_EVERY") + r'''
static bool s_pmu = true;
''' + poll + r'''
/* The PMU is read every PMU_KEY_EVERY polls. */
static unsigned poll_pmu(unsigned key) {
    s_pmu_key = key;
    unsigned ev = 0;
    for (int i = 0; i < PMU_KEY_EVERY; i++) ev |= poll_buttons();
    assert(s_pmu_key == 0);
    return ev;
}
int main(void) {
    assert(poll_pmu(MUSE_PMU_KEY_PRESS) == MUSE_BTN_TALK_PRESS);
    assert(calls == 1 && !shutdown_on);
    for (int i = 0; i < 750; i++) assert(poll_pmu(0) == 0);   /* held 15 s */
    assert(calls == 1);
    assert(poll_pmu(MUSE_PMU_KEY_RELEASE | MUSE_PMU_KEY_LONG) == MUSE_BTN_TALK_RELEASE);
    assert(calls == 2 && shutdown_on);
    /* A tap inside one read: press then release, so still released. */
    assert(poll_pmu(MUSE_PMU_KEY_PRESS | MUSE_PMU_KEY_RELEASE | MUSE_PMU_KEY_CLICK) ==
           (MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE));
    assert(calls == 2 && shutdown_on);
    /* Held, then release and a new press inside one read: still held. */
    poll_pmu(MUSE_PMU_KEY_PRESS);
    assert(calls == 3 && !shutdown_on);
    poll_pmu(MUSE_PMU_KEY_RELEASE | MUSE_PMU_KEY_PRESS);
    assert(calls == 3 && !shutdown_on);
    poll_pmu(MUSE_PMU_KEY_RELEASE);
    assert(calls == 4 && shutdown_on);
    /* The PMU not answering doesn't stop the talk. */
    set_err = ESP_FAIL;
    assert(poll_pmu(MUSE_PMU_KEY_PRESS) == MUSE_BTN_TALK_PRESS && calls == 5);
    return 0;
}
''')

    def test_waveshare_216_pwr_but_not_key3(self):
        source = self.board("board_waveshare_s3_216.c")
        (poll,) = functions(source, "poll_buttons")
        self.build_and_run("s3_216", defines(source, r"PMU_KEY_EVERY") + r'''
static muse_gpio_button_t s_key, s_boot;
''' + poll + r'''
static unsigned poll_pmu(unsigned key) {
    s_pmu_key = key;
    unsigned ev = 0;
    for (int i = 0; i < PMU_KEY_EVERY; i++) ev |= poll_buttons();
    assert(s_pmu_key == 0);
    return ev;
}
int main(void) {
    /* KEY3 talks without the PMU. */
    s_key.next = MUSE_BTN_TALK_PRESS;
    assert(poll_pmu(0) == MUSE_BTN_TALK_PRESS);
    s_key.next = MUSE_BTN_TALK_RELEASE;
    assert(poll_pmu(0) == MUSE_BTN_TALK_RELEASE);
    assert(calls == 0);
    /* PWR does. */
    assert(poll_pmu(MUSE_PMU_KEY_PRESS) == MUSE_BTN_TALK_PRESS);
    assert(calls == 1 && !shutdown_on);
    for (int i = 0; i < 750; i++) assert(poll_pmu(0) == 0);
    assert(poll_pmu(MUSE_PMU_KEY_RELEASE) == MUSE_BTN_TALK_RELEASE);
    assert(calls == 2 && shutdown_on);
    /* A whole press inside one read is dropped, and so leaves it on. */
    assert(poll_pmu(MUSE_PMU_KEY_PRESS | MUSE_PMU_KEY_RELEASE) == 0);
    assert(calls == 2 && shutdown_on);
    /* KEY3 held through a PWR press: PWR still decides. */
    s_key.next = MUSE_BTN_TALK_PRESS;
    assert(poll_pmu(0) == MUSE_BTN_TALK_PRESS);
    assert(poll_pmu(MUSE_PMU_KEY_PRESS) == 0 && calls == 3 && !shutdown_on);
    assert(poll_pmu(MUSE_PMU_KEY_RELEASE) == 0 && calls == 4 && shutdown_on);
    s_key.next = MUSE_BTN_TALK_RELEASE;
    assert(poll_pmu(0) == MUSE_BTN_TALK_RELEASE && calls == 4);
    /* BOOT is aux. */
    s_boot.next = MUSE_BTN_TALK_PRESS;
    assert(poll_pmu(0) == MUSE_BTN_AUX_PRESS && calls == 4);
    return 0;
}
''')
