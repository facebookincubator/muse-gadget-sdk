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

"""The web flasher's updater rules, on the host.

web-flasher/flasher-core.js and version-read.js run in Node: the version
compare and the console's version lines (the firmware's own formats, taken
from its source), the stop messages, the session log masked exactly as
tools/redact.py masks (same inputs, same output), and the version read over a
fake Web Serial port. make_manifest.py's version comes from the app image."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))   # its helpers, also when run from esp32/ as tests.<module>
from host_child import run_child  # noqa: E402 (macOS's pipe deadlock on a driver's input: host_child.py)

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
FLASHER = REPO / "web-flasher"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(FLASHER))
import make_manifest  # noqa: E402
import redact  # noqa: E402

FAKE = "mgst_FAKEfakeFAKEfake0123456789abcdefghijklmnopq"   # made up: never a real token
# Made up too, but they pass the page's token check (the last character carries four bits).
TOKEN1 = "mgst_" + ("FAKEfake0123456789" * 3)[:42] + "A"
TOKEN2 = "mgst_" + ("OTHERother9876543210" * 3)[:42] + "Q"
ACCESS = "AcXq7Zk2Pq9Lm4Rt8Wv1Yb3NuQ5sD6fG"
REFRESH = "RfZt5Hj8Kd2Qw6Er9Ty0Ui4OpA7sS1dF3"

DRIVER = r"""
import * as core from "./flasher-core.js";
import { readGadgetVersion } from "./version-read.js";
import { createTokenMemory, LEGACY_KEY, STORAGE_KEY } from "./token-memory.js";
import { render, setLanguage } from "./i18n.js";

const input = JSON.parse(await new Promise((resolve) => {
  let data = "";
  process.stdin.on("data", (c) => (data += c));
  process.stdin.on("end", () => resolve(data));
}));

const enc = new TextEncoder();
const fromHex = (h) => Uint8Array.from(h.match(/../g) ?? [], (b) => parseInt(b, 16));

// A Web Serial port: reply(text, push) answers what the page writes.
function fakePort({ reply, initial = [], chunk = 7 } = {}) {
  let ctrl;
  const push = (text) => {
    const bytes = enc.encode(text);
    for (let i = 0; i < bytes.length; i += chunk) ctrl.enqueue(bytes.slice(i, i + chunk));
  };
  const port = {
    written: "", opens: 0, closes: 0, signals: null, readable: null, writable: null,
    async open(options) {
      if (port.readable) throw new Error("already open");
      port.opens++;
      port.baud = options.baudRate;
      port.readable = new ReadableStream({ start(c) { ctrl = c; } });
      port.writable = new WritableStream({
        write(data) {
          const text = new TextDecoder().decode(data);
          port.written += text;
          reply?.(text, push);
        },
      });
      for (const line of initial) push(line);
    },
    async setSignals(s) { port.signals = s; },
    async close() { port.closes++; port.readable = null; port.writable = null; },
  };
  return port;
}

const out = {};
out.sha256 = input.sha256.map((h) => core.sha256Hex(fromHex(h)));
out.fingerprint = input.fingerprint.map((s) => core.fingerprint(s));
out.redact = input.redact.map((s) => core.redactText(s));
out.parse = input.parse.map((l) => core.parseVersionLine(l));
out.match = input.match.map(([a, b]) => core.versionsMatch(a, b));
// What the page says is an i18n.js message: drawn in English, then Japanese.
const transfers = () => [core.transferLines({}), core.transferLines({ worn: true }), core.transferLines(undefined)];
out.stop = input.stop.map((c) => render(core.stopMessage(c)));
out.transfer = transfers().map((lines) => lines.map(render));
setLanguage("ja");
out.stopJa = input.stop.map((c) => render(core.stopMessage(c)));
out.transferJa = transfers().map((lines) => lines.map(render));
setLanguage("en");

// The session log.
let t = 0;
const log = core.createSessionLog({ now: () => new Date(Date.UTC(2026, 10, 7, 18, 0, t++)) });
const seen = [];
const fakeConsole = { log: (...a) => seen.push(["log", ...a]), info() {}, warn() {}, error: (...a) => seen.push(["error", ...a]), debug() {} };
log.teeConsole(fakeConsole);
fakeConsole.log("Writing at 0x10000... (3%)");
fakeConsole.error(new Error(`write failed near >sdk_token=${input.token}`));
log.add("token", `sending ${input.token}`, { sdk_token: input.token, n: 1 });
log.add("gadget", "line one\nline two");
out.log = log.text();
out.consoleSeen = seen.length;
const small = core.createSessionLog({ now: () => new Date(0), maxLines: 4 });
for (let i = 0; i < 10; i++) small.add("x", `line ${i}`);
out.smallLog = small.text();
out.fileName = core.logFileName("muse-sensecap-watcher", new Date(2026, 10, 7, 9, 5, 3));
out.fileNameNone = core.logFileName(undefined, new Date(2026, 10, 7, 9, 5, 3));

// The version read over a fake port.
const answering = fakePort({ reply: (text, push) => text.includes(">version") && push("\x1b[0;32mI (812) mg.ble: x\x1b[0m\r\n@version 1.0.0+g75d81c33\r\n") });
const answer = await readGadgetVersion(answering, { firstAskMs: 0, timeoutMs: 3000 });
out.answer = { found: answer, written: answering.written, opens: answering.opens, closes: answering.closes, baud: answering.baud, signals: answering.signals, locked: answering.readable };

const lines = [];
const booting = fakePort({ initial: ["ESP-ROM:esp32s3\r\n", "\x1b[0;32mI (301) link.app:   Version:  1.0.0\x1b[0m\r\n"] });
const boot = await readGadgetVersion(booting, { firstAskMs: 1000, timeoutMs: 3000, onLine: (l) => lines.push(l) });
out.boot = { found: boot, written: booting.written, lines };

const already = fakePort({ reply: (text, push) => push("@version 2.0.0\n") });
await already.open({ baudRate: 115200 });
const kept = await readGadgetVersion(already, { open: false, firstAskMs: 0, timeoutMs: 2000 });
out.kept = { found: kept, closes: already.closes, readerFree: !already.readable.locked, signals: already.signals };

const stopping = fakePort();
await stopping.open({ baudRate: 115200 });
const abort = new AbortController();
setTimeout(() => abort.abort(), 100);
// A timeout far past the driver's own: only the abort can end this read.
const aborted = await readGadgetVersion(stopping, { open: false, firstAskMs: 0, timeoutMs: 3600000, signal: abort.signal });
out.aborted = { found: aborted, readerFree: !stopping.readable.locked };

// A port still closing (ESP Web Tools' dialog): open throws InvalidStateError twice with both streams gone.
const closing = fakePort({ reply: (text, push) => push("@version 3.0.0\n") });
const realOpen = closing.open;
let refusals = 0;
closing.open = async (options) => {
  if (refusals < 2) { refusals++; throw new DOMException("The port is already open.", "InvalidStateError"); }
  return realOpen(options);
};
const retried = await readGadgetVersion(closing, { firstAskMs: 0, timeoutMs: 3000, openRetryMs: 1 });
out.closing = { found: retried, refusals, opens: closing.opens, closes: closing.closes };

// The token and "Remember this token on this computer".
const store = (init = {}) => {
  const m = new Map(Object.entries(init));
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), removeItem: (k) => m.delete(k), dump: () => Object.fromEntries(m) };
};
const valid = (t) => /^mgst_[A-Za-z0-9_-]{42}[AEIMQUYcgkosw048]$/.test(t);
const [T1, T2] = input.tokens;
const steps = [];
const snap = (name, mem, st) => steps.push({ name, token: mem.token() === null ? null : mem.token() === T1 ? "T1" : mem.token() === T2 ? "T2" : "other", remember: mem.remember(), saved: mem.saved(), stored: st.dump() });
let st = store({ [LEGACY_KEY]: T1, other: "x" });
let mem = createTokenMemory(st, valid);
snap("legacy copy at load", mem, st);
mem.enter(T1); snap("entered", mem, st);
mem.setRemember(true); snap("ticked before a send", mem, st);
mem.sent(T1); snap("sent", mem, st);
mem = createTokenMemory(st, valid); snap("reload", mem, st);
mem.enter("mgst_half"); snap("invalid entry", mem, st);
mem.enter(T2); snap("new token, not sent", mem, st);
mem.sent(T1); snap("a stale send", mem, st);
mem.sent(T2); snap("new token sent", mem, st);
mem.forget(); snap("forget", mem, st);
mem.setRemember(true); snap("ticked after a send", mem, st);
mem.setRemember(false); snap("unticked", mem, st);
mem.setRemember(true); mem.clear(); snap("clear", mem, st);
st = store({ [STORAGE_KEY]: "garbage" }); mem = createTokenMemory(st, valid); snap("invalid saved copy", mem, st);
const throwing = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); }, removeItem() { throw new Error("blocked"); } };
mem = createTokenMemory(throwing, valid); mem.enter(T1); mem.setRemember(true); mem.sent(T1);
steps.push({ name: "blocked storage", token: mem.token() === T1 ? "T1" : "other", remember: mem.remember(), saved: mem.saved(), stored: {} });
mem = createTokenMemory(null, valid); mem.enter(T1); mem.setRemember(true); mem.sent(T1);
steps.push({ name: "no storage", token: mem.token() === T1 ? "T1" : "other", remember: mem.remember(), saved: mem.saved(), stored: {} });
out.memory = steps;
out.keys = [STORAGE_KEY, LEGACY_KEY];

process.stdout.write(JSON.stringify(out));
"""


def assert_no_secret(case: unittest.TestCase, text: str) -> None:
    """None of a secret's characters: no 6-character piece of any of them."""
    for secret in (FAKE[5:], ACCESS, REFRESH, "hunter2wifi"):
        for i in range(len(secret) - 5):
            case.assertNotIn(secret[i:i + 6], text, f"a piece of a secret in: {text[:300]}")


REDACT_INPUTS = [
    f">sdk_token={FAKE}",
    f"I (5512) link.sdk_token: got >sdk_token={FAKE} from the console",
    json.dumps({"sdk_token": FAKE, "access_token": ACCESS, "name": "Muse"}),
    "{'password': 'hunter2wifi', 'ssid': 'home'}",
    '{"token": "", "Access_Token": "' + ACCESS + '"}',
    f"Authorization: Bearer hatch_refresh:{REFRESH}",
    f"refresh with hatch_refresh:{REFRESH} now",
    "stored the SDK token (48 chars, sha256 1a2b3c4d), restarting to use it",
    f"I (900) app: hatch.token={ACCESS}",
    f"token={ACCESS} at the start",
    "mgst_ab is too short to be one",
    '{"password": "p\u00e4ssw\u00f6rd\u00fc"}',
    f"\x1b[0;33mW (77) x: {FAKE[:20]}\x1b[0m",
    "",
    "nothing secret here: link.app:   Version:  1.0.0+g75d81c33",
]
# Secrets whose tail ("LEAK") an end-of-value guess once let through.
LEAK_INPUTS = [
    '{"password": "it\'sLEAK"}',              # the other quote inside
    '{"password": "a\\"bLEAK", "ssid": "x"}',  # an escaped quote
    "{'psk': 'a\\'bLEAK'}",
    '{"password": "never closedLEAK',
    ">wifi=home,two wordsLEAK",
    "wifi.pass=two wordsLEAK",
    "I (1) muse_ble: hatch.token+=abcdefghLEAK",
]
REDACT_INPUTS += LEAK_INPUTS

PARSE_LINES = [
    "@version 1.0.0+g75d81c33",
    "\x1b[0;32mI (4021) mg.ble: status ok\x1b[0m@version 1.2.3",
    "I (301) link.app:   Version:  1.0.0+g75d81c33.dirty",
    "\x1b[0;32mI (301) link.app:   Version:  1.0.0\x1b[0m",
    "I (301) link.app:   Version:  ?",
    "I (812) mg.ble: version 1.0.0, mg3",   # the Nordic UART's word, not the USB console's
    "I (301) other.tag:   Version:  9.9.9",
    "random noise",
]

MATCH_CASES = [
    (["1.0.0+g75d81c33", "1.0.0+g75d81c33"], True),
    (["1.0.0", "1.0.0"], True),
    (["1.0.0+75d81c3", "1.0.0+g75d81c33"], True),     # a manifest from before the image's own version
    (["1.0.0+g75d81c33", "1.0.0+g75d81c34"], False),
    (["1.0.0+g75d81c33", "1.0.1+g75d81c33"], False),
    (["1.0.0+g75d81c33", "1.0.0+g75d81c33.dirty"], False),
    (["1.0.0", "1.0.0+g75d81c33"], False),
    (["1.0.0+g75d81c33", "1.0.0"], False),
    (["v1.2.0", "1.2.0"], True),
    (["1.0.0+gabc", "1.0.0+gabd"], False),            # too short to be a commit: exact only
    (["", "1.0.0"], False),
    ([None, "1.0.0"], False),
    (["1.0.0", None], False),
    (["999.0.0", "999.0.0"], True),
]


@unittest.skipUnless(shutil.which("node"), "needs Node (CI installs nodejs)")
class WebFlasherCoreTest(unittest.TestCase):
    out: dict

    @classmethod
    def setUpClass(cls) -> None:
        cls.sha_inputs = [os.urandom(n).hex() for n in (0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 200, 1000)]
        cls.fp_inputs = [None, "", "abc", FAKE, "p\u00e4ss"]
        payload = {
            "sha256": cls.sha_inputs,
            "fingerprint": cls.fp_inputs,
            "redact": REDACT_INPUTS,
            "parse": PARSE_LINES,
            "match": [c for c, _ in MATCH_CASES],
            "stop": [
                {"cancelled": True, "oldVersion": "1.0.0+gaaaaaaaa"},
                {"cancelled": True},
                {"oldVersion": "1.0.0"},
                {},
                {"erased": True, "oldVersion": "1.0.0"},
                {"writeStarted": True, "erased": True, "oldVersion": "1.0.0"},
            ],
            "token": FAKE,
            "tokens": [TOKEN1, TOKEN2],
        }
        with tempfile.TemporaryDirectory() as tmp:
            for js in FLASHER.glob("*.js"):
                shutil.copy(js, tmp)
            Path(tmp, "package.json").write_text('{"type": "module"}\n')
            Path(tmp, "driver.js").write_text(DRIVER)
            run = run_child(["node", "driver.js"], cwd=tmp, input=json.dumps(payload), capture_output=True,
                            text=True, timeout=120)
        if run.returncode != 0:
            raise AssertionError(f"node failed:\n{run.stderr}")
        cls.out = json.loads(run.stdout)

    def test_sha256(self) -> None:
        for h, got in zip(self.sha_inputs, self.out["sha256"]):
            self.assertEqual(got, hashlib.sha256(bytes.fromhex(h)).hexdigest(), f"{len(h) // 2} bytes")

    def test_fingerprint_is_redact_py(self) -> None:
        for s, got in zip(self.fp_inputs, self.out["fingerprint"]):
            self.assertEqual(got, redact.fingerprint(s))

    def test_masking_is_redact_py(self) -> None:
        for text, got in zip(REDACT_INPUTS, self.out["redact"]):
            self.assertEqual(got, redact.redact_text(text), text)
            assert_no_secret(self, got)
        for text in LEAK_INPUTS:
            self.assertNotIn("LEAK", redact.redact_text(text), text)

    def test_version_lines(self) -> None:
        got = self.out["parse"]
        self.assertEqual(got[0], {"version": "1.0.0+g75d81c33", "source": "answer"})
        self.assertEqual(got[1], {"version": "1.2.3", "source": "answer"})
        self.assertEqual(got[2], {"version": "1.0.0+g75d81c33.dirty", "source": "boot"})
        self.assertEqual(got[3], {"version": "1.0.0", "source": "boot"})
        self.assertEqual(got[4:], [None] * 4)


    def test_versions_match(self) -> None:
        for (case, want), got in zip(MATCH_CASES, self.out["match"]):
            self.assertEqual(got, want, case)

    def test_stop_messages(self) -> None:
        cancelled, cancelled_unknown, failed, failed_unknown, erased, partial = self.out["stop"]
        self.assertEqual(cancelled, "Nothing was installed; your gadget still runs 1.0.0+gaaaaaaaa.")
        self.assertEqual(cancelled_unknown, "Nothing was installed; your gadget still runs the firmware it had.")
        self.assertEqual(failed, "The update stopped before it finished. Nothing was installed; your gadget still "
                                 "runs 1.0.0.")
        self.assertTrue(failed_unknown.startswith("The update stopped before it finished. Nothing was installed"))
        self.assertIn("erased", erased)
        self.assertNotIn("Nothing was installed", erased)
        self.assertIn("partly written", partial)
        for m in self.out["stop"]:
            self.assertNotIn("Hatch", m)
        # The same stops in Japanese, with the version the gadget still runs.
        japanese = re.compile(r"[\u3040-\u30ff\u4e00-\u9fff]")
        for en, ja in zip(self.out["stop"], self.out["stopJa"]):
            self.assertRegex(ja, japanese)
            self.assertNotEqual(ja, en)
        self.assertIn("1.0.0+gaaaaaaaa", self.out["stopJa"][0])
        self.assertIn("1.0.0", self.out["stopJa"][2])


    def test_session_log(self) -> None:
        text = self.out["log"]
        assert_no_secret(self, text)
        self.assertEqual(self.out["consoleSeen"], 2, "the console still prints")
        self.assertIn("[log] Writing at 0x10000... (3%)", text)
        self.assertIn("[error] Error: write failed near >sdk_token=<redacted: 48 chars, sha256 ", text)
        self.assertIn(f"<redacted: {redact.fingerprint(FAKE)}>", text)
        self.assertIn('"n":1', text)
        self.assertIn("2026-11-07T18:00:03.000Z [gadget] line one\n2026-11-07T18:00:03.000Z [gadget] line two\n",
                      text)
        small = self.out["smallLog"].splitlines()
        self.assertEqual([s.split("] ", 1)[-1] for s in small],
                         ["line 0", "line 1", "(6 lines from the middle of the session dropped)", "line 8",
                          "line 9"])
        self.assertEqual(self.out["fileName"], "muse-web-flasher-muse-sensecap-watcher-20261107-090503.txt")
        self.assertEqual(self.out["fileNameNone"], "muse-web-flasher-session-20261107-090503.txt")

    def test_token_memory(self) -> None:
        key, legacy = self.out["keys"]
        got = {s["name"]: s for s in self.out["memory"]}

        def saved(name, which):
            stored = got[name]["stored"].get(key)
            self.assertEqual(stored, {"T1": TOKEN1, "T2": TOKEN2, None: None}[which], name)

        load = got["legacy copy at load"]
        self.assertEqual((load["token"], load["remember"], load["saved"]), (None, False, False),
                         "a copy saved before the opt-in is never adopted")
        self.assertNotIn(legacy, load["stored"], "and it's deleted")
        self.assertEqual(load["stored"].get("other"), "x", "other keys stay")
        saved("entered", None)
        self.assertEqual(got["entered"]["token"], "T1")
        saved("ticked before a send", None)
        self.assertTrue(got["ticked before a send"]["remember"])
        saved("sent", "T1")
        r = got["reload"]
        self.assertEqual((r["token"], r["remember"], r["saved"]), ("T1", True, True), "a reload fills it in, ticked")
        saved("invalid entry", "T1")
        self.assertEqual(got["invalid entry"]["token"], "T1")
        saved("new token, not sent", "T1")
        self.assertEqual(got["new token, not sent"]["token"], "T2")
        saved("a stale send", "T1")
        saved("new token sent", "T2")
        f = got["forget"]
        self.assertEqual((f["token"], f["remember"], f["saved"]), ("T2", False, False), "forget keeps the page's token")
        saved("ticked after a send", "T2")
        saved("unticked", None)
        c = got["clear"]
        self.assertEqual((c["token"], c["remember"], c["saved"]), (None, False, False))
        self.assertEqual(got["invalid saved copy"]["stored"], {}, "an invalid saved copy is deleted")
        self.assertIsNone(got["invalid saved copy"]["token"])
        for name in ("blocked storage", "no storage"):
            self.assertEqual((got[name]["token"], got[name]["saved"]), ("T1", False), name)

    def test_version_read_asks(self) -> None:
        a = self.out["answer"]
        self.assertEqual(a["found"], {"version": "1.0.0+g75d81c33", "source": "answer"})
        self.assertEqual(a["written"], "\n>version\n")
        self.assertEqual((a["opens"], a["closes"], a["baud"]), (1, 1, 115200))
        self.assertEqual(a["signals"], {"dataTerminalReady": False, "requestToSend": False})

    def test_version_read_boot_log(self) -> None:
        b = self.out["boot"]
        self.assertEqual(b["found"], {"version": "1.0.0", "source": "boot"})
        self.assertEqual(b["written"], "", "the boot log answered before it asked")
        self.assertEqual(b["lines"][0], "ESP-ROM:esp32s3")


    def test_version_read_waits_for_a_closing_port(self) -> None:
        c = self.out["closing"]
        self.assertEqual(c["found"], {"version": "3.0.0", "source": "answer"})
        self.assertEqual((c["refusals"], c["opens"], c["closes"]), (2, 1, 1))

    def test_version_read_on_an_open_port(self) -> None:
        k = self.out["kept"]
        self.assertEqual(k["found"], {"version": "2.0.0", "source": "answer"})
        self.assertEqual(k["closes"], 0, "a port it didn't open stays open")
        self.assertTrue(k["readerFree"], "and its reader is released for the installer")
        self.assertIsNone(k["signals"], "EN and BOOT untouched")
        ab = self.out["aborted"]
        self.assertIsNone(ab["found"])
        self.assertTrue(ab["readerFree"])


def app_image(version: bytes, magic: int = make_manifest.APP_DESC_MAGIC) -> bytes:
    header = bytes([0xE9, 4, 2, 0x20]) + bytes(20)        # esp_image_header_t, 24 bytes
    segment = struct.pack("<II", 0x3C000020, 256)           # the first segment: the app description
    desc = struct.pack("<IIII", magic, 0, 0, 0) + version.ljust(32, b"\0") + b"muse-gadget".ljust(32, b"\0")
    return header + segment + desc + bytes(256)


class MakeManifestTest(unittest.TestCase):
    def test_version_from_the_image(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build, out = Path(tmp, "build"), Path(tmp, "out")
            (build / "partition_table").mkdir(parents=True)
            (build / "bootloader").mkdir()
            (build / "muse-gadget.bin").write_bytes(app_image(b"1.0.0+g75d81c33"))
            (build / "bootloader/bootloader.bin").write_bytes(b"\xe9" + bytes(31))
            table = struct.pack("<HBBII16s4x", 0x50AA, 0, 0, 0x20000, 0x400000, b"ota_0") + b"\xff" * 32
            (build / "partition_table/partition-table.bin").write_bytes(table)
            (build / "flasher_args.json").write_text(json.dumps({
                "extra_esptool_args": {"chip": "esp32s3"},
                "flash_files": {"0x0": "bootloader/bootloader.bin", "0x10000": "partition_table/partition-table.bin",
                                "0x20000": "muse-gadget.bin"},
                "partition-table": {"offset": "0x10000", "file": "partition_table/partition-table.bin"},
                "app": {"offset": "0x20000", "file": "muse-gadget.bin"},
            }))
            run = subprocess.run([sys.executable, str(FLASHER / "make_manifest.py"), str(build), str(out),
                                  "Muse Gadget (Test)"], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["version"], "1.0.0+g75d81c33")
        self.assertEqual(manifest["new_install_improv_wait_time"], 0)
        self.assertEqual(manifest["min_flash_bytes"], 0x420000)
        self.assertEqual([p["offset"] for p in manifest["builds"][0]["parts"]], [0, 0x10000, 0x20000])

    def test_bad_images(self) -> None:
        with self.assertRaises(ValueError):
            make_manifest.app_version(b"\x00" * 100)
        with self.assertRaises(ValueError):
            make_manifest.app_version(app_image(b"1.0.0", magic=0x12345678))
        with self.assertRaises(ValueError):
            make_manifest.app_version(app_image(b""))
        self.assertEqual(make_manifest.app_version(app_image(b"x" * 32)), "x" * 32)


if __name__ == "__main__":
    unittest.main()
