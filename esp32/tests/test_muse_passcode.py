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

"""The passcode lock's rules and defaults.

muse_passcode_core.c is pure C, run here through tests/muse_passcode_harness.c
with portable/mg_link_setup's builtin SHA-256: the stored record (salted
SHA-256, never the code), when it locks, the wrong-code waits and their
survival across reboots, the pairing's erase and the console line. The
security wiring (what a lock holds back, the erase in
app.c, nothing logged) is checked in the sources below; the locked talk press
runs in test_muse_ptt_ui's input harness.
"""

from __future__ import annotations

import hashlib
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
MUSE = ROOT / "components" / "muse"
PORTABLE = REPO / "portable" / "mg_link_setup"

MIN = 60 * 1000


def build(tmp):
    exe = Path(tmp) / "pass"
    cc = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MUSE),
                         "-I", str(PORTABLE / "include"), "-I", str(PORTABLE / "src" / "crypto"),
                         str(ROOT / "tests" / "muse_passcode_harness.c"), str(MUSE / "muse_passcode_core.c"),
                         str(PORTABLE / "src" / "crypto" / "mgls_crypto_builtin.c"),
                         str(PORTABLE / "src" / "crypto" / "mgls_uecc.c"), "-lm", "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    return exe


class Harness:
    def __init__(self, exe):
        self.p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def __call__(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def kv(self, line):
        out = self(line)
        d = {}
        for tok in out.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                d[k] = v
            else:
                d.setdefault("_", tok)
        return d

    def close(self):
        self.p.stdin.close()
        self.p.wait()


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.h = Harness(self.exe)

    def tearDown(self):
        self.h.close()


class Record(Base):
    def test_off_by_default_and_any_four_digits(self):
        self.assertEqual(self.h("status 0"), "off")
        self.assertEqual(self.h.kv("fresh 0")["locked"], "0")
        for code in ("0000", "1111", "1234", "9999"):   # any four digits: no "too simple" check
            self.assertEqual(self.h.kv("set %s 0" % code)["set"], "1")
        self.assertEqual(self.h.kv("set 12a4 0")["set"], "0")

    def test_record_is_salted_sha256_never_the_code(self):
        self.h("set 2738 0")
        rec = bytes.fromhex(self.h("rec"))
        fmt, on, lock, tries, level = rec[:5]
        salt, digest = rec[5:21], rec[21:53]
        self.assertEqual((fmt, on, lock, tries, level), (2, 1, 0, 0, 0))
        self.assertEqual((len(rec), rec[53]), (54, 0))   # format 2: its last byte reserved, 0
        self.assertEqual(salt, bytes(range(0x10, 0x20)))
        self.assertEqual(digest, hashlib.sha256(salt + b"mg-passcode-v1" + b"2738").digest())
        self.assertNotIn(b"2738", rec)
        status = self.h("status 0")
        self.assertEqual(status, "on unlocked tries=0 wait=0")   # never the code, salt or hash
        self.assertNotIn("2738", status)

    def test_format_1_records_read_and_stored_as_format_2(self):
        # The earlier record (53 bytes, format 1): kept, stored again as format 2 (its reserved byte 0).
        self.h("set 2738 0")
        rec = bytearray(bytes.fromhex(self.h("rec")))
        rec[0] = 1
        d = self.h.kv("load %s 0" % rec[:53].hex())
        self.assertEqual((d["dirty"], d["locked"]), ("1", "1"))   # a boot locks
        new = bytes.fromhex(self.h("rec"))
        self.assertEqual((new[0], new[53], new[5:53]), (2, 0, bytes(rec[5:53])))
        self.assertEqual(self.h.kv("check 2738 0")["_"], "right")
        rec[0] = 3   # an unknown format reads as no passcode
        self.assertEqual(self.h.kv("load %s 0" % rec.hex())["on"], "0")

    def test_the_reserved_byte_reads_as_0(self):
        # A format 2 record whose last byte isn't 0 loads as it is otherwise, unchanged in store until its next write,
        # which writes 0; Turn off and the pairing's erase write 0 too.
        self.h("set 2738 0")
        rec = bytearray(bytes.fromhex(self.h("rec")))
        rec[53] = 30
        d = self.h.kv("load %s 0" % rec.hex())
        self.assertEqual((d["dirty"], d["on"], d["locked"]), ("0", "1", "1"))
        self.assertEqual(bytes.fromhex(self.h("rec"))[53], 0)
        self.assertEqual(self.h.kv("check 2738 0")["_"], "right")
        self.h("turnoff")
        self.assertEqual(bytes.fromhex(self.h("rec"))[53], 0)
        self.h("forget")
        self.assertEqual(bytes.fromhex(self.h("rec"))[53], 0)

    def test_a_failed_hash_sets_nothing_and_unlocks_nothing(self):
        self.h("shafail 1")
        self.assertEqual(self.h.kv("set 1234 0")["set"], "0")
        self.assertEqual(self.h("status 0"), "off")
        self.h("shafail 0")
        self.h("set 1234 0")
        self.h("locknow")
        self.h("shafail 1")
        d = self.h.kv("check 1234 0")
        self.assertEqual((d["_"], d["locked"], d["tries"]), ("wrong", "1", "0"))   # not right, not counted

    def test_reboot_starts_locked(self):
        self.h("set 1234 0")
        self.assertEqual(self.h.kv("reboot 1000")["locked"], "1")
        self.assertEqual(self.h("status 1000"), "on locked tries=0 wait=0")
        self.assertEqual(self.h.kv("check 1234 1000")["_"], "right")
        self.assertEqual(self.h("status 1000"), "on unlocked tries=0 wait=0")

    def test_turn_off_wipes_the_code_and_keeps_the_lock_choice(self):
        self.h("set 1234 0")
        self.h("setlock 1")
        d = self.h.kv("turnoff")
        self.assertEqual((d["on"], d["locked"], d["lock"]), ("0", "0", "1"))
        rec = bytes.fromhex(self.h("rec"))
        self.assertEqual(rec[5:53], bytes(48))   # salt and hash gone
        self.assertEqual(self.h.kv("reboot 0")["locked"], "0")

    def test_the_pairing_erase_clears_everything(self):
        self.h("set 1234 0")
        self.h("setlock 1")
        for _ in range(6):
            self.h("check 0000 0")
        d = self.h.kv("forget")
        self.assertEqual((d["on"], d["locked"], d["tries"], d["level"], d["lock"], d["left"]),
                         ("0", "0", "0", "0", "0", "0"))
        self.assertEqual(self.h.kv("reboot 0")["on"], "0")
        self.assertEqual(self.h("status 0"), "off")

    def test_lock_now_needs_a_passcode(self):
        self.assertEqual(self.h.kv("locknow")["locked"], "0")
        self.h("set 1234 0")
        self.assertEqual(self.h.kv("locknow")["locked"], "1")


class WhenItLocks(Base):
    def test_screen_off_locks_by_default(self):
        self.h("set 1234 0")
        self.assertEqual(self.h.kv("off 1000 0")["locked"], "1")
        self.assertEqual(self.h.kv("on 2000")["locked"], "1")

    def test_never_during_a_call_or_turn_the_next_screen_off_locks(self):
        self.h("set 1234 0")
        self.assertEqual(self.h.kv("off 1000 1")["locked"], "0")   # busy
        self.assertEqual(self.h.kv("on 2000")["locked"], "0")
        self.assertEqual(self.h.kv("off 3000 0")["locked"], "1")   # the first screen-off after it ends

    def test_after_5_min(self):
        self.h("set 1234 0")
        self.h("setlock 1")
        self.assertEqual(self.h.kv("off 0 0")["locked"], "0")
        self.assertEqual(self.h.kv("on %d" % (5 * MIN - 1))["locked"], "0")   # waking within: no code
        self.h("off %d 0" % (6 * MIN))
        self.assertEqual(self.h.kv("tick %d 0 0 0" % (6 * MIN + 5 * MIN - 1))["locked"], "0")
        self.assertEqual(self.h.kv("tick %d 0 0 0" % (11 * MIN))["locked"], "1")   # dark 5 min
        self.h("check 1234 %d" % (11 * MIN))
        self.h("off %d 0" % (12 * MIN))
        self.assertEqual(self.h.kv("on %d" % (17 * MIN))["locked"], "1")

    def test_after_5_min_a_press_in_the_dark_locks_before_the_tick(self):
        self.h("set 1234 0")
        self.h("setlock 1")
        self.h("off 0 0")
        self.assertEqual(self.h.kv("press %d" % (5 * MIN - 1))["locked"], "0")   # within: no code
        self.assertEqual(self.h.kv("press %d" % (5 * MIN))["locked"], "1")       # dark 5 min, no tick yet
        self.h("check 1234 %d" % (5 * MIN))
        self.assertEqual(self.h.kv("press %d" % (20 * MIN))["locked"], "0")      # applied once: no second lock
        self.h("setlock 0")
        self.assertEqual(self.h.kv("press %d" % (21 * MIN))["locked"], "0")

    def test_a_screen_that_stays_on_locks_after_5_idle_minutes(self):
        self.h("set 1234 0")
        self.assertEqual(self.h.kv("tick 0 %d 1 0" % (5 * MIN - 1))["locked"], "0")
        self.assertEqual(self.h.kv("tick 0 %d 1 1" % (5 * MIN))["locked"], "0")   # in a call
        self.assertEqual(self.h.kv("tick 0 %d 0 0" % (5 * MIN))["locked"], "0")   # the timeout will do it
        self.assertEqual(self.h.kv("tick 0 %d 1 0" % (5 * MIN))["locked"], "1")

    def test_nothing_locks_without_a_passcode(self):
        self.assertEqual(self.h.kv("off 0 0")["locked"], "0")
        self.assertEqual(self.h.kv("tick 0 %d 1 0" % (10 * MIN))["locked"], "0")


class WrongCodes(Base):
    def test_five_free_then_30s_doubling_to_5_min(self):
        self.h("set 1234 0")
        self.h("locknow")
        t = 0
        for i in range(1, 5):
            d = self.h.kv("check 0000 %d" % t)
            self.assertEqual((d["_"], d["tries"]), ("wrong", str(i)))
        waits = []
        for i in range(6):
            d = self.h.kv("check 0000 %d" % t)
            self.assertEqual(d["_"], "wait")
            waits.append(int(d["left"]) // 1000)
            # refused unchecked (and uncounted) while it runs, even the right code
            w = self.h.kv("check 1234 %d" % (t + 1000))
            self.assertEqual((w["_"], w["tries"], w["locked"]), ("waiting", d["tries"], "1"))
            t += int(d["left"])
        self.assertEqual(waits, [30, 60, 120, 240, 300, 300])
        self.assertEqual(self.h("status %d" % (t - 20000)), "on locked tries=10 wait=20")
        d = self.h.kv("check 1234 %d" % t)
        self.assertEqual((d["_"], d["tries"], d["level"], d["locked"]), ("right", "0", "0", "0"))

    def test_the_count_and_wait_survive_a_reboot(self):
        self.h("set 1234 0")
        t = 0
        for _ in range(6):
            d = self.h.kv("check 0000 %d" % t)
            t += int(d["left"])
        self.assertEqual((d["tries"], d["level"]), ("6", "1"))
        d = self.h.kv("reboot 0")   # a flat battery, a crash
        self.assertEqual((d["locked"], d["tries"], d["level"], d["left"]), ("1", "6", "1", "60000"))
        self.assertEqual(self.h.kv("check 1234 59000")["_"], "waiting")
        self.assertEqual(self.h.kv("check 0000 60000")["left"], "120000")

    def test_wrong_codes_never_erase_anything(self):
        self.h("set 1234 0")
        t = 0
        for _ in range(30):
            d = self.h.kv("check 0000 %d" % t)
            t += int(d["left"]) + 1
        self.assertEqual(d["on"], "1")
        self.assertEqual(self.h.kv("check 1234 %d" % t)["_"], "right")


def read(name):
    return (MUSE / name).read_text()


class Wiring(unittest.TestCase):
    def has(self, needle, hay, yes=True):
        self.assertEqual(needle in hay, yes, needle)   # never the whole file in a failure

    def test_ui_streams_and_camera_views_are_held_while_locked(self):
        # A UI stream or a phone camera view is held like a drawn file (not shown, the screen not
        # woken for it) and shows once unlocked if still live; a call's video too (a call opens only unlocked).
        v = read("muse_video.c")
        held = v[v.index("static bool held(void)"):]
        held = held[:held.index("\n}\n")]
        self.has("return s_hold || s_cover;", held)   # locked, or a passcode keypad up
        self.has("muse_video_set_cover(true);", read("muse_passcode_ui.c"))
        self.has("bool drawable = sl->pic && !sl->failed && s_open && !held();", v)   # its pictures dropped
        self.has("|| stalled || held())) {", v)                                    # the screen given back at once
        self.has("bool open = s_open && !s_call_cover && !s_call_stalled && !held();", v)  # the view hidden
        self.has("if (s_open && !held()) {", v)                                     # no awake tick: nothing drawn
        begin = v[v.index("static bool begin_locked(const muse_video_transport_t *t, const muse_video_params_t *vp)\n{"):]
        begin = begin[:begin.index("\n}\n")]
        self.has("if (!held()) {\n            s_wake_req = true;", begin)             # never woken for it
        self.has("s_open = true;", begin)   # the stream stays live behind the lock: once unlocked it shows
        self.has("muse_video_set_hold(locked);", read("muse_passcode_esp.c"))
        core = read("muse_passcode.c")
        self.has("muse_passcode_platform_locked(atomic_load(&s_locked));", core)   # every lock change


    def test_the_pairing_erase_takes_the_passcode(self):
        app = (ROOT / "main" / "app.c").read_text()
        for call in ('passcode_forget("setup reset")', 'passcode_forget("Link setup")', "muse_passcode_forget(why);"):
            self.assertTrue(call in app, call)


    def test_the_locked_face_holds_back(self):
        ui = read("muse_ui.c")
        self.has("muse_passcode_screen_off(passcode_busy());", ui)   # the lock at screen-off
        self.has("muse_passcode_screen_on();", ui)                    # "After 5 min" at the wake
        self.has("&& !muse_passcode_ui_locked_face()) {\n            answer = layout;", ui)   # no reply text
        self.has("if (s_image_dsc.data && !image_held_off()) {", ui)                        # no drawn files
        self.has("!call && !locked;", ui)                             # no swipes to Status / Settings
        home = read("muse_home.c")
        self.has("lv_obj_t *const objs[] = {\n        s_spk,\n#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL\n        s_call,\n"
                 "#endif\n        s_hold,\n    };", home)   # no buttons (Call only with Live call)
        self.has("!(s_locked && s_caption_turn)", home)

    def test_never_logged(self):
        for name in ("muse_passcode.c", "muse_passcode_esp.c", "muse_passcode_ui.c"):
            src = read(name)
            for m in re.finditer(r"(ESP_LOG[EWIDV]|printf)\([^;]*;", src, re.S):
                args = re.sub(r'"(\\.|[^"\\])*"', '""', m.group(0))   # the arguments, not the words
                self.assertNotRegex(args, r"\b(digits|salt|hash|rec\.hash|rec\.salt|s_entry|s_code)\b", name)


# The gadget's platform (muse_passcode_esp.c) and muse_passcode.c on a fake NVS: what a boot finds when the store
# can't be read, and the way back.
STORE_FAKES = {
    "sdkconfig.h": "#pragma once\n#define CONFIG_MUSE_VIDEO 0\n",
    "esp_log.h": "#pragma once\n#include <stdio.h>\n"   # the arguments still evaluated, the line dropped
                 "#define ESP_LOGE(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))\n"
                 "#define ESP_LOGW(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))\n"
                 "#define ESP_LOGI(tag, ...) ((void)(tag), (void)snprintf(NULL, 0, __VA_ARGS__))\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": "#pragma once\n#define portMAX_DELAY 0xffffffffu\n",
    "freertos/semphr.h": "#pragma once\ntypedef int *SemaphoreHandle_t;\nstatic int test_mutex;\n"
                         "static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &test_mutex; }\n"
                         "static inline int xSemaphoreTake(SemaphoreHandle_t m, unsigned t) { (void)m; (void)t; return 1; }\n"
                         "static inline int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return 1; }\n",
    "psa/crypto.h": "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n#define PSA_SUCCESS 0\n#define PSA_ALG_SHA_256 1\n"
                    "static inline int psa_crypto_init(void) { return PSA_SUCCESS; }\n"
                    "int psa_hash_compute(int alg, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *got);\n",
    "secure_random.h": "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n"
                       "static inline bool secure_random(void *b, size_t n) { unsigned char *p = b; "
                       "for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(0x40 + i); return true; }\n",
    "nvs.h": "#pragma once\n#include <stddef.h>\ntypedef int esp_err_t;\ntypedef int nvs_handle_t;\n"
             "typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;\n#define ESP_OK 0\n#define ESP_FAIL (-1)\n"
             "#define ESP_ERR_NVS_NOT_FOUND 0x1102\n#define ESP_ERR_NVS_INVALID_LENGTH 0x110c\n"
             "esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);\nvoid nvs_close(nvs_handle_t h);\n"
             "esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);\n"
             "esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len);\n"
             "esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);\nesp_err_t nvs_commit(nvs_handle_t h);\n"
             "const char *esp_err_to_name(esp_err_t e);\n",
}

STORE_HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_passcode.h"
#include "nvs.h"
#include "esp_timer.h"
#include "psa/crypto.h"
/* The store: one blob, or none; what open and read answer (an error to inject). */
static unsigned char blob[128];
static size_t blob_n;
static bool have_blob;
static esp_err_t open_err, get_err;
static int sets, erases;
static int64_t clock_us;
int64_t esp_timer_get_time(void) { return clock_us; }
int psa_hash_compute(int alg, const uint8_t *in, size_t n, uint8_t *out, size_t cap, size_t *got)
{
    (void)alg;
    uint32_t h = 2166136261u;   /* FNV, spread: a stand-in hash (the core's rules only need one) */
    for (size_t i = 0; i < cap; i++) {
        for (size_t j = 0; j < n; j++) h = (h ^ in[j]) * 16777619u;
        h = (h ^ (uint32_t)i) * 16777619u;
        out[i] = (uint8_t)(h >> 24);
    }
    *got = cap;
    return 0;
}
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)ns;
    *h = 1;
    return mode == NVS_READONLY ? open_err : ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h; (void)key;
    if (get_err) return get_err;
    if (!have_blob) return ESP_ERR_NVS_NOT_FOUND;
    if (blob_n > *len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, blob, blob_n);
    *len = blob_n;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    (void)h; (void)key;
    memcpy(blob, v, len); blob_n = len; have_blob = true; sets++;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; (void)key;
    bool had = have_blob;
    have_blob = false; blob_n = 0; erases++;
    return had ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }

static void boot(void)
{
    muse_passcode_init();
    muse_passcode_set_supported(true);
}

static void line(const char *what)
{
    char st[64];
    muse_passcode_status(st, sizeof(st));
    printf("%s: %s locked=%d sets=%d erases=%d blob=%d\n", what, st, muse_passcode_locked(), sets, erases, have_blob);
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (!strcmp(c, "none")) {             /* no namespace yet, then no key: no passcode */
        open_err = ESP_ERR_NVS_NOT_FOUND; boot(); line("no namespace");
        open_err = ESP_OK; boot(); line("no key");
    } else if (!strcmp(c, "set")) {       /* a passcode set, then a reboot reads it back: locked, its code unlocks */
        boot();
        muse_passcode_set("1234"); line("set");
        boot(); line("reboot");
        printf("check %d\n", (int)muse_passcode_check("1234")); line("right code");
    } else if (!strncmp(c, "err", 3)) {   /* a passcode stored, then the store can't be read */
        boot(); muse_passcode_set("1234");
        if (!strcmp(c, "err-open")) open_err = ESP_FAIL;
        else if (!strcmp(c, "err-read")) get_err = ESP_FAIL;
        else if (!strcmp(c, "err-length")) blob_n = 100;
        else if (!strcmp(c, "err-format")) blob[0] = 9;
        int before = sets;
        boot(); line("boot");
        /* No code unlocks it, the stored one either; wrong codes count to the wait in RAM, nothing written. */
        printf("check %d\n", (int)muse_passcode_check("1234"));
        for (int i = 0; i < 4; i++) muse_passcode_check("0000");
        printf("wait %d\n", muse_passcode_wait_left_ms() > 0);
        printf("written %d\n", sets - before);
        line("tries");
        /* The way back: the pairing's erase (Reset pairing, from Forgot passcode on the wait screen) clears it and
         * erases the stored record; the next boot has no passcode. */
        open_err = get_err = ESP_OK;
        muse_passcode_forget("Reset pairing"); line("reset");
        boot(); line("after reset");
    } else if (!strcmp(c, "clear")) {     /* the bench's `pass clear` clears an unreadable one too */
        boot(); muse_passcode_set("1234");
        get_err = ESP_FAIL; boot(); line("boot");
        get_err = ESP_OK; muse_passcode_clear(); line("clear");
        boot(); line("after clear");
    }
    return 0;
}
"""


class UnreadableStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        for name, text in STORE_FAKES.items():
            (d / "inc" / name).parent.mkdir(parents=True, exist_ok=True)
            (d / "inc" / name).write_text(text)
        (d / "h.c").write_text(STORE_HARNESS)
        cls.exe = d / "store"
        cc = shlex.split(os.environ.get("CC", "cc"))
        r = subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(d / "inc"), "-I", str(MUSE),
                                 str(d / "h.c"), str(MUSE / "muse_passcode_esp.c"), str(MUSE / "muse_passcode.c"),
                                 str(MUSE / "muse_passcode_core.c"), "-lm", "-o", str(cls.exe)],
                           capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stdout + r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, case):
        return subprocess.run([str(self.exe), case], check=True, capture_output=True, text=True).stdout.splitlines()

    def test_no_record_is_no_passcode(self):
        self.assertEqual(self.run_case("none"), [
            "no namespace: off locked=0 sets=0 erases=0 blob=0",
            "no key: off locked=0 sets=0 erases=0 blob=0"])

    def test_a_readable_record_boots_locked_and_its_code_unlocks(self):
        out = self.run_case("set")
        self.assertEqual(out[1], "reboot: on locked tries=0 wait=0 locked=1 sets=1 erases=0 blob=1")
        self.assertEqual(out[2:], ["check 0", "right code: on unlocked tries=0 wait=0 locked=0 sets=1 erases=0 blob=1"])

    def test_a_read_error_boots_locked_and_the_reset_clears_it(self):
        # Only "no record" (ESP_ERR_NVS_NOT_FOUND) is no passcode: an open or read error, or a blob of no known size
        # or format, boots locked, never unlocked. No code is right, not even the stored one; wrong codes reach the
        # wait screen (its Forgot passcode leads to Reset pairing), counted in RAM, nothing written over the record.
        # Reset pairing (the pairing's erase) clears the lock and erases the record: the next boot has no passcode.
        for case in ("err-open", "err-read", "err-length", "err-format"):
            with self.subTest(case=case):
                out = self.run_case(case)
                self.assertTrue(out[0].startswith("boot: on locked ") and " locked=1 " in out[0], out[0])
                self.assertEqual(out[1:4], ["check 1", "wait 1", "written 0"])
                self.assertIn(" locked=1 ", out[4])
                self.assertTrue(out[5].startswith("reset: off ") and " locked=0 " in out[5] and out[5].endswith("blob=0"),
                                out[5])
                self.assertTrue(out[6].startswith("after reset: off ") and " locked=0 " in out[6], out[6])

    def test_pass_clear_clears_an_unreadable_record_too(self):
        out = self.run_case("clear")
        self.assertIn(" locked=1 ", out[0])
        self.assertTrue(out[1].startswith("clear: off ") and out[1].endswith("blob=0"), out[1])
        self.assertTrue(out[2].startswith("after clear: off ") and " locked=0 " in out[2], out[2])
