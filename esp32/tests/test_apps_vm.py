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
"""The apps API 1.0's VM side (chat, file reads, state): the chat's parser
and message store run on the host against synthetic lines, and the request
code keeps the gadget's headers, its bounds and its permissions."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APPS = ROOT / "components/muse_apps"
CHAT = APPS / "muse_apps_chat.c"
FILES = APPS / "muse_apps_files.c"
VM = APPS / "muse_apps_vm.c"

SECTION_START = "/* ---- The chat's formats and messages"
SECTION_END = "/* ---- The chat for apps ---- */"


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 0
    for i in range(body, len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError("unterminated " + signature)


class ChatParserAndStore(unittest.TestCase):
    def test_on_the_host(self):
        source = CHAT.read_text()
        section = source[source.index(SECTION_START):source.index(SECTION_END)]
        self.assertNotIn("#include", section)
        for idf in ("muse_apps_", "esp_", "xSemaphore", "heap_caps", "ESP_LOG"):
            self.assertNotIn(idf, section, "the host-tested part is plain C")
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "chat_section.inc").write_text(section)
            binary = Path(tmp) / "chat"
            base = [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-I", tmp, "-I", str(APPS),
                    str(ROOT / "tests/apps_vm_chat_harness.c"), "-o", str(binary)]
            sanitize = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            built = subprocess.run(base[:1] + sanitize + base[1:], capture_output=True, text=True)
            if built.returncode:  # a compiler without the sanitizers
                built = subprocess.run(base, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


class Requests(unittest.TestCase):
    def test_every_request_carries_the_gadgets_headers(self):
        """Muse's own chat requests on Link's session send x-request-id and
        x-app-id; the apps' requests (/fs/write among them) send the same."""
        vm = VM.read_text()
        headers = function(vm, "static const char *const *gadget_headers(")
        for name in ('"x-request-id"', '"x-app-id"'):
            self.assertIn(name, headers)
        self.assertIn('#define APP_ID "hatch-web"', vm)
        self.assertIn('"x-app-id", "hatch-web"', (ROOT / "components/muse/muse_chat_link.c").read_text())
        self.assertNotIn("const char *headers[] =", vm)
        opens = re.findall(r"= muse_link_req_open\(([^;]*)\);", vm)
        self.assertEqual(len(opens), 4)
        for call in opens:
            self.assertRegex(call, r"headers|gadget_headers\(")
        for module in (CHAT, FILES):
            self.assertNotIn("muse_link_req_open(", module.read_text(), "through muse_apps_vm_* only")

    def test_refusals_are_logged_with_their_body(self):
        vm = VM.read_text()
        self.assertIn("s_r.err_len", function(vm, "static void on_frame("))
        self.assertIn("s_w.err_len", function(vm, "static void w_frame("))
        self.assertIn("ESP_LOGW", function(vm, "static int post_json("))
        self.assertIn("ESP_LOGW", function(vm, "int muse_apps_vm_fetch("))
        self.assertIn("ESP_LOGI", function(FILES.read_text(), "static void save_run("))

    def test_bounds(self):
        vm = VM.read_text()
        self.assertIn("#define BODY_PIECE 6144", vm)          # under the session's 8 KB a piece
        files = FILES.read_text()
        self.assertIn("muse_link_req_max_body()", function(files, "static void read_run("))
        self.assertIn('"Range"', vm)
        self.assertIn('\\"overwrite\\":true,\\"append\\":false,\\"create_parent\\":true', files)
        chat = CHAT.read_text()
        self.assertIn('"/chat/history?limit=1"', chat)          # one message fits a frame
        self.assertIn('"[%s on the gadget] %s"', chat)
        self.assertIn('"output_modality\\":\\"text\\"', chat)

    def test_permissions(self):
        chat = CHAT.read_text()
        for name in ("gadget_chat_open(", "gadget_chat_close(", "gadget_chat_latest(", "gadget_chat_info(",
                     "gadget_chat_text("):
            self.assertIn('muse_apps_permitted("chat.read")', function(chat, name), name)
        self.assertIn('muse_apps_permitted("chat.send")', function(chat, "int32_t gadget_chat_send("))
        self.assertIn("muse_apps_vm_path(", function(FILES.read_text(), "int32_t gadget_file_read("))

    def test_stopping_lets_go(self):
        self.assertIn("atomic_fetch_add(&s_epoch, 1);", function(VM.read_text(), "void muse_apps_vm_close_all("))
        chat = CHAT.read_text()
        end = function(chat, "static void chat_end(")
        for step in ("muse_link_req_cancel(id);", "muse_apps_free(mem);", "esp_timer_stop("):
            self.assertIn(step, end)
        self.assertIn("chat_end();", function(chat, "void muse_apps_chat_stop("))
        self.assertIn("muse_apps_free(t);", function(FILES.read_text(), "void muse_apps_files_stop("))

    def test_no_large_static_buffers(self):
        for path in (CHAT, FILES, VM):
            source = path.read_text()
            defines = {k: int(v) for k, v in re.findall(r"#define\s+(\w+)\s+\(?(\d+)\)?\s", source)}
            for kind, name, size in re.findall(
                    r"^static\s+(?!const)([\w ]+?)\s+\**(\w+)\[(\w+)\]\s*[;=]", source, re.M):
                count = int(size) if size.isdigit() else defines.get(size, 0)
                width = 1 if "char" in kind else 4
                self.assertLess(count * width, 512, "%s: static %s[%s]" % (path.name, name, size))
            self.assertNotIn("xQueueCreate(", source)


if __name__ == "__main__":
    unittest.main()
