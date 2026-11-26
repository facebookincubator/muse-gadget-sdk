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

"""display.show_image and the Ask Muse for an image test
(CONFIG_MUSE_SESSION_IMAGE, main/session_image.h): a use case taught to Muse
through the device session alone. The core (parameters, the request to Muse,
the run's state, its lines) on the host; link.register as each build sends
it (BLE-only: display.show_image, no display.draw_url; Wi-Fi: draw_url, no
show_image); and the Nordic UART allowlist."""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
FAKES = ROOT / "tests/link_fakes"


def function(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    raise AssertionError(signature)


def compile_and_run(test: unittest.TestCase, sources: list[Path], defines: list[str], args: list[str] = ()) -> str:
    with tempfile.TemporaryDirectory(prefix="simg-") as name:
        exe = Path(name) / "a.out"
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmd = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
               "-I", str(FAKES), "-I", str(MAIN), *[f"-D{d}" for d in defines], *map(str, sources), "-o", str(exe)]
        r = subprocess.run(cmd, text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe), *args], text=True, capture_output=True)
        test.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout


CORE_HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "session_image_core.h"

static void parse(const char *json) {
    cJSON *p = json ? cJSON_ParseWithLength(json, strlen(json)) : NULL;
    char path[SIMG_PATH_MAX + 1];
    int ms = -1;
    const char *code = "", *message = "";
    bool ok = simg_parse(p, path, sizeof(path), &ms, &code, &message);
    if (ok) printf("parse ok %s %d\n", path, ms);
    else printf("parse %s: %s\n", code, message);
    cJSON_Delete(p);
}

static void status(const simg_run_t *r, long long now_ms) {
    char line[160];
    simg_status_text(r, now_ms * 1000, line, sizeof(line));
    printf("status %s\n", line);
}

int main(void) {
    parse("{\"path\":\"workspace/random.jpg\"}");
    parse("{\"path\":\"workspace/random.jpg\",\"duration_ms\":8000}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":10}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":999999}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":null}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":0}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":-5}");
    parse("{\"path\":\"a.jpg\",\"duration_ms\":\"5000\"}");
    parse("{\"path\":\"\"}");
    parse("{\"path\":\"///\"}");
    parse("{\"path\":\"a\\nb.jpg\"}");
    parse("{\"path\":7}");
    parse("{}");
    parse(NULL);
    {
        char big[260 + 20];
        memset(big, 'x', 201);
        big[201] = 0;
        char json[300];
        snprintf(json, sizeof(json), "{\"path\":\"%s\"}", big);
        parse(json);
    }

    char body[320];
    size_t n = simg_chat_body(body, sizeof(body), "MuseGadget-SCW1-5AFC3C", "homelink-5afc3c");
    printf("body %zu %s\n", n, body);
    n = simg_chat_body(body, sizeof(body), "Evil\" name\\ \n", "node\"\\ x");
    printf("body2 %s\n", body);
    printf("small %zu\n", simg_chat_body(body, 20, "MuseGadget-SCW1-5AFC3C", "homelink-5afc3c"));

    /* The test's run: press, Muse takes it, calls, the picture shows, ends. */
    simg_run_t r;
    memset(&r, 0, sizeof(r));
    status(&r, 0);
    simg_on_ask(&r, 1000 * 1000);
    status(&r, 13500);
    printf("busy %d\n", simg_busy(&r));
    simg_on_chat_status(&r, 200, 1300LL * 1000);
    simg_on_chat_status(&r, 500, 1400LL * 1000);   /* a later frame changes nothing */
    simg_on_invoke(&r, 5000, 15200LL * 1000);
    status(&r, 15300);
    simg_on_drawn(&r, true, 48211, 412, 412, 1, 2800, NULL, 18000LL * 1000);
    status(&r, 18100);
    simg_tick(&r, 22999LL * 1000);
    printf("state %d\n", r.state);
    simg_tick(&r, 23000LL * 1000);
    status(&r, 23000);
    char rep[256];
    simg_report(&r, 23000LL * 1000, rep, sizeof(rep));
    printf("%s\n", rep);

    /* Muse refuses the request. */
    simg_on_ask(&r, 0);
    simg_on_chat_status(&r, 403, 200LL * 1000);
    status(&r, 300);
    printf("busy %d\n", simg_busy(&r));
    /* The session drops. */
    simg_on_ask(&r, 0);
    simg_on_chat_status(&r, -1, 200LL * 1000);
    status(&r, 300);
    /* No session opened in the 10 s the request waited (mgcommands.h). */
    simg_on_ask(&r, 0);
    simg_on_chat_status(&r, SIMG_CHAT_NO_SESSION, 10000LL * 1000);
    status(&r, 10100);
    /* No picture: the timeout, then a late one still shows (the same run). */
    simg_on_ask(&r, 0);
    simg_on_chat_status(&r, 200, 300LL * 1000);
    simg_tick(&r, (SIMG_ASK_TIMEOUT_MS - 1) * 1000LL);
    printf("state %d\n", r.state);
    simg_tick(&r, SIMG_ASK_TIMEOUT_MS * 1000LL);
    status(&r, SIMG_ASK_TIMEOUT_MS);
    simg_on_invoke(&r, 5000, (SIMG_ASK_TIMEOUT_MS + 5000) * 1000LL);
    printf("late asked=%d state=%d error=[%s]\n", r.asked, r.state, r.error);
    /* The download fails. */
    simg_on_drawn(&r, false, 900, 0, 0, 0, 1500, "not a valid JPEG", (SIMG_ASK_TIMEOUT_MS + 7000) * 1000LL);
    status(&r, SIMG_ASK_TIMEOUT_MS + 7000);
    simg_report(&r, (SIMG_ASK_TIMEOUT_MS + 7000) * 1000LL, rep, sizeof(rep));
    printf("%s\n", rep);
    /* Muse calls on its own later: a run of its own. */
    simg_on_invoke(&r, 3000, 500000LL * 1000);
    simg_on_drawn(&r, true, 20480, 412, 412, 1, 900, NULL, 501000LL * 1000);
    simg_tick(&r, 504000LL * 1000);
    status(&r, 504000);
    /* The ask itself failed (not connected). */
    simg_on_ask_failed(&r, "not connected to Muse", 0);
    status(&r, 1);
    printf("busy %d\n", simg_busy(&r));
    /* A drawn event without a command in flight is ignored. */
    simg_on_drawn(&r, true, 1, 1, 1, 1, 1, NULL, 2);
    printf("state %d\n", r.state);
    return 0;
}
"""


class CoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.out = None

    def run_core(self) -> list[str]:
        with tempfile.TemporaryDirectory(prefix="simg-core-") as name:
            h = Path(name) / "harness.c"
            h.write_text(CORE_HARNESS)
            out = compile_and_run(self, [h, MAIN / "session_image_core.c", FAKES / "cJSON.c"], [])
        return out.splitlines()

    def test_parameters(self) -> None:
        lines = [l for l in self.run_core() if l.startswith("parse")]
        self.assertEqual(lines, [
            "parse ok workspace/random.jpg 5000",
            "parse ok workspace/random.jpg 8000",
            "parse ok a.jpg 1000",
            "parse ok a.jpg 60000",
            "parse ok a.jpg 5000",
            "parse ok a.jpg 0",
            "parse invalid_params: duration_ms must be 0 (until dismissed) or 1000 to 60000",
            "parse invalid_params: duration_ms must be a number of milliseconds",
            "parse invalid_params: path is required: the JPEG's path on this VM, e.g. workspace/picture.jpg",
            "parse invalid_params: path names no file",
            "parse invalid_params: path has a control character",
            "parse invalid_params: path is required: the JPEG's path on this VM, e.g. workspace/picture.jpg",
            "parse invalid_params: path is required: the JPEG's path on this VM, e.g. workspace/picture.jpg",
            "parse invalid_params: path is required: the JPEG's path on this VM, e.g. workspace/picture.jpg",
            "parse invalid_params: path is too long (at most 200 characters)",
        ])

    def test_the_request_names_the_gadget_not_the_command(self) -> None:
        lines = self.run_core()
        size, raw = next(l for l in lines if l.startswith("body ")).split(" ", 2)[1:]
        body = json.loads(raw)
        self.assertEqual(int(size), len(raw))
        self.assertEqual(body, {"node_id": "homelink-5afc3c",
                                "message": "Please make a random picture (any subject you like) and show it on my "
                                           "gadget MuseGadget-SCW1-5AFC3C for 5 seconds.",
                                "output_modality": "text"})
        # Plain words: Muse finds display.show_image from link.register alone.
        self.assertNotIn("show_image", raw)
        self.assertNotIn("display.", raw)
        # A name with JSON's specials stays parsable (the specials dropped).
        body2 = json.loads(next(l for l in lines if l.startswith("body2 ")).split(" ", 1)[1])
        self.assertEqual(body2["message"], "Please make a random picture (any subject you like) and show it on my "
                                           "gadget Evil name  for 5 seconds.")
        self.assertEqual(body2["node_id"], "nodex")   # a node id's own characters only
        self.assertIn("small 0", lines)

    def test_a_run_and_its_lines(self) -> None:
        lines = [l for l in self.run_core() if not l.startswith(("parse", "body", "small"))]
        timeout = 120
        self.assertEqual(lines, [
            "status Asks Muse to make a picture and show it here for 5 s.",
            "status Asking Muse... 12 s",
            "busy 1",
            "status Muse sent a picture. Loading...",
            "status Showing it for 5 s",
            "state 3",
            "status Shown 5 s: 412x412, 47 KB, on screen 17.0 s after the press.",
            "img shown asked=1 chat=200@300ms invoke=14200ms drawn=17000ms fetch=2800ms bytes=48211 size=412x412 "
            "scale=1/1 show=5000ms age=22000ms",
            "status No picture: Muse didn't take the request (HTTP 403).",
            "busy 0",
            "status No picture: the connection to Muse dropped (is the Muse app open on the phone?).",
            "status No picture: Muse isn't connected yet; asked the phone to connect, press again in a few seconds "
            "(the Muse app must be open on the phone).",
            "state 1",
            f"status No picture: Muse sent no picture in {timeout} s.",
            "late asked=1 state=2 error=[]",
            "status No picture: not a valid JPEG.",
            f"img failed asked=1 chat=200@300ms invoke={(timeout + 5) * 1000}ms drawn=-1ms fetch=1500ms bytes=900 "
            f"size=0x0 scale=1/1 show=5000ms age={(timeout + 7) * 1000}ms error=\"not a valid JPEG\"",
            "status Muse showed a picture: 412x412, 20 KB.",
            "status No picture: not connected to Muse.",
            "busy 0",
            "state 5",
        ])


def register_json(test: unittest.TestCase, defines: list[str]) -> dict:
    """build_register_json() as a build with these options sends it, on the
    host with fakes for the display (412 px round, as the Watcher's)."""
    src = (MAIN / "noise_control.cpp").read_text()
    pieces = [function(src, s) for s in ("static cJSON *string_param(", "static void add_command(",
                                         "static void add_register_metadata_string(",
                                         "static char *build_register_json(")]
    rows = src[src.index("#if CONFIG_HOMEHUB_DISPLAY_COMMANDS && !CONFIG_MUSE_SESSION_IMAGE\n// The optional `row`"):]
    rows = rows[:rows.index("}\n#endif\n") + 9]
    with tempfile.TemporaryDirectory(prefix="simg-reg-") as name:
        h = Path(name) / "register.c"
        h.write_text(r"""
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "register_family.h"
#include "session_image_core.h"
#define nullptr NULL
#ifndef CONFIG_MUSE_DEVICE_MODEL_NAME   /* every overlay sets one (Kconfig, protocols/DEVICE.md) */
#define CONFIG_MUSE_DEVICE_MODEL_NAME "Seeed SenseCAP Watcher"
#endif
static esp_app_desc_t app = { .version = "1.2.3" };
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
static bool led_status_display_info(int *w, int *h) { *w = 412; *h = 412; return true; }
static int led_status_display_bits(void) { return 16; }
__attribute__((unused)) static bool led_status_display_round(void) { return true; }
static char s_wifi_ssid[33] = "", s_register_req_id[40];
static const char *s_node_id = "gadget-node", *s_display_name = "MuseGadget-SCW1-5AFC3C";
static void copy_wifi_ssid(char *out, size_t size) { snprintf(out, size, "%s", s_wifi_ssid); }
static void make_uuid(char *out, size_t size) { snprintf(out, size, "req-1"); }
""" + rows + "\n" + "\n".join(pieces) + r"""
int main(void) {
    char *json = build_register_json();
    if (!json) return 1;
    puts(json);
    free(json);
    return 0;
}
""")
        out = compile_and_run(test, [h, FAKES / "cJSON.c"], defines)
    return json.loads(out)


class RegisterTest(unittest.TestCase):
    BLE_ONLY = ["CONFIG_HOMEHUB_DISPLAY_COMMANDS=1", "CONFIG_HOMEHUB_LED_BACKEND_MUSE=1",
                "CONFIG_MUSE_GADGET_BLE_STANDALONE=1", "CONFIG_MUSE_SESSION_IMAGE=1"]
    WIFI = ["CONFIG_HOMEHUB_DISPLAY_COMMANDS=1", "CONFIG_HOMEHUB_LED_BACKEND_MUSE=1"]
    BLE_ONLY_NO_TEST = ["CONFIG_HOMEHUB_DISPLAY_COMMANDS=1", "CONFIG_HOMEHUB_LED_BACKEND_MUSE=1",
                        "CONFIG_MUSE_GADGET_BLE_STANDALONE=1"]

    def test_ble_only_registers_show_image_and_no_url(self) -> None:
        reg = register_json(self, self.BLE_ONLY)
        commands = reg["params"]["commands_v2"]
        # One way to show a picture: with draw_file there too, Muse kept
        # using it and timed the showing with show_animation.
        self.assertNotIn("display.draw_url", commands)
        self.assertNotIn("display.draw_file", commands)
        self.assertIn("display.show_animation", commands)
        show = commands["display.show_image"]
        self.assertEqual(set(show["required"]), {"path"})
        self.assertEqual(set(show["optional"]), {"duration_ms"})
        self.assertEqual(show["optional"]["duration_ms"]["type"], "integer")
        self.assertEqual(show["timeout_ms"], 90000)
        d = show["description"]
        # It teaches the use case: when to use it, what to make, the screen's
        # format and size, where the file goes, what comes back.
        for words in ("Show a picture on this gadget's screen for duration_ms", "This is how to show any picture",
                      "generate one",
                      "baseline (not progressive) JPEG of 412x412 pixels", "quality about 70 and under 30 KB",
                      "save it on this VM",
                      "round (the corners are cut off), 412x412", "replies once the picture is on screen"):
            self.assertIn(words, d)
        self.assertLessEqual(len(d.encode()), 1000)
        self.assertIn("0 keeps it until a tap", show["optional"]["duration_ms"]["description"])
        # link.register is printed into at most 8 KB (AGENTS.md, Adding a command).
        self.assertLess(len(json.dumps(reg, separators=(",", ":")).encode()), 8 * 1024)

    def test_a_gadget_registers_its_model_name_within_8_kb(self) -> None:
        # metadata.model_name, the board's product name; within the 8 KB link.register
        # is printed into, in the BLE-only build (show_image's long description) and the Wi-Fi one alike.
        gadget = ["CONFIG_MUSE_GADGET_BLE_AUDIO=1", 'CONFIG_HOMEHUB_BLE_PRODUCT_CODE="SCW1"',
                  'CONFIG_MUSE_DEVICE_MODEL_ID="muse-gadget-sensecap-watcher"', 'CONFIG_HOMEHUB_MODEL_NAME="Watcher"']
        for build in (self.BLE_ONLY, self.WIFI):
            reg = register_json(self, build + gadget)
            self.assertEqual(reg["params"]["metadata"]["model_name"], "Watcher")
            self.assertEqual((reg["params"]["device_family"], reg["params"]["model_id"]),
                             ("gadget", "muse-gadget-sensecap-watcher"))
            self.assertLess(len(json.dumps(reg, separators=(",", ":")).encode()), 8 * 1024)
        # No short name set: the model name its overlay sets (CONFIG_MUSE_DEVICE_MODEL_NAME), the only
        # metadata without an SSID.
        self.assertEqual(register_json(self, self.BLE_ONLY)["params"]["metadata"],
                         {"model_name": "Seeed SenseCAP Watcher"})

    def test_family_split_and_the_overlay_model_id(self) -> None:
        # "gadget" for every build, Wi-Fi only included; model_id is the board's overlay id, never the
        # catalog code, and the Kconfig default without an overlay.
        board = ['CONFIG_MUSE_DEVICE_MODEL_ID="muse-gadget-m5stack-stopwatch"', 'CONFIG_HOMEHUB_BLE_PRODUCT_CODE="M5SW"']
        for build, family in ((self.WIFI, "gadget"), (self.WIFI + ["CONFIG_MUSE_GADGET_BLE_AUDIO=1"], "gadget"),
                              (self.BLE_ONLY + ["CONFIG_MUSE_GADGET_BLE_AUDIO=1"], "gadget")):
            params = register_json(self, build + board)["params"]
            self.assertEqual((params["device_family"], params["model_id"]), (family, "muse-gadget-m5stack-stopwatch"))
        params = register_json(self, self.WIFI)["params"]
        self.assertEqual((params["device_family"], params["model_id"]), ("gadget", "muse-gadget"))

    def test_ble_only_without_the_test_keeps_draw_file_alone(self) -> None:
        commands = register_json(self, self.BLE_ONLY_NO_TEST)["params"]["commands_v2"]
        self.assertNotIn("display.draw_url", commands)
        self.assertNotIn("display.show_image", commands)
        # draw_file says it all, with no draw_url to point at.
        self.assertNotIn("display.draw_url", commands["display.draw_file"]["description"])
        self.assertIn("Takes a path on this VM", commands["display.draw_file"]["description"])
        self.assertEqual(commands["display.draw_file"]["timeout_ms"], 60000)

    def test_wifi_keeps_draw_url_and_has_no_show_image(self) -> None:
        commands = register_json(self, self.WIFI)["params"]["commands_v2"]
        self.assertIn("display.draw_url", commands)
        self.assertIn("display.draw_file", commands)
        self.assertNotIn("display.show_image", commands)
        self.assertIn("display.draw_url", commands["display.draw_file"]["description"])


class WiringTest(unittest.TestCase):


    def test_nordic_uart_words(self) -> None:
        glue = (MAIN / "mg_glue.c").read_text()
        allow = function(glue, "static int ble_bench_input(char *line) {")
        harness = r"""
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_MUSE_SESSION_IMAGE 1
#define CONFIG_MUSE_GADGET_BLE_SESSION 1
static bool muse_input_console_post(const char *line) { (void)line; return true; }
@ALLOW@
int main(void) {
    static const char *ok[] = { "img.ask", "img?", "img.show=workspace/a.jpg,5000", "dev?", "dev=1", "dev=0" };
    static const char *no[] = { "img", "img.asks", "dev", "dev=2" };
    int bad = 0;
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) { char b[64]; strcpy(b, ok[i]); if (ble_bench_input(b) != 1) { printf("refused %s\n", ok[i]); bad++; } }
    for (unsigned i = 0; i < sizeof(no) / sizeof(no[0]); i++) { char b[64]; strcpy(b, no[i]); if (ble_bench_input(b) != 0) { printf("allowed %s\n", no[i]); bad++; } }
    printf("bad %d\n", bad);
    return 0;
}
""".replace("@ALLOW@", allow)
        with tempfile.TemporaryDirectory(prefix="simg-nus-") as tmp:
            h = Path(tmp) / "a.c"
            h.write_text(harness)
            out = compile_and_run(self, [h], [])
        self.assertEqual(out.strip().splitlines()[-1], "bad 0", out)
        unlock = (ROOT / "components/muse_gadget_ble/mg_unlock.c").read_text()
        word = function(unlock, "bool mg_unlock_automation_word(const char *line)")
        self.assertIn('strcmp(w, "img?") && strcmp(w, "dev?")', word)   # diagnostics stay with the switch off
        # img? is the owner's content (the shown image's path): proven only on a set-up release gadget.
        self.assertIn('return !strcmp(line, "!IN voice?") || !strcmp(line, "!IN img?");', unlock)


if __name__ == "__main__":
    unittest.main()
