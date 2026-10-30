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

"""Compile the StopWatch's battery-rest power code against host fakes."""
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
    match = re.search(r"^(?:static )?(?:unsigned|int|void|esp_err_t|bool) " + name + r"\([^)]*\)\n\{", source, re.M)
    start = match.start()
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def defines(source, pattern):
    return "\n".join(re.findall(r"^#define (?:" + pattern + r")\b.*", source, re.M))


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIT(n) (1u << (n))
#define ESP_OK 0
#define ESP_FAIL -1
#define pdMS_TO_TICKS(n) (n)
#define TAG "test"
typedef int esp_err_t;
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { int e_ = (expr); if (e_) return e_; } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))
#define ESP_LOGW(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))
'''


class StopWatchPower(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.board = (MUSE / "boards/board_m5stack_stopwatch.c").read_text()
        cls.flags = defines((MUSE / "muse_board.h").read_text(), r"MUSE_BTN_\w+")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, name, code):
        path = Path(self.tmp.name) / (name + ".c")
        path.write_text(code)
        cc = shlex.split(os.environ.get("CC", "cc"))
        result = subprocess.run(cc + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(path.with_suffix("")), "-lm"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return path.with_suffix("")

    def run_cases(self, binary, cases):
        for case in range(cases):
            with self.subTest(case=case):
                result = subprocess.run([str(binary), str(case)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_power_key_interrupt(self):
        """PM1 IRQ setup, clicks to taps, credits for seen presses, stuck line and paused waits."""
        code = COMMON + self.flags + "\n" + defines(self.board, r"PMIC_\w+|POWER_\w+|CLICK_\w+|REST_\w+|IRQ_\w+") + r'''
#define GPIO_NUM_12 12
static uint8_t regs[256];
static int fail_reg = -1, stuck, writes_to_status;
static uint8_t key;
static int64_t now = 10000000;
static void *s_pmic;
static int line(void) { return !(stuck || (regs[0x42] & ~regs[0x45] & 7)); }
static int gpio_get_level(int gpio) { assert(gpio == 12); return line(); }
static int reg_read(void *dev, uint8_t reg, uint8_t *out, size_t n) {
    (void)dev; assert(n == 1);
    if (reg == fail_reg) return -7;
    *out = reg == 0x48 ? key : regs[reg]; return ESP_OK;
}
static int reg_write(void *dev, uint8_t reg, const uint8_t *data, size_t n) {
    (void)dev; assert(n == 1);
    if (reg == fail_reg) return -7;
    if (reg >= 0x40 && reg <= 0x42) { assert(data[0] == 0); ++writes_to_status; }
    regs[reg] = data[0]; return ESP_OK;
}
static int64_t esp_timer_get_time(void) { return now; }
static int delayed, waited_n, waited_ms;
static void vTaskDelay(int ms) { delayed = ms; }
typedef struct { int gpio; } muse_gpio_button_t;
static muse_gpio_button_t s_talk = { 1 }, s_speaker = { 2 }, s_irq_line = { 12 };
static void muse_gpio_buttons_wait(muse_gpio_button_t *const *b, int n, int ms) {
    assert(b[0] == &s_talk && b[1] == &s_speaker && (n == 2 || b[2] == &s_irq_line));
    waited_n = n; waited_ms = ms;
}
static bool s_power_pressed, s_power_candidate, s_power_armed = true, s_power_long;
static int64_t s_power_changed_at, s_power_held_at;
static unsigned s_click_credits, s_click_pending;
static int64_t s_click_credit_at;
static bool s_pmic_irq = true, s_irq_stuck;
''' + "\n".join(function(self.board, n) for n in (
            "pmic_update", "pmic_irq_clear", "pmic_irq_setup", "take_power_irq", "poll_power_button", "wait_buttons")) + r'''
static unsigned sample(uint8_t k, int us) { key = k; now += us; return poll_power_button(); }
static void reset_wait(void) { delayed = waited_n = waited_ms = -1; }
/* A press and release the poll sees, 50 ms polls as at rest. */
static void seen_tap(int hold_us) {
    assert(sample(1, 10000) == 0); assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
    for (int t = 30000; t < hold_us; t += 50000) assert((sample(1, 50000) & ~MUSE_BTN_POWER_LONG) == 0);
    assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
}
#define TAP (MUSE_BTN_POWER_PRESS | MUSE_BTN_POWER_RELEASE)
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    if (test == 0) {   /* setup: only the clicks interrupt, 125 ms click delay, GPIO1 as IRQ, statuses cleared */
        regs[0x49] = 0x1F; regs[0x42] = 0x03; regs[0x40] = 0x04; regs[0x16] = 0x00; regs[0x13] = 0x00;
        assert(pmic_irq_setup() == ESP_OK);
        assert(regs[0x43] == 0x1F && regs[0x44] == 0x3F && regs[0x45] == 0x02);
        assert(regs[0x49] == 0x39);              /* click 125 ms, double gap 250 ms; reset-disable and long delay kept */
        assert(CLICK_CREDIT_US == 500000);       /* 125 + 250 ms, plus 125 ms margin */
        assert(regs[0x13] == 0x02 && regs[0x16] == 0x04);
        assert(regs[0x40] == 0 && regs[0x41] == 0 && regs[0x42] == 0 && line());
        fail_reg = 0x45; assert(pmic_irq_setup() == -7);
    } else if (test == 1) {   /* a tap at rest arrives as a click once released: press + release */
        regs[0x42] = 0x01; assert(!line());
        assert(sample(0, 10000) == (MUSE_BTN_POWER_PRESS | MUSE_BTN_POWER_RELEASE));
        assert(line() && regs[0x42] == 0 && writes_to_status == 3 && !s_irq_stuck);
        assert(sample(0, 10000) == 0);           /* once */
    } else if (test == 2) {   /* the poll saw the press: its click adds nothing */
        assert(sample(1, 10000) == 0); assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
        regs[0x42] = 0x01; assert(sample(0, 125000) == 0 && line());
        regs[0x42] = 0x01; assert(sample(0, 1500000) == TAP);
        assert(s_click_credits == 0 && s_click_pending == 0);
    } else if (test == 3) {   /* key down again, not armed, or wake-only events: no fabricated tap */
        regs[0x42] = 0x01; assert(sample(1, 10000) == 0);
        assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
        now += 2000000; s_power_armed = false;
        regs[0x42] = 0x01; assert(sample(0, 10000) == 0 && line());
        s_power_armed = true;
        regs[0x42] = 0x02; regs[0x45] = 0x00; assert(sample(0, 10000) == 0 && line()); /* WAKEUP only */
    } else if (test == 4) {   /* a failed key read keeps the click for the next poll */
        regs[0x42] = 0x01; fail_reg = 0x48;
        assert(sample(0, 10000) == 0 && line());
        fail_reg = -1; assert(sample(0, 10000) == (MUSE_BTN_POWER_PRESS | MUSE_BTN_POWER_RELEASE));
    } else if (test == 5) {   /* paused waits */
        reset_wait(); wait_buttons(1000); assert(waited_n == 3 && waited_ms == 1000 && delayed == -1);
        regs[0x42] = 0x01; reset_wait(); wait_buttons(1000);
        assert(waited_n == -1 && delayed == 10);   /* fell since the poll: take it first */
        regs[0x42] = 0;
        s_power_candidate = true; reset_wait(); wait_buttons(1000);
        assert(waited_n == -1 && delayed == 50);   /* settling: polled as before */
        s_power_candidate = false; s_power_pressed = true; reset_wait(); wait_buttons(1000);
        assert(waited_n == -1 && delayed == 50);   /* held: polled for the 1.5 s hold */
        s_power_pressed = false; s_pmic_irq = false; reset_wait(); wait_buttons(1000);
        assert(waited_n == 2 && waited_ms == 200); /* no interrupt: capped */
        reset_wait(); wait_buttons(150); assert(waited_n == 2 && waited_ms == 150);
    } else if (test == 6) {   /* tap to wake (seen, then its late click), a second tap 0.3 s later only as a click */
        seen_tap(30000);
        regs[0x42] = 0x01; assert(sample(0, 125000) == 0);   /* the wake tap's own click: spent */
        regs[0x42] = 0x01; assert(sample(0, 300000) == TAP); /* well within 1.5 s: still a press */
        assert(sample(0, 10000) == 0);
    } else if (test == 7) {   /* a credit no click spends (a long press) runs out: a later tap isn't swallowed */
        seen_tap(1600000);
        now += CLICK_CREDIT_US;
        regs[0x42] = 0x01; assert(sample(0, 0) == TAP && s_click_credits == 0);
        seen_tap(30000);   /* just inside the window, its click is still that press's */
        now += CLICK_CREDIT_US - 10001;
        regs[0x42] = 0x01; assert(sample(0, 10000) == 0);
    } else if (test == 8) {   /* a double click is two presses: one a poll, quick waits until both are out */
        regs[0x42] = 0x04; assert(sample(0, 10000) == TAP && line() && s_click_pending == 1);
        reset_wait(); wait_buttons(1000); assert(waited_n == -1 && delayed == 10);
        assert(sample(0, 10000) == TAP);
        assert(sample(0, 10000) == 0);
        reset_wait(); wait_buttons(1000); assert(waited_n == 3);
        seen_tap(30000);   /* one press seen, then a double: its credit covers one */
        regs[0x42] = 0x04; assert(sample(0, 125000) == TAP);
        assert(sample(0, 10000) == 0);
    } else if (test == 9) {   /* a missed tap's click while the next press is down: counted once that press is up */
        regs[0x42] = 0x01; assert(sample(1, 10000) == 0);
        assert(sample(1, 30000) == MUSE_BTN_POWER_PRESS);
        assert(sample(0, 10000) == 0); assert(sample(0, 30000) == MUSE_BTN_POWER_RELEASE);
        regs[0x42] = 0x01; assert(sample(0, 125000) == TAP);   /* the second press's own click */
        assert(sample(0, 10000) == 0);
    } else if (test == 10) {   /* napping waits are capped at 1 s: a held red key is found that soon */
        reset_wait(); wait_buttons(10000); assert(waited_n == 3 && waited_ms == 1000);
        reset_wait(); wait_buttons(1000); assert(waited_n == 3 && waited_ms == 1000);
        reset_wait(); wait_buttons(400); assert(waited_n == 3 && waited_ms == 400);
        s_pmic_irq = false; reset_wait(); wait_buttons(10000); assert(waited_n == 2 && waited_ms == 200);
    } else {   /* a line that won't clear is distrusted until it reads high again */
        stuck = 1; assert(sample(0, 10000) == 0 && s_irq_stuck);
        reset_wait(); wait_buttons(1000); assert(waited_n == 2 && waited_ms == 200);
        stuck = 0; assert(sample(0, 10000) == 0 && !s_irq_stuck);
        reset_wait(); wait_buttons(1000); assert(waited_n == 3 && waited_ms == 1000);
    }
    return 0;
}
'''
        self.run_cases(self.build("irq", code), 12)

    def test_oled_rail(self):
        """The OLED's rail goes off while paused and its state comes back with it."""
        code = COMMON + defines(self.board, r"LCD_RES|LCD_CHUNK_BYTES|LCD_LEVEL_BOOT|LCD_POWER_UP_MS|LCD_RESET_MS|IOE_\w+|TP_\w+") + r'''
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
#define heap_caps_calloc(n, size, caps) calloc(n, size)
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
static esp_lcd_panel_io_handle_t s_io = (void *)1;
static esp_lcd_panel_handle_t s_panel = (void *)2;
static void *s_tp = (void *)3;
static char ev[4096];
static uint8_t ioe_out;   /* IO1-8 */
static void note(const char *fmt, int v) { char b[32]; snprintf(b, sizeof(b), fmt, v); strcat(ev, b); strcat(ev, " "); }
static int esp_lcd_panel_io_tx_param(void *io, int cmd, const void *param, size_t n) {
    assert(io == s_io && (cmd >> 24) == 2);
    assert(ioe_out & IOE_L3B);   /* never to an unpowered panel */
    if (n == 1) { char b[32]; snprintf(b, sizeof(b), "c%02x=%d ", (cmd >> 8) & 0xff, *(const uint8_t *)param); strcat(ev, b); }
    else note("c%02x", (cmd >> 8) & 0xff);
    return ESP_OK;
}
static int draws;
static int esp_lcd_panel_reset(void *p) { assert(p == s_panel && (ioe_out & IOE_L3B)); note("swreset", 0); return 0; }
static int esp_lcd_panel_init(void *p) { assert(p == s_panel); note("init", 0); return 0; }
static int esp_lcd_panel_draw_bitmap(void *p, int x1, int y1, int x2, int y2, const void *d) {
    assert(p == s_panel && x1 == 0 && x2 == LCD_RES && y1 % 2 == 0 && y2 % 2 == 0 && y2 > y1 && d);
    for (int i = 0; i < LCD_CHUNK_BYTES; i++) assert(((const uint8_t *)d)[i] == 0);
    if (!draws++) note("black", 0);
    return 0;
}
static int esp_lcd_panel_disp_on_off(void *p, bool on) { assert(p == s_panel && on); note("on", 0); return 0; }
static void muse_lcd_bands_run(void (*fn)(void *), void *arg) { fn(arg); }
static void lcd_bus(void *spi) { note(*(bool *)spi ? "bus" : "lines-low", 0); }
static int ioe_set(uint16_t pins, bool high) {
    note(pins == IOE_L3B ? "l3b=%d" : pins == IOE_LCD_RST ? "rst=%d" : "ioe?=%d", high);
    ioe_out = high ? ioe_out | pins : ioe_out & ~pins; return ESP_OK;
}
static void vTaskDelay(int ms) { note("wait%d", ms); }
static void tp_reset(void) { note("tp-reset", 0); }
static int reg_write(void *dev, uint8_t reg, const uint8_t *v, size_t n) { assert(dev == s_tp && reg == TP_REG_SLEEP && n == 1 && v[0] == 3); note("tp-sleep", 0); return 0; }
static void esp_lv_adapter_pause(int ms) { assert(ms == -1); note("lv-pause", 0); }
static void esp_lv_adapter_resume(void) { note("lv-resume", 0); }
static int esp_lv_adapter_lock(int ms) { (void)ms; return ESP_OK; }
static void esp_lv_adapter_unlock(void) {}
static void *lv_screen_active(void) { return (void *)4; }
static void lv_obj_invalidate(void *o) { assert(o == (void *)4); note("redraw", 0); }
static bool s_lcd_off;
static uint8_t s_lcd_level = LCD_LEVEL_BOOT;
static bool s_lcd_asleep;
''' + "\n".join(function(self.board, n) for n in (
            "lcd_cmd", "panel_setup", "send_brightness", "set_brightness", "send_sleep", "panel_sleep",
            "panel_off", "lcd_power", "display_pause")) + r'''
static void expect(const char *want) {
    if (strcmp(ev, want)) { printf("got:  %s\nwant: %s\n", ev, want); exit(1); }
    ev[0] = 0;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    ioe_out = IOE_L3B | IOE_LCD_RST;
    if (test == 0) {   /* boot: black before display on, M5GFX's brightness */
        panel_setup(NULL);
        expect("swreset init black c51=160 on ");
        assert(draws == (LCD_RES + 7) / 8 && !s_lcd_off);
    } else if (test == 1) {   /* the usual rest: the UI dims and sleeps the panel, then pauses */
        set_brightness(80); panel_sleep(false);
        expect("c51=204 c11 wait120 ");
        set_brightness(0); panel_sleep(true);
        expect("c51=0 c10 wait120 ");
        display_pause(true);
        expect("lv-pause tp-sleep lines-low rst=0 l3b=0 ");
        assert(s_lcd_off && !(ioe_out & (IOE_L3B | IOE_LCD_RST)));
        set_brightness(60);          /* recorded, not sent: the rail is off */
        panel_sleep(false);
        expect("wait120 ");
        set_brightness(0); panel_sleep(true); ev[0] = 0;
        display_pause(false);        /* back as the UI left it: dark and asleep */
        expect("l3b=1 wait20 bus rst=1 wait120 swreset init black c51=0 on c10 wait120 "
               "tp-reset lv-resume redraw ");
        assert(!s_lcd_off);
        panel_sleep(false); set_brightness(80);   /* then the UI wakes it */
        expect("c11 wait120 c51=204 ");
    } else {   /* paused without the UI's sleep (never expected): sleep it before the rail goes */
        set_brightness(50); ev[0] = 0;
        display_pause(true);
        expect("lv-pause tp-sleep c10 wait120 lines-low rst=0 l3b=0 ");
        display_pause(false);
        expect("l3b=1 wait20 bus rst=1 wait120 swreset init black c51=127 on tp-reset lv-resume redraw ");
    }
    return 0;
}
'''
        self.run_cases(self.build("oled", code), 3)

    def test_codec_supply_in_muse_audio(self):
        """muse_audio powers a board-switched codec around open and close, never touches it off, and
        takes the supply down again when bringing the codecs up fails, at a wake or at init."""
        audio = (MUSE / "muse_audio.c").read_text()
        code = COMMON + r'''
#define ESP_ERR_NO_MEM 0x101
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 0xffffffffu
#define ESP_RETURN_ON_FALSE(c, err, tag, ...) do { if (!(c)) return err; } while (0)
typedef enum { ESP_LOG_NONE } esp_log_level_t;
static esp_log_level_t esp_log_level_get(const char *t) { (void)t; return ESP_LOG_NONE; }
static void esp_log_level_set(const char *t, esp_log_level_t l) { (void)t; (void)l; }
#define ESP_LOGE(...) ((void)0)
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
typedef int *esp_codec_dev_handle_t;
static int spk_dev, mic_dev;
static char ev[2048];
static int locked, lock_ops, mic_fail, spk_fail, init_err, init_null;
static void note(const char *e) { strcat(ev, e); strcat(ev, " "); }
static const char *who(esp_codec_dev_handle_t h) { return h == &spk_dev ? "spk" : "mic"; }
typedef void *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static int xSemaphoreTake(SemaphoreHandle_t s, unsigned t) { (void)s; (void)t; assert(!locked); locked = 1; ++lock_ops; return 1; }
static int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; assert(locked); locked = 0; return 1; }
static int esp_codec_dev_open(esp_codec_dev_handle_t h, esp_codec_dev_sample_info_t *fs) {
    (void)fs; char b[16]; snprintf(b, sizeof(b), "open-%s", who(h)); note(b);
    return (h == &mic_dev && mic_fail) || (h == &spk_dev && spk_fail) ? -1 : ESP_CODEC_DEV_OK;
}
static int esp_codec_dev_close(esp_codec_dev_handle_t h) { char b[16]; snprintf(b, sizeof(b), "close-%s", who(h)); note(b); return 0; }
static int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int v) { assert(h == &spk_dev); char b[16]; snprintf(b, sizeof(b), "vol%d", v); note(b); return 0; }
static int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float db) { assert(h == &mic_dev); char b[16]; snprintf(b, sizeof(b), "gain%d", (int)db); note(b); return 0; }
static int muse_settings_volume(void) { return 70; }
static int muse_settings_mic_gain(void) { return 24; }
static void board_codec_power(bool on) { assert(locked || !lock_ops); note(on ? "rail-on" : "rail-off"); }
static void board_audio_power(bool on) { note(on ? "amp-on" : "amp-off"); }
static int board_audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic) {
    note("audio-init"); *spk = &spk_dev; *mic = init_null ? NULL : &mic_dev; return init_err;
}
static int pdm_init(void) { note("pdm-init"); return ESP_OK; }
static int pdm_power(bool on) { note(on ? "pdm-on" : "pdm-off"); return ESP_OK; }
static int pdm_read(int16_t *mono, size_t frames) { (void)mono; (void)frames; return ESP_OK; }
static struct {
    int (*audio_init)(esp_codec_dev_handle_t *, esp_codec_dev_handle_t *);
    void (*set_mic_gain)(esp_codec_dev_handle_t, int);
    void (*audio_power)(bool);
    void (*codec_power)(bool);
    int (*mic_init)(void);
    int (*mic_power)(bool);
    int (*mic_read)(int16_t *, size_t);
} board = { .audio_init = board_audio_init, .audio_power = board_audio_power,
            .codec_power = board_codec_power }, *muse_board = &board;
#include <math.h>
#define CHANNELS 2
#define HPF_HZ 80.0f
#define MUSE_AUDIO_RATE 16000
static float s_hpf_a;
static float s_mic_scale;
static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_open;
static SemaphoreHandle_t s_lock;
''' + "\n".join(function(audio, n) for n in (
            "lock", "unlock", "codec_power", "codecs_reachable", "apply_volume", "apply_mic_gain", "open_codecs",
            "muse_audio_set_volume", "muse_audio_set_mic_gain", "muse_audio_init", "muse_audio_power")) + r'''
static void expect(const char *want) {
    if (strcmp(ev, want)) { printf("got:  %s\nwant: %s\n", ev, want); exit(1); }
    ev[0] = 0;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    s_spk = &spk_dev; s_mic = &mic_dev; s_open = true; s_lock = xSemaphoreCreateMutex();
    if (test == 0) {   /* rest and wake: supply around close and open, settings reapplied */
        muse_audio_power(false);
        expect("amp-off close-spk close-mic rail-off ");
        muse_audio_set_volume(40); muse_audio_set_mic_gain(30);
        expect("");                                  /* unpowered: nothing reaches the codec */
        muse_audio_power(false); expect("");
        muse_audio_power(true);
        expect("rail-on open-spk open-mic vol70 gain24 amp-on ");
        muse_audio_set_volume(40); muse_audio_set_mic_gain(30);
        expect("vol40 gain30 ");
        assert(!locked);
    } else if (test == 1) {   /* a failed open leaves it closed: setters still skip, the next wake retries */
        muse_audio_power(false); ev[0] = 0; mic_fail = 1;
        muse_audio_power(true);
        expect("rail-on open-spk open-mic close-spk rail-off ");   /* supply off again, amp never on */
        assert(!s_open && !locked);
        muse_audio_set_volume(40); expect("");
        mic_fail = 0; spk_fail = 1; muse_audio_power(true);
        expect("rail-on open-spk rail-off ");
        assert(!s_open);
        spk_fail = 0; muse_audio_power(true);
        expect("rail-on open-spk open-mic vol70 gain24 amp-on ");
        assert(s_open);
    } else if (test == 3) {   /* init: rail first, codecs opened and set */
        s_spk = s_mic = NULL; s_open = false; s_lock = NULL; lock_ops = 0;
        assert(muse_audio_init(50, 20) == ESP_OK);
        expect("rail-on audio-init open-spk open-mic vol50 gain20 ");
        assert(s_open && !locked);
    } else if (test == 4) {   /* init failures after the rail went on: each takes it down again */
        int cases[][4] = { { -5, 0, 0, -5 }, { 0, 1, 0, ESP_FAIL }, { 0, 0, 1, ESP_FAIL } };
        const char *want[] = { "rail-on audio-init rail-off ", "rail-on audio-init rail-off ",
                               "rail-on audio-init open-spk open-mic close-spk rail-off " };
        for (int i = 0; i < 3; i++) {
            s_spk = s_mic = NULL; s_open = false; s_lock = NULL; lock_ops = 0;
            init_err = cases[i][0]; init_null = cases[i][1]; mic_fail = cases[i][2];
            assert(muse_audio_init(50, 20) == cases[i][3]);
            expect(want[i]);
            assert(!s_open && !locked);
        }
    } else if (test == 5) {
        board.mic_init = pdm_init; board.mic_power = pdm_power; board.mic_read = pdm_read;
        s_spk = s_mic = NULL; s_open = false; s_lock = NULL;
        assert(muse_audio_init(50, 20) == ESP_OK);
        expect("pdm-init pdm-on ");
        assert(s_open && s_mic_scale > 9.9f && s_mic_scale < 10.1f);
        muse_audio_power(false); expect("pdm-off ");
        muse_audio_power(true); expect("pdm-on ");
    } else {   /* other boards (no codec_power): unchanged, setters reach a closed codec as before */
        board.codec_power = NULL;
        muse_audio_power(false); expect("amp-off close-spk close-mic ");
        muse_audio_set_volume(40); muse_audio_set_mic_gain(30); expect("vol40 gain30 ");
        muse_audio_power(true); expect("open-spk open-mic vol70 gain24 amp-on ");
    }
    return 0;
}
'''
        self.run_cases(self.build("audio", code), 6)
        init = function(audio, "muse_audio_init")
        self.assertLess(init.index("codec_power(true)"), init.index("muse_board->audio_init("))

    def test_stopwatch_codec_supply(self):
        """The StopWatch parks I2S, cuts AU_L3B, and sets the ES8311 up and re-arms it after power-up."""
        code = COMMON + defines(self.board, r"I2S_\w+|CODEC_\w+|ES8311_REG_SYSTEM|IOE_\w+") + r'''
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_21 21
#define ESP_CODEC_DEV_OK 0
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2, GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
typedef enum { GPIO_PULLUP_ONLY, GPIO_PULLDOWN_ONLY, GPIO_PULLUP_PULLDOWN, GPIO_FLOATING } gpio_pull_mode_t;
typedef struct { uint32_t sig_out; bool pu, pd, ie, oe; } gpio_io_config_t;
typedef struct { int cfg; } es8311_codec_cfg_t;
typedef struct audio_codec_if_t audio_codec_if_t;
struct audio_codec_if_t {
    int (*open)(const audio_codec_if_t *h, void *cfg, int size);
    int (*set_reg)(const audio_codec_if_t *h, int reg, int value);
};
static char ev[4096];
static void note(const char *fmt, int v) { char b[40]; snprintf(b, sizeof(b), fmt, v); strcat(ev, b); strcat(ev, " "); }
static int64_t now = 5000000;
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ms) { note("wait%d", ms); now += ms * 1000; }
static bool rail = true;
static int ioe_set(uint16_t pins, bool high) { assert(pins == IOE_AUDIO); rail = high; note("rail=%d", high); return ESP_OK; }
static struct { int level, mode, pull; uint32_t sig; } pin[48];
static int gpio_get_io_config(gpio_num_t p, gpio_io_config_t *io) {
    *io = (gpio_io_config_t){ .sig_out = pin[p].sig, .ie = pin[p].mode & 1, .oe = pin[p].mode & 2 }; return ESP_OK;
}
static int gpio_set_level(gpio_num_t p, int l) { pin[p].level = l; return ESP_OK; }
static int gpio_set_direction(gpio_num_t p, gpio_mode_t m) { pin[p].mode = m; if (m & GPIO_MODE_OUTPUT) pin[p].sig = 256; return ESP_OK; }
static int gpio_set_pull_mode(gpio_num_t p, gpio_pull_mode_t m) { pin[p].pull = m; return ESP_OK; }
static void esp_rom_gpio_connect_out_signal(gpio_num_t p, uint32_t sig, bool inv, bool oen_inv) {
    assert(!inv && !oen_inv && sig != 256); pin[p].sig = sig;
}
static es8311_codec_cfg_t s_es_cfg = { 42 };
static int opens, arms, arm_fail;
static int fake_open(const audio_codec_if_t *h, void *cfg, int size) {
    (void)h; assert(rail && cfg == &s_es_cfg && size == sizeof(s_es_cfg)); ++opens; note("es8311-setup", 0); return 0;
}
static int fake_set_reg(const audio_codec_if_t *h, int reg, int value) {
    (void)h; assert(rail && reg == 0x0D && value == 0x01); ++arms; note("arm", 0); return arm_fail ? -1 : 0;
}
static const audio_codec_if_t codec = { fake_open, fake_set_reg };
static const audio_codec_if_t *s_codec;
static bool s_codec_off, s_codec_arm;
static int64_t s_codec_off_at;
''' + re.search(r"^static const gpio_num_t s_i2s_pins.*?\n.*?\n", self.board, re.M | re.S).group(0) + "\n".join(function(self.board, n) for n in (
            "i2s_park", "codec_power", "audio_power")) + r'''
static void expect(const char *want) {
    if (strcmp(ev, want)) { printf("got:  %s\nwant: %s\n", ev, want); exit(1); }
    ev[0] = 0;
}
static const int sigs[] = { 23, 24, 25, 26 };   /* MCLK, BCLK, WS, DOUT as the I2S driver routed them */
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    const gpio_num_t outs[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT };
    for (int i = 0; i < 4; i++) { pin[outs[i]].mode = GPIO_MODE_OUTPUT; pin[outs[i]].sig = sigs[i]; pin[outs[i]].level = 1; }
    pin[I2S_DIN].mode = GPIO_MODE_INPUT; pin[I2S_DIN].pull = GPIO_FLOATING;
    s_codec = &codec;
    if (test == 0) {   /* boot: already on, nothing to do; first audio_power arms nothing */
        codec_power(true); audio_power(true); expect("");
        assert(opens == 0 && arms == 0);
    } else if (test == 1) {   /* rest, then wake soon after: min off time, setup, routing back, armed once */
        codec_power(false);
        expect("rail=0 ");
        for (int i = 0; i < 4; i++) assert(pin[outs[i]].level == 0 && pin[outs[i]].mode == GPIO_MODE_OUTPUT && pin[outs[i]].sig == 256 && pin[outs[i]].pull == GPIO_FLOATING);
        assert(pin[I2S_DIN].mode == GPIO_MODE_INPUT && pin[I2S_DIN].pull == GPIO_FLOATING);
        codec_power(false); expect("");
        now += 50000;
        codec_power(true);
        expect("wait151 rail=1 wait10 es8311-setup ");
        for (int i = 0; i < 4; i++) assert(pin[outs[i]].sig == (uint32_t)sigs[i] && pin[outs[i]].mode == GPIO_MODE_OUTPUT);
        codec_power(true); expect("");
        audio_power(false); expect("");
        audio_power(true); expect("wait30 arm wait30 arm wait30 arm ");
        audio_power(true); expect("");
    } else {   /* a long rest: no extra wait; a failed arming is tried again next open */
        codec_power(false); now += 60000000; ev[0] = 0;
        codec_power(true); expect("rail=1 wait10 es8311-setup ");
        arm_fail = 1; audio_power(true); assert(arms == 3 && s_codec_arm);
        arm_fail = 0; ev[0] = 0; audio_power(true); expect("wait30 arm wait30 arm wait30 arm ");
        assert(!s_codec_arm);
    }
    return 0;
}
'''
        self.run_cases(self.build("codec", code), 3)
        self.assertIn(".codec_power = codec_power", self.board)
        self.assertIn(".audio_power = audio_power", self.board)
        self.assertIn("s_codec = codec;", self.board)

    def test_imu_soft_reset(self):
        """A BMI270 that answers is soft-reset once; anything else is left alone."""
        code = COMMON + defines(self.board, r"IMU_\w+") + r'''
typedef void *i2c_master_bus_handle_t;
typedef int *i2c_master_dev_handle_t;
static i2c_master_bus_handle_t s_i2c = (void *)1;
static int present = 1, chip_id = 0x24, resets, added, removed;
static int dev;
static int i2c_master_probe(i2c_master_bus_handle_t bus, int addr, int ms) { (void)ms; assert(bus == s_i2c && addr == 0x68); return present ? ESP_OK : -1; }
static int add_device(uint8_t addr, uint32_t hz, i2c_master_dev_handle_t *d) { assert(addr == 0x68 && hz); *d = &dev; ++added; return ESP_OK; }
static int i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { assert(d == &dev); ++removed; return ESP_OK; }
static int reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *out, size_t n) { assert(d == &dev && reg == 0x00 && n == 1); *out = chip_id; return ESP_OK; }
static int reg_write(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *v, size_t n) { assert(d == &dev && reg == 0x7E && n == 1 && v[0] == 0xB6); ++resets; return ESP_OK; }
''' + function(self.board, "imu_rest") + r'''
int main(int argc, char **argv) {
    assert(argc == 2);
    int test = atoi(argv[1]);
    if (test == 0) { imu_rest(); assert(resets == 1 && added == 1 && removed == 1); }
    else if (test == 1) { chip_id = 0x26; imu_rest(); assert(resets == 0 && removed == 1); }   /* another chip at 0x68 */
    else { present = 0; imu_rest(); assert(resets == 0 && added == 0 && removed == 0); }
    return 0;
}
'''
        self.run_cases(self.build("imu", code), 3)
        self.assertIn("imu_rest();", function(self.board, "init"))

    def test_rest_wiring(self):
        init = function(self.board, "init")
        self.assertIn("muse_gpio_button_init(&s_irq_line, PMIC_IRQ_GPIO)", init)
        self.assertIn("pmic_irq_setup()", init)
        self.assertRegex(self.board, r"#define PMIC_IRQ_GPIO GPIO_NUM_12\b")
        self.assertIn(".wait_buttons = wait_buttons", self.board)
        start = self.board.index("static lv_display_t *display_start(")
        self.assertIn("panel_setup(NULL)", self.board[start:self.board.index("esp_lv_adapter_init", start)])
        self.assertNotIn("0x29", self.board[self.board.index("s_lcd_init[]"):self.board.index("static void lcd_cmd")])


if __name__ == "__main__":
    unittest.main()
