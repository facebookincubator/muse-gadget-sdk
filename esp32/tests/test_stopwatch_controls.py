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

"""Compile StopWatch production PMIC/input/UI handlers against host fakes."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


def function(source, name):
    match = re.search(r"static (?:unsigned|int|void|esp_err_t) " + name + r"\([^)]*\)\n\{", source)
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class StopWatchControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.board = (MUSE / "boards/board_m5stack_stopwatch.c").read_text()
        cls.ui = (MUSE / "muse_ui.c").read_text()
        cls.input = (MUSE / "muse_input.c").read_text()
        flags = "\n".join(re.findall(r"^#define MUSE_BTN_.*", (MUSE / "muse_board.h").read_text(), re.M))
        board_defines = "\n".join(re.findall(r"^#define (?:PMIC_BTN_\w+|PMIC_SYS_CMD|PMIC_SHUTDOWN|POWER_\w+).*", cls.board, re.M))
        common = '#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n' + flags + '\n' + '#include "' + str(MUSE / 'muse_state.h') + '"\n'
        board = common + r'''
#define BIT(n) (1u << (n))
#define ESP_OK 0
#define ESP_FAIL -1
#define pdMS_TO_TICKS(n) (n)
#define TAG "test"
typedef int esp_err_t;
#define ESP_RETURN_ON_ERROR(expr, tag, msg) do { int e = (expr); if(e) return e; } while(0)
static void *s_pmic;
static bool s_power_pressed = true, s_power_candidate = true, s_power_armed, s_power_long;
static int64_t s_power_changed_at, s_power_held_at, now = 10000;
static int read_error, write_error, writes;
static uint8_t key;
static int64_t esp_timer_get_time(void) { return now; }
''' + board_defines + r'''
static int reg_read(void *dev, uint8_t reg, uint8_t *out, size_t n) {
    (void)dev; assert(reg == 0x48 && n == 1); *out = key; return read_error;
}
static int reg_write(void *dev, uint8_t reg, const uint8_t *data, size_t n) {
    (void)dev; assert(reg == 0x0c && n == 1 && data[0] == 0xa1); ++writes; return write_error;
}
static void vTaskDelay(int ms) { (void)ms; }
typedef struct { int id; } muse_gpio_button_t;
static muse_gpio_button_t s_talk = { 1 }, s_speaker = { 2 };
static unsigned talk_ev, speaker_ev;
static unsigned muse_gpio_button_poll(muse_gpio_button_t *b) { return b->id == 1 ? talk_ev : speaker_ev; }
''' + '\n'.join(function(cls.board, n) for n in ('poll_power_button', 'poll_buttons', 'power_off')) + r'''
static unsigned sample(uint8_t value, int us) { key = value; now += us; return poll_power_button(); }
static void arm(void) { assert(sample(0, 10000) == 0); assert(sample(0, 30000) == 0); assert(s_power_armed); }
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    if (test == 0) {
        assert(sample(0x81, 10000) == 0); assert(sample(1, 2000000) == 0);
        arm(); /* boot/wake hold is swallowed; bit7 isn't a held state */
        assert(sample(0x80, 50000) == 0);
        assert(sample(1, 10000) == 0); assert(sample(0, 10000) == 0);
        assert(sample(1, 10000) == 0); assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        assert(sample(1, 50000) == 0);
        assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
    } else if (test == 1) {
        arm(); assert(sample(1, 10000) == 0); assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        assert(sample(1, 1499999) == 0); assert(sample(1, 1) == MUSE_BTN_POWER_LONG);
        assert(sample(1, 2000000) == 0);
        assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
    } else if (test == 2) {
        arm(); read_error = -7; assert(sample(1, 3000000) == 0);
        read_error = 0; assert(sample(1, 10000) == 0); assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        read_error = -7; assert(sample(0, 3000000) == 0); assert(s_power_pressed);
        read_error = 0; assert(sample(1, 10000) == 0); assert(sample(1, 1499999) == 0);
        assert(sample(1, 1) == MUSE_BTN_POWER_LONG);
    } else if (test == 3) {
        arm(); talk_ev = MUSE_BTN_TALK_PRESS; speaker_ev = MUSE_BTN_TALK_PRESS;
        assert(poll_buttons() == (MUSE_BTN_TALK_PRESS | MUSE_BTN_SPEAKER_PRESS));
        talk_ev = MUSE_BTN_TALK_RELEASE; speaker_ev = MUSE_BTN_TALK_RELEASE;
        assert(poll_buttons() == (MUSE_BTN_TALK_RELEASE | MUSE_BTN_SPEAKER_RELEASE));
    } else {
        write_error = -7; assert(power_off() == -7 && writes == 1);
        write_error = 0; assert(power_off() == ESP_FAIL && writes == 2);
    }
    return 0;
}
'''
        inp = common + r'''
static bool s_power_down, s_power_swallow, s_talk_down;
static bool asleep, speaker = true;
static int shutdowns, ptt_releases, toggles, pokes;
static muse_mode_t mode = MUSE_MODE_IDLE;
static struct { const char *power_button, *speaker_button; } board = {"red", "yellow"}, *muse_board = &board;
bool muse_state_asleep(void) { return asleep; }
static void set_asleep(bool value, const char *why) { (void)why; asleep = value; }
void muse_state_poke(void) { ++pokes; }
muse_mode_t muse_state_mode(float *secs) { (void)secs; return mode; }
static bool muse_settings_speaker_on(void) { return speaker; }
static void muse_settings_set_speaker_on(bool value) { speaker = value; ++toggles; }
void muse_state_set_caption(const char *caption, ...) { assert(strcmp(caption, speaker ? "SPEAKER ON" : "SPEAKER OFF") == 0); }
static void talk_button(unsigned ev) { assert(ev == MUSE_BTN_TALK_RELEASE); s_talk_down = false; ++ptt_releases; }
static void power_off(void) { ++shutdowns; }
''' + '\n'.join(function(cls.input, n) for n in ('power_release', 'dedicated_buttons')) + r'''
int main(int argc, char **argv) {
    assert(argc == 2); int test = atoi(argv[1]);
    if (test == 0) {
        dedicated_buttons(MUSE_BTN_POWER_PRESS); assert(!asleep && shutdowns == 0 && toggles == 0);
        dedicated_buttons(MUSE_BTN_POWER_RELEASE); assert(asleep);
        dedicated_buttons(MUSE_BTN_POWER_PRESS); assert(!asleep);
        dedicated_buttons(MUSE_BTN_POWER_LONG | MUSE_BTN_POWER_RELEASE); assert(!asleep && shutdowns == 0);
        dedicated_buttons(MUSE_BTN_POWER_PRESS | MUSE_BTN_POWER_RELEASE); assert(asleep);
    } else if (test == 1) {
        s_talk_down = true;
        dedicated_buttons(MUSE_BTN_POWER_PRESS); dedicated_buttons(MUSE_BTN_POWER_LONG);
        assert(shutdowns == 1 && ptt_releases == 1);
        dedicated_buttons(MUSE_BTN_POWER_LONG | MUSE_BTN_POWER_RELEASE);
        assert(shutdowns == 1 && !asleep); /* failed shutdown remains usable */
        dedicated_buttons(MUSE_BTN_POWER_LONG); assert(shutdowns == 1); /* no press, no off */
    } else if (test == 2) {
        dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); assert(!speaker && toggles == 1 && shutdowns == 0);
        dedicated_buttons(MUSE_BTN_SPEAKER_RELEASE); assert(toggles == 1);
        mode = MUSE_MODE_SPEAKING; dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); assert(speaker && toggles == 2);
        asleep = true; dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); assert(!asleep && toggles == 2);
        mode = MUSE_MODE_OFF; dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); assert(toggles == 2);
    } else {
        dedicated_buttons(MUSE_BTN_AUX_PRESS | MUSE_BTN_AUX_RELEASE | MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE);
        assert(!asleep && toggles == 0 && shutdowns == 0 && pokes == 0); /* other boards untouched */
    }
    return 0;
}
'''
        ui = common + r'''
#define LV_OBJ_FLAG_HIDDEN 1
#define LV_SYMBOL_VOLUME_MID "\xef\x80\xa7"
#define LV_SYMBOL_MUTE "\xef\x80\xa6"
#define LV_SYMBOL_POWER "\xef\x80\x91"
static struct { char text[64]; bool hidden; } hints[3];
typedef void lv_obj_t;
static lv_obj_t *s_ptt_hint = &hints[0], *s_spk_hint = &hints[1], *s_pwr_hint = &hints[2];
static lv_obj_t *s_tv = (void *)1, *s_settings = (void *)2;
static bool settings, speaker = true;
static uint32_t mic_color;
static int lv_calls;
static void color_mic(void *icon, uint32_t color) { assert(icon == s_ptt_hint); mic_color = color; ++lv_calls; }
static void *lv_tileview_get_tile_active(void *tv) { (void)tv; return settings ? s_settings : 0; }
static void lv_obj_set_flag(void *obj, int flag, bool value) { (void)flag; ((typeof(hints[0]) *)obj)->hidden = value; ++lv_calls; }
static const char *lv_label_get_text(void *obj) { return ((typeof(hints[0]) *)obj)->text; }
static void lv_label_set_text(void *obj, const char *text) { strcpy(((typeof(hints[0]) *)obj)->text, text); }
static void lv_obj_set_style_text_color(void *obj, int color, int part) { (void)obj; (void)color; (void)part; }
static int lv_color_hex(int color) { return color; }
static bool muse_settings_speaker_on(void) { return speaker; }
''' + function(cls.ui, 'update_control_hints') + r'''
int main(void) {
    strcpy(hints[2].text, LV_SYMBOL_POWER); /* created once, never replaced by words */
    for(int m = MUSE_MODE_BOOT; m <= MUSE_MODE_OFF; ++m) {
        update_control_hints(m);
        assert(!hints[0].hidden && !hints[1].hidden && !hints[2].hidden);
        assert(hints[0].text[0] == 0); /* mic is primitives, not a text label */
        assert(strcmp(hints[1].text, LV_SYMBOL_VOLUME_MID) == 0);
        assert(strcmp(hints[2].text, LV_SYMBOL_POWER) == 0);
        assert(mic_color == (m == MUSE_MODE_LISTENING ? 0x64d4ff : 0x80bfff));
    }
    speaker = false; update_control_hints(MUSE_MODE_SPEAKING); assert(strcmp(hints[1].text, LV_SYMBOL_MUTE) == 0);
    settings = true; update_control_hints(MUSE_MODE_IDLE); assert(hints[0].hidden && hints[1].hidden && hints[2].hidden);
    settings = false; update_control_hints(MUSE_MODE_ERROR); assert(!hints[0].hidden);
    s_ptt_hint = NULL; int before = lv_calls;
    update_control_hints(MUSE_MODE_IDLE); assert(lv_calls == before); /* legacy board unchanged */
    return 0;
}
'''
        for name, code in (("board", board), ("input", inp), ("ui", ui)):
            path = Path(cls.tmp.name) / (name + '.c')
            path.write_text(code)
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=gnu11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(path.with_suffix(''))], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, binary, case):
        subprocess.run([str(Path(self.tmp.name) / binary), str(case)], check=True, capture_output=True, text=True)

    def test_pmic_boot_flag_and_debounce(self): self.run_case('board', 0)
    def test_pmic_hold_once(self): self.run_case('board', 1)
    def test_pmic_read_errors_do_not_create_edges_or_holds(self): self.run_case('board', 2)
    def test_gpio_events_are_independent(self): self.run_case('board', 3)
    def test_pmic_shutdown_command_and_failure(self): self.run_case('board', 4)
    def test_power_sleep_wake_and_latched_short(self): self.run_case('input', 0)
    def test_power_hold_consumed_after_failure_and_ends_ptt(self): self.run_case('input', 1)
    def test_speaker_toggle_and_wake_only(self): self.run_case('input', 2)
    def test_legacy_aux_and_talk_not_reinterpreted(self): self.run_case('input', 3)
    def test_icon_hints_all_modes_mute_and_settings(self): self.run_case('ui', 0)

    def test_settings_power_hint_uses_red_not_ptt(self):
        source = (MUSE / 'muse_settings_ui.c').read_text()
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef void lv_obj_t;
#define LV_SYMBOL_POWER "P"
#define COLOR_DANGER 1
#define COLOR_TEXT 2
static lv_obj_t *s_power;
static char shown[128];
static struct { const char *power_button, *talk_button, *aux_button; } board = {"red bottom-left", "top-right", NULL}, *muse_board = &board;
static void on_power_off(void *p) { (void)p; }
static void on_back(void *p) { (void)p; }
static void *page(void *tile, const char *title, bool back, void **list) { (void)tile; (void)title; (void)back; *list = (void *)1; return *list; }
static void note(void *list, const char *text) { (void)list; strcpy(shown, text); }
static void button(void *list, const char *text, int color, void (*cb)(void *), void *data) { (void)list; (void)text; (void)color; (void)cb; (void)data; }
''' + function(source, 'build_power_page') + r'''
int main(void) {
    build_power_page(NULL);
    assert(strcmp(shown, "Press the red bottom-left button to turn it back on. To just turn the screen off, press the red bottom-left button.") == 0);
    board.power_button = NULL; board.talk_button = "talk"; board.aux_button = "aux";
    build_power_page(NULL);
    assert(strcmp(shown, "Press the talk button to turn it back on. To just turn the screen off, press the aux button.") == 0);
    return 0;
}
'''
        path = Path(self.tmp.name) / 'settings.c'
        path.write_text(code)
        subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(path.with_suffix(''))], check=True, capture_output=True, text=True)
        self.run_case('settings', 0)

    def test_positions_and_round_safe_area(self):
        self.assertRegex(self.board, r'#define TALK_GPIO GPIO_NUM_1')
        self.assertRegex(self.board, r'#define SPEAKER_GPIO GPIO_NUM_2')
        self.assertIn('.wait_buttons = NULL', self.board)
        self.assertNotIn('esp_sleep_enable_ext1_wakeup', self.board)
        for name, sign_x, sign_y in [('talk', 1, -1), ('speaker', -1, -1), ('power', -1, 1)]:
            x, y = map(int, re.search(r'\.' + name + r'_hint = \{ LV_ALIGN_CENTER, (-?\d+), (-?\d+) \}', self.board).groups())
            self.assertGreater(x * sign_x, 0)
            self.assertGreater(y * sign_y, 0)
            for dx in [-18, 18]:
                for dy in [-18, 18]: self.assertLess((x + dx) ** 2 + (y + dy) ** 2, 223 ** 2)
        self.assertIn('s_ptt_hint = make_mic(scr, 26)', self.ui)
        self.assertNotIn('s_mic_icon = make_mic(scr', self.ui)
        self.assertNotIn('s_ptt_hint = s_mic_icon', self.ui)
        self.assertNotIn('s_ptt_hint', function(self.ui, 'add_hides'))
        self.assertIn('make_control_icon(scr, &muse_board->speaker_hint, LV_SYMBOL_VOLUME_MID', self.ui)
        self.assertIn('make_control_icon(scr, &muse_board->power_hint, LV_SYMBOL_POWER', self.ui)
        self.assertNotRegex(function(self.ui, 'update_control_hints'), r'"(?:PTT|SPK|POWER|PRESS|HOLD)')
        self.assertIn('muse_board->rim_controls ? 280 : s_w', self.ui)
        self.assertIn('muse_board->rim_controls ? 64', self.ui)
        self.assertIn('muse_board->rim_controls ? 88', self.ui)


if __name__ == '__main__':
    unittest.main()
