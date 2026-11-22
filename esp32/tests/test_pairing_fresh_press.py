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

"""Pairing confirms only with a press that began after its prompt.

Compiles the real debounced GPIO button, talk-button input, Muse's confirm
press and the Link button task, with Link's check (a press that began after
the prompt was armed) modelled as link_pairing_confirm_active_session() makes it.
"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


def block(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


MUSE_HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MUSE_BTN_TALK_PRESS 1u
#define MUSE_BTN_TALK_RELEASE 2u
#define MUSE_BTN_TALK_HELD (1u << 17)
typedef int gpio_num_t;
typedef struct { gpio_num_t gpio; bool active_high; bool pressed; uint8_t stable; } muse_gpio_button_t;
static int64_t clock_us;
static int level;                       /* the button's pin */
static int gpio_get_level(gpio_num_t gpio) { (void)gpio; return level; }
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool muse_state_asleep(void) { return false; }
static void muse_state_poke(void) {}
static bool muse_menu_is_open(void) { return false; }
static void muse_menu_key(int key) { (void)key; }
static void set_asleep(bool on, const char *why) { (void)on; (void)why; }
enum { MUSE_MENU_SELECT, MUSE_PTT_DOWN, MUSE_PTT_UP };
static int downs, ups;
static void post(int type, bool wake) { (void)wake; downs += type == MUSE_PTT_DOWN; ups += type == MUSE_PTT_UP; }
static int fake_mic(void) { return 0; }
static const struct { int (*audio_init)(void), (*mic_init)(void); bool keyboard; const char *talk_button; } board = {
    .mic_init = fake_mic, .talk_button = "boot" }, *muse_board = &board;
static bool s_talk_down;
static int64_t s_talk_released_us;
/* Tap-to-toggle state (muse_input.c, tap-to-toggle PR): this prelude is
 * maintained with the input state it stands in for; talk_release is
 * stubbed to the pre-latch behaviour (the latch itself is covered by
 * the ptt_ui harness). */
#define TAP_TICKS 300u
#define TAP_LATCH_TICKS 20000u
static bool s_tap_latched, s_tap_swallow_release, s_press_wake;
static uint32_t s_press_tick, s_latch_tick;
static uint32_t xTaskGetTickCount(void) { return (uint32_t)(clock_us / 1000); }
static void talk_release(void) { post(MUSE_PTT_UP, false); }

/* Link: a pairing waiting for its press (confirm_required), its prompt armed
 * at armed_us. app_confirm_pairing_press() and link_pairing_confirm_active_session(). */
static bool pending, armed;
static int64_t armed_us;
static int confirms;
static bool link_press(int64_t pressed_after_us) {
    if (!pending) return false;
    if (armed && pressed_after_us >= armed_us) { confirms++; pending = armed = false; }
    return true;
}
typedef struct { bool (*talk_press)(int64_t pressed_after_us); } muse_link_ops_t;
static const muse_link_ops_t ops = { .talk_press = link_press }, *s_ops = &ops;
static void prompt(int64_t ms) { pending = armed = true; armed_us = ms * 1000; }
bool muse_link_confirm_press(int64_t released_us);
bool muse_link_talk_press(void);
'''

MUSE_CASES = r'''
static muse_gpio_button_t btn = { .gpio = 0, .active_high = false, .pressed = false };
/* The input task's 10 ms poll, from `from` to `to` ms. */
static void run(int64_t from, int64_t to) {
    for (int64_t ms = from; ms < to; ms += 10) {
        clock_us = ms * 1000;
        unsigned ev = muse_gpio_button_poll(&btn);
        if (ev) talk_button(ev);
    }
}
static void reset(void) {
    memset(&btn, 0, sizeof(btn)); level = 1;
    s_talk_down = false; s_talk_released_us = 0;
    pending = armed = false; confirms = downs = ups = 0;
    run(0, 50); talk_button(MUSE_BTN_TALK_RELEASE);   /* clear talk_input's swallow */
    s_talk_released_us = 0;
}

/* Pressed just before the prompt and held: debouncing reports the press after
 * the prompt was armed, and it doesn't confirm. Let go and press: confirms. */
static void pressed_before_the_prompt(void) {
    reset();
    run(50, 1000);
    level = 0;                 /* the press starts */
    run(1000, 1010);           /* one sample in */
    prompt(1005);
    run(1010, 1500);           /* its edge lands at 1020 ms, after the prompt */
    assert(!confirms && pending && !downs);   /* Link took the press, but it doesn't confirm */
    level = 1; run(1500, 1600);
    level = 0; run(1600, 1700);
    assert(confirms == 1 && !pending && !downs && !ups);
}

/* Held from before the prompt, then released and pressed again soon after it:
 * the new press began after its release, which came after the prompt. */
static void quick_release_and_press(void) {
    reset();
    level = 0; run(50, 1000);  /* pressed and held: a voice note */
    assert(downs == 1);
    prompt(1000);
    run(1000, 1050);
    assert(!confirms && pending);
    level = 1; run(1050, 1100);
    assert(ups == 1);
    level = 0; run(1100, 1150);   /* 120 ms after the prompt */
    assert(confirms == 1 && downs == 1);
}

/* The usual case: the prompt, then a press. */
static void press_after_the_prompt(void) {
    reset();
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 1500);
    assert(confirms == 1 && !downs);
    level = 1; run(1500, 1600);
    assert(!ups && !s_talk_down);
}

/* A press that began under one prompt doesn't confirm the next one. */
static void replaced_prompt(void) {
    reset();
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 1410);
    prompt(1415);              /* another phone, mid-debounce */
    run(1410, 1600);
    assert(!confirms && pending);
    level = 1; run(1600, 1700);
    level = 0; run(1700, 1800);
    assert(confirms == 1);
}

/* A finger held on the face to talk is reported once the hold passes 500 ms
 * (MUSE_BTN_TALK_HELD), long after it began: it talks and never confirms, even
 * a hold that began after the prompt. The talk button then confirms. */
static void held_face(void) {
    reset();
    prompt(1100);              /* the finger went down at 1000 ms */
    clock_us = 1500 * 1000; talk_button(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_HELD);
    assert(!confirms && pending && downs == 1);
    clock_us = 2000 * 1000; talk_button(MUSE_BTN_TALK_RELEASE);
    clock_us = 3000 * 1000; talk_button(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_HELD);
    assert(!confirms && pending && downs == 2);
    clock_us = 3500 * 1000; talk_button(MUSE_BTN_TALK_RELEASE);
    run(3500, 3600); level = 0; run(3600, 3700);
    assert(confirms == 1 && downs == 2 && ups == 2);
}

/* Screen confirmation: touch taps report press and release together
 * (muse_input_touch_talk()), and boards that confirm on a tap themselves call
 * muse_link_talk_press(). A tap too soon after the prompt to have begun after
 * it doesn't confirm; the next one does. */
static void screen_taps(void) {
    reset();
    prompt(1000);
    clock_us = 1100 * 1000; talk_button(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE);
    assert(!confirms && pending && !downs);
    clock_us = 1180 * 1000; talk_button(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE);
    assert(confirms == 1 && !downs && !ups);

    reset();
    prompt(1000);
    clock_us = 1100 * 1000; assert(muse_link_talk_press() && !confirms);
    clock_us = 1300 * 1000; assert(muse_link_talk_press() && confirms == 1);
    assert(!muse_link_talk_press());   /* nothing waits: the press is the board's */
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "before")) pressed_before_the_prompt();
    else if (!strcmp(argv[1], "quick")) quick_release_and_press();
    else if (!strcmp(argv[1], "after")) press_after_the_prompt();
    else if (!strcmp(argv[1], "replaced")) replaced_prompt();
    else if (!strcmp(argv[1], "screen")) screen_taps();
    else if (!strcmp(argv[1], "held")) held_face();
    else return 2;
    return 0;
}
'''

# The Link button (boards without the Muse UI): its short press is reported
# after the release, with when the button was last seen up before the press.
BUTTON_HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "fakes.h"
#include "stack_monitor.h"
static int64_t clock_us, end_us;
static int64_t press_from_us[2], press_to_us[2];
static jmp_buf done;
int64_t esp_timer_get_time(void) { return clock_us; }
int gpio_get_level(int gpio) {
    (void)gpio;
    for (int i = 0; i < 2; i++)
        if (clock_us >= press_from_us[i] && clock_us < press_to_us[i]) return 0;
    return 1;
}
void vTaskDelay(int ticks) { clock_us += (int64_t)ticks * 1000; if (clock_us >= end_us) longjmp(done, 1); }
void stack_monitor_poll(stack_monitor_t *s) { (void)s; }
#include "button.c"
static int shorts;
static int64_t after_us[2];
static void on_short(void) { after_us[shorts++] = button_pressed_after_us(); }
static void on_other(void) { assert(0); }
int main(void) {
    s_short_press_cb = on_short; s_double_press_cb = on_other; s_long_press_cb = on_other;
    clock_us = 0; end_us = 3000 * 1000;
    press_from_us[0] = 210 * 1000; press_to_us[0] = 520 * 1000;    /* across a prompt armed at 300 ms */
    press_from_us[1] = 1210 * 1000; press_to_us[1] = 1420 * 1000;
    if (!setjmp(done)) button_task(NULL);
    assert(shorts == 2);
    /* The first press began before 300 ms: Link refuses it. The second began after. */
    assert(after_us[0] < 300 * 1000 && after_us[0] >= 150 * 1000);
    assert(after_us[1] >= 1150 * 1000 && after_us[1] < 1210 * 1000);
    puts("ok");
    return 0;
}
'''

BUTTON_FAKES = r'''
#pragma once
#include <stdint.h>
#define CONFIG_HOMEHUB_BUTTON_GPIO 0
#define CONFIG_HOMEHUB_VOICE 0
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
static inline esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
int gpio_get_level(int gpio);
int64_t esp_timer_get_time(void);
void vTaskDelay(int ticks);
#define pdMS_TO_TICKS(ms) (ms)
static inline int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int prio, void *handle)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    return 1;
}
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
'''


class PairingFreshPress(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        if not cls.cc or not shutil.which(cls.cc[0]):
            raise unittest.SkipTest("C compiler unavailable")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        board = (MUSE / "muse_board.c").read_text()
        source = (MUSE / "muse_input.c").read_text()
        link = (MUSE / "muse_link.c").read_text()
        code = MUSE_HARNESS
        code += block(board, "#define DEBOUNCE_SAMPLES", "#define DEBOUNCE_POLL_MS")
        code += block(board, "static bool raw_pressed(", "\nvoid muse_gpio_buttons_wait(")
        code += block(source, "/* Talk button: push-to-talk", "\n/* Keyboard menus")
        code += block(link, "/* Longer than a press takes", "\nvoid muse_link_reset_setup(")
        code += MUSE_CASES
        (out / "muse.c").write_text(code)
        flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-g",
                 "-fsanitize=address,undefined"]
        subprocess.run([*cls.cc, *flags, str(out / "muse.c"), "-o", str(out / "muse")], check=True)
        (out / "inc").mkdir()
        for name in ("driver/gpio.h", "freertos/FreeRTOS.h", "freertos/task.h", "esp_log.h", "esp_timer.h"):
            path = out / "inc" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fakes.h"\n')
        (out / "inc/fakes.h").write_text(BUTTON_FAKES)
        (out / "button_harness.c").write_text(BUTTON_HARNESS)
        subprocess.run([*cls.cc, *flags, "-Wno-unused-parameter", "-I", str(out / "inc"), "-I", str(ROOT / "main"),
                        str(out / "button_harness.c"), "-o", str(out / "button")], check=True)
        cls.out = out

    def run_case(self, case):
        result = subprocess.run([str(self.out / "muse"), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_press_begun_before_the_prompt_and_held_does_not_confirm(self):
        self.run_case("before")

    def test_release_then_press_confirms(self):
        self.run_case("quick")

    def test_press_after_the_prompt_confirms(self):
        self.run_case("after")

    def test_a_press_for_a_replaced_prompt_does_not_confirm(self):
        self.run_case("replaced")

    def test_a_face_held_to_talk_does_not_confirm(self):
        self.run_case("held")

    def test_screen_confirmation_still_works(self):
        self.run_case("screen")

    def test_link_button_reports_when_its_short_press_began(self):
        result = subprocess.run([str(self.out / "button")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
