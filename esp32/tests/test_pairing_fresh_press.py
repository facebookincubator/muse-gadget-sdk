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

"""One press confirms a Muse Gadget's pairing prompt, a first setup or a re-setup, if it went down after the prompt showed.

Compiles the real debounced GPIO button, talk-button input, Muse's confirm
press and the Link button task, with a gadget build's Link (one press while
the prompt is armed confirms: no hold, no press time, any press) modelled as
link_pairing_confirm_active_session() makes it. A press that went down before
the prompt showed (muse_link_prompt_seq: ordering, not time) confirms
nothing and does nothing else. The Link
button's short press still reports when the button was last seen up (Home
Link's own check, link_pairing.c).
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
static void muse_state_set_asleep(bool on) { (void)on; }
static void muse_state_poke(void) {}
static bool muse_menu_is_open(void) { return false; }
static void muse_menu_key(int key) { (void)key; }
static void set_asleep(bool on, const char *why) { (void)on; (void)why; }
enum { MUSE_MENU_SELECT, MUSE_PTT_DOWN, MUSE_PTT_UP };
static int downs, ups;
static void post(int type, bool wake) { (void)wake; downs += type == MUSE_PTT_DOWN; ups += type == MUSE_PTT_UP; }
static int fake_mic(void) { return 0; }
static struct { int (*audio_init)(void), (*mic_init)(void); bool keyboard, touch; const char *talk_button; } board = {
    .mic_init = fake_mic, .talk_button = "boot" }, *muse_board = &board;
static bool s_talk_down;
static int64_t s_talk_released_us;
#define MUSE_PASS_WHY_TALK 2
static bool muse_passcode_locked_at_press(void) { return false; }   /* test_muse_passcode covers the lock */
static void muse_passcode_request_keypad(int why) { (void)why; }

/* Link on a gadget build: a pairing waiting for its press (confirm_required), its prompt armed: one press
 * confirms, whenever it began (app_confirm_pairing_press(), link_pairing_confirm_active_session()). On a set-up
 * gadget outside setup mode the talk key's press only dismisses it (test_link_gadget_setup runs app.c's). */
static bool pending, armed, set_up, setup_mode;
static int confirms, dismisses;
static bool link_press(int64_t pressed_after_us, bool talk_key) {
    (void)pressed_after_us;
    if (!pending) return false;
    if (talk_key && set_up && !setup_mode) { dismisses++; pending = armed = false; return true; }
    if (armed) { confirms++; pending = armed = false; }
    return true;
}
static void link_dismiss(void) { dismisses++; pending = armed = false; }   /* Not now */
static void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
typedef struct {
    bool (*talk_press)(int64_t pressed_after_us, bool talk_key);
    void (*confirm_dismiss)(void);
} muse_link_ops_t;
static const muse_link_ops_t ops = { .talk_press = link_press, .confirm_dismiss = link_dismiss }, *s_ops = &ops;
enum { MUSE_LINK_CONFIRM = 3 };
static int muse_link_state(void) { return pending ? MUSE_LINK_CONFIRM : 0; }
/* The prompt's showings (muse_link.c's, bumped as each prompt arms: at its first showing on a screen), whether the
 * one up is armed, and where the talk press being handled went down (muse_input.c's input task: a key's at its
 * edge, a touch's at its touch-down). */
static unsigned prompt_seq;
static bool prompt_armed;
uint32_t muse_link_prompt_seq(void);
uint32_t muse_link_prompt_seq(void) { return prompt_seq; }
bool muse_link_prompt_armed(void);
bool muse_link_prompt_armed(void) { return prompt_armed; }
static uint32_t s_down_seq;
static const char *TAG = "muse_link";
#define ESP_LOGI(tag, ...) ((void)(tag))
/* Link's prompt comes (the state becomes MUSE_LINK_CONFIRM), then its first frame on the screen arms it. */
static void prompt_up(void) { pending = armed = true; prompt_armed = false; }
static void prompt_shows(void) { prompt_seq++; prompt_armed = true; }
static void prompt(int64_t ms) { (void)ms; prompt_up(); prompt_shows(); }
/* A touch whose down was seen at `seq`, reported now as `ev`. */
static void talk_button(unsigned ev);
static void touch(unsigned ev, unsigned seq) { s_down_seq = seq; talk_button(ev); }
bool muse_link_confirm_press(int64_t released_us, uint32_t down_seq);
bool muse_link_talk_press(void);
bool muse_link_confirm_key_press(void);
bool muse_link_bench_confirm(void);
bool muse_link_confirm_dismiss(void);
'''

MUSE_CASES = r'''
static muse_gpio_button_t btn = { .gpio = 0, .active_high = false, .pressed = false };
/* The input task's 10 ms poll, from `from` to `to` ms. */
static void run(int64_t from, int64_t to) {
    for (int64_t ms = from; ms < to; ms += 10) {
        clock_us = ms * 1000;
        unsigned ev = muse_gpio_button_poll(&btn);
        s_down_seq = muse_link_prompt_seq();   /* a key's press went down at its (debounced) edge */
        if (ev) talk_button(ev);
    }
}
static void reset(void) {
    memset(&btn, 0, sizeof(btn)); level = 1; board.touch = false;
    s_talk_down = false; s_talk_released_us = 0;
    pending = armed = set_up = setup_mode = false; confirms = downs = ups = dismisses = 0;
    run(0, 50); talk_button(MUSE_BTN_TALK_RELEASE);   /* clear talk_input's swallow */
    s_talk_released_us = 0;
}

/* Pressed just before the prompt: debouncing reports the press after the
 * prompt was armed, and that one press confirms (no edge check). */
static void pressed_before_the_prompt(void) {
    reset();
    run(50, 1000);
    level = 0;                 /* the press starts */
    run(1000, 1010);           /* one sample in */
    prompt(1005);
    run(1010, 1500);           /* its edge lands at 1020 ms, after the prompt */
    assert(confirms == 1 && !pending && !downs);
    level = 1; run(1500, 1600);
    assert(!ups && !s_talk_down);
}

/* Held from before the prompt (a voice note): no press edge while the prompt
 * is up, so nothing confirms until the next press, which does. */
static void quick_release_and_press(void) {
    reset();
    level = 0; run(50, 1000);  /* pressed and held: a voice note */
    assert(downs == 1);
    prompt(1000);
    run(1000, 1050);
    assert(!confirms && pending);
    level = 1; run(1050, 1100);
    assert(ups == 1);
    level = 0; run(1100, 1150);
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

/* A press under way when another phone's prompt replaces the first confirms
 * the one that's up when it lands. */
static void replaced_prompt(void) {
    reset();
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 1410);
    prompt(1415);              /* another phone, mid-debounce */
    run(1410, 1600);
    assert(confirms == 1 && !pending);
}

/* A finger held on the face, reported once the hold passes 500 ms
 * (MUSE_BTN_TALK_HELD): down before the prompt showed, it confirms nothing
 * and never talks; down after, it's one press and confirms. */
static void held_face(void) {
    reset();
    unsigned down = prompt_seq;   /* the finger went down at 1000 ms */
    prompt(1100);
    clock_us = 1500 * 1000; touch(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_HELD, down);
    assert(!confirms && pending && !downs);
    clock_us = 2000 * 1000; touch(MUSE_BTN_TALK_RELEASE, down);
    assert(!ups && pending);
    down = prompt_seq;            /* down again at 2100 ms, the prompt up */
    clock_us = 2600 * 1000; touch(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_HELD, down);
    assert(confirms == 1 && !pending && !downs);
    clock_us = 3000 * 1000; touch(MUSE_BTN_TALK_RELEASE, down);
    assert(!ups);
}

/* A touch's talk press that went down before the prompt showed but reaches
 * the input task after it (its events are drained a poll later): it confirms
 * nothing and never talks; a key pressed once the prompt is up still does. */
static void drained_touch(void) {
    reset();
    unsigned down = prompt_seq;
    prompt(1000);
    clock_us = 1010 * 1000; touch(MUSE_BTN_TALK_PRESS, down);
    assert(!confirms && pending && !downs);
    clock_us = 1200 * 1000; touch(MUSE_BTN_TALK_RELEASE, down);
    assert(!ups && pending);
    run(1200, 1400);
    level = 0; run(1400, 1500);
    assert(confirms == 1 && !pending && !downs);
}

/* Before the prompt's first frame (it isn't armed yet): a key's edge, a touch's down and a confirm key's press
 * confirm nothing and never talk, held on into the prompt and let go there; the next press, once it shows,
 * confirms. */
static void before_the_first_frame(void) {
    reset();
    prompt_up();
    level = 0; run(50, 150);         /* the edge lands before the first frame */
    assert(!confirms && pending && !downs);
    prompt_shows();
    run(150, 300);
    level = 1; run(300, 400);
    assert(!confirms && pending && !ups && !s_talk_down);
    level = 0; run(400, 500);
    assert(confirms == 1 && !pending && !downs);
    level = 1; run(500, 600);
    assert(!ups);

    reset();
    prompt_up();
    unsigned down = prompt_seq;      /* a touch down before the first frame */
    prompt_shows();
    clock_us = 1010 * 1000; touch(MUSE_BTN_TALK_PRESS, down);
    clock_us = 1200 * 1000; touch(MUSE_BTN_TALK_RELEASE, down);
    assert(!confirms && pending && !downs && !ups);
    clock_us = 1300 * 1000; touch(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE, prompt_seq);
    assert(confirms == 1 && !pending && !downs && !ups);

    reset();
    set_up = true;                   /* a re-setup's confirm key (the second key, the Confirm disc) */
    prompt_up();
    clock_us = 1010 * 1000; assert(muse_link_confirm_key_press() && !confirms && pending);
    prompt_shows();
    clock_us = 1300 * 1000; assert(muse_link_confirm_key_press() && confirms == 1 && !dismisses);
}

/* Screen confirmation: touch taps report press and release together
 * (muse_input_touch_talk()), and the prompt's Confirm calls
 * muse_link_confirm_key_press(). One tap confirms, however soon after the
 * prompt, a first setup or a re-setup. */
static void screen_taps(void) {
    reset();
    prompt(1000);
    clock_us = 1010 * 1000; touch(MUSE_BTN_TALK_PRESS | MUSE_BTN_TALK_RELEASE, prompt_seq);
    assert(confirms == 1 && !downs && !ups);

    reset();
    set_up = true;   /* a re-setup: the Confirm tap (a confirm key) confirms it */
    prompt(1000);
    clock_us = 1010 * 1000; assert(muse_link_confirm_key_press() && confirms == 1 && !dismisses);
    assert(!muse_link_confirm_key_press());   /* nothing waits: the press is the board's */
}

/* A stranger raised a re-setup prompt on a set-up gadget: the owner's talk
 * press only dismisses it, never confirms, and is never a turn; the second
 * key (a confirm key) confirms it. */
static void resetup_talk_dismisses(void) {
    reset();
    set_up = true;
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 1500);
    level = 1; run(1500, 1600);
    assert(dismisses == 1 && !confirms && !pending && !downs && !ups);

    reset();
    set_up = true;
    prompt(1000);
    clock_us = 1300 * 1000;
    assert(muse_link_confirm_key_press() && confirms == 1 && !dismisses);
}

/* In setup mode (a board whose only key is its talk key, pressed at power-on)
 * the talk key confirms a re-setup as a first setup is: one short press
 * confirms (and doesn't talk); so does one long one. */
static void resetup_one_press(void) {
    reset();
    set_up = setup_mode = true;
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 1500);
    level = 1; run(1500, 1600);
    assert(confirms == 1 && !dismisses && !pending && !downs && !ups);

    reset();
    set_up = setup_mode = true;
    prompt(1000);
    run(50, 1400);
    level = 0; run(1400, 6000);    /* held 4.6 s: still one press */
    assert(confirms == 1 && !pending && !downs);
    level = 1; run(6000, 6100);
    assert(!ups && !s_talk_down);
}

/* A touch board, first setup: the talk key's press confirms too; the back
 * gesture (muse_ui.c's swipe, Escape) ends the prompt: Not now. */
static void dismiss_on_a_touch_board(void) {
    reset();
    board.touch = true;
    prompt(1000);
    clock_us = 1300 * 1000;
    assert(muse_link_confirm_dismiss() && dismisses == 1 && !pending && !confirms);
    assert(!muse_link_confirm_dismiss());   /* no prompt up: nothing */
    prompt(2000);
    run(50, 1400);
    level = 0; run(1400, 1500);
    assert(confirms == 1 && dismisses == 1);
}

/* The bench (>pair.confirm, the auto-confirm) presses as the key does, in
 * any build, a re-setup included. */
static void bench(void) {
    reset();
    prompt(1000);
    clock_us = 1300 * 1000;
    assert(muse_link_bench_confirm() && confirms == 1);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "before")) pressed_before_the_prompt();
    else if (!strcmp(argv[1], "quick")) quick_release_and_press();
    else if (!strcmp(argv[1], "after")) press_after_the_prompt();
    else if (!strcmp(argv[1], "replaced")) replaced_prompt();
    else if (!strcmp(argv[1], "screen")) screen_taps();
    else if (!strcmp(argv[1], "held")) held_face();
    else if (!strcmp(argv[1], "drained")) drained_touch();
    else if (!strcmp(argv[1], "first_frame")) before_the_first_frame();
    else if (!strcmp(argv[1], "resetup")) resetup_one_press();
    else if (!strcmp(argv[1], "resetup_talk")) resetup_talk_dismisses();
    else if (!strcmp(argv[1], "dismiss")) dismiss_on_a_touch_board();
    else if (!strcmp(argv[1], "bench")) bench();
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
static int shorts, longs, claims;
static bool claim;
static int64_t after_us[2], claim_at_us;
static void on_short(void) { after_us[shorts++] = button_pressed_after_us(); }
static void on_other(void) { assert(0); }
static bool on_claim(void) { claims++; claim_at_us = clock_us; return claim; }
static void on_long(void) { longs++; }
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
    /* A gadget's setup prompt takes the press at its down edge (button_set_confirm): it confirms there, and
     * held on past 5 s it's still no long press (setup reset) and no tap. */
    s_short_press_cb = on_other; s_double_press_cb = on_other; s_long_press_cb = on_long;
    button_set_confirm(on_claim);
    claim = true;
    clock_us = 0; end_us = 8000 * 1000;
    press_from_us[0] = 1010 * 1000; press_to_us[0] = 7010 * 1000;
    press_from_us[1] = press_to_us[1] = 0;
    if (!setjmp(done)) button_task(NULL);
    assert(claims == 1 && longs == 0);
    assert(claim_at_us >= 1010 * 1000 && claim_at_us < 1100 * 1000);   /* at the down edge */
    /* A short one too: one press. */
    clock_us = 0; end_us = 3000 * 1000;
    press_from_us[0] = 1010 * 1000; press_to_us[0] = 1100 * 1000;
    if (!setjmp(done)) button_task(NULL);
    assert(claims == 2 && longs == 0);
    /* Not claimed (no prompt waits): the 5 s hold is the long press, as before. */
    claim = false;
    clock_us = 0; end_us = 8000 * 1000;
    press_from_us[0] = 1010 * 1000; press_to_us[0] = 7010 * 1000;
    if (!setjmp(done)) button_task(NULL);
    assert(claims == 3 && longs == 1);
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
/* muse_local_press.h: the update's confirm press (not under test here: no prompt shows). */
#include <stdbool.h>
static inline bool muse_local_press_prompting(void) { return false; }
static inline bool muse_local_press_note(const char *what) { (void)what; return false; }
/* muse_bootguard.h: the way into SAFE stays closed here (no press counts as one). */
#include <stdbool.h>
static inline bool muse_bootguard_entry_open(void) { return false; }
static inline void muse_bootguard_entry_watch(bool (*key_down)(void)) { (void)key_down; }
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
        subprocess.run([*cls.cc, *flags, "-DRELEASE_BUILD", str(out / "muse.c"), "-o", str(out / "muse_rel")],
                       check=True)
        (out / "inc").mkdir()
        for name in ("driver/gpio.h", "freertos/FreeRTOS.h", "freertos/task.h", "esp_log.h", "esp_timer.h",
                     "muse_bootguard.h", "muse_local_press.h", "sdkconfig.h"):   # sdkconfig: no BLE gadget here
            path = out / "inc" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fakes.h"\n')
        (out / "inc/fakes.h").write_text(BUTTON_FAKES)
        (out / "button_harness.c").write_text(BUTTON_HARNESS)
        subprocess.run([*cls.cc, *flags, "-Wno-unused-parameter", "-I", str(out / "inc"), "-I", str(ROOT / "main"),
                        str(out / "button_harness.c"), "-o", str(out / "button")], check=True)
        cls.out = out

    def run_case(self, case, exe="muse"):
        result = subprocess.run([str(self.out / exe), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_press_begun_just_before_the_prompt_confirms(self):
        self.run_case("before")

    def test_a_note_held_through_the_prompt_then_a_press_confirms(self):
        self.run_case("quick")

    def test_press_after_the_prompt_confirms(self):
        self.run_case("after")

    def test_a_press_landing_after_a_replaced_prompt_confirms_it(self):
        self.run_case("replaced")

    def test_a_face_held_from_before_the_prompt_confirms_nothing_and_never_talks(self):
        self.run_case("held")

    def test_a_touch_down_before_the_prompt_drained_after_it_confirms_nothing(self):
        self.run_case("drained")

    def test_a_press_before_the_prompts_first_frame_confirms_nothing(self):
        self.run_case("first_frame")

    def test_one_screen_tap_confirms(self):
        self.run_case("screen")

    def test_setting_up_again_in_setup_mode_takes_one_press(self):
        self.run_case("resetup")

    def test_a_talk_press_on_a_resetup_prompt_dismisses_and_the_second_key_confirms(self):
        self.run_case("resetup_talk")

    def test_the_back_gesture_dismisses(self):
        self.run_case("dismiss")

    def test_the_bench_confirms_in_a_release_build_too(self):
        self.run_case("bench", "muse_rel")

    def test_link_button_reports_when_its_short_press_began_and_a_prompt_takes_its_press(self):
        result = subprocess.run([str(self.out / "button")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
