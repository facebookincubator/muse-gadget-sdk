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

"""The firmware logs no token characters: whether there is one, its length and
a SHA-256 prefix (main/token_fp.h, token_fp_text()) at most (AGENTS.md, Muse
Gadgets SDK token). test_link_pairing_handshake runs token_fp_text() itself."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TokenLogsTest(unittest.TestCase):
    def test_firmware_logs_no_token_characters(self) -> None:
        # No log line or console print formats a token (or part of one): only
        # token_fp_text() (length and SHA-256 prefix) or lengths.
        secret = re.compile(r"\b(s_sdk_token|sdk_token|identity_sdk_token\(\)|token|access_token|refresh_token|"
                            r"access|refresh|raw|tok|paired_sdk_hint|hex|key)\b")
        log = re.compile(r"(ESP_LOG[EWIDV]|printf)\s*\((.*)\);")
        bad = []
        for path in list((ROOT / "main").glob("*.c")) + list((ROOT / "main").glob("*.cpp")) + \
                list((ROOT / "components").glob("muse*/**/*.c")):
            text = path.read_text(errors="replace")
            for m in log.finditer(text):
                call = m.group(2)
                fmt = re.search(r'"((?:[^"\\]|\\.)*)"', call)
                if not fmt or "%" not in fmt.group(1):
                    continue
                args = call[fmt.end():]
                if re.search(r"%\.\d+s|%\.\*s", fmt.group(1)) and secret.search(args):
                    bad.append(f"{path.relative_to(ROOT)}: {m.group(0)[:120]}")
                elif re.search(r"\b(s_sdk_token|identity_sdk_token\(\)|paired_sdk_hint)\b", args) \
                        and "token_fp_text" not in args:
                    bad.append(f"{path.relative_to(ROOT)}: {m.group(0)[:120]}")
        self.assertEqual(bad, [])
        for rel in ("main/link_pairing.c", "main/app.c", "main/sdk_token_console.c", "main/muse_glue.c"):
            self.assertIn("token_fp_text(", (ROOT / rel).read_text(), rel)


    def test_nothing_dumps_memory_to_the_console(self) -> None:
        # A security check (from a gadget: a closed
        # update library hexdumped the saved settings, device token included,
        # during a BLE OTA): no firmware code dumps buffers or memory to the
        # console, no build turns on debug logs (NimBLE's DEBUG prints ATT
        # payloads) or a core dump to the UART. The ESP-Mosaico's core dump
        # goes to its retained flash partition for Vibe Mode, never the console.
        dump = re.compile(r"\b(ESP_LOG_BUFFER_\w+|esp_log_buffer_\w+|ESP_LOG_LEVEL_LOCAL|hexdump|LOG_HEXDUMP_\w+)\s*\(")
        bad = []
        sources = list((ROOT / "main").glob("*.c")) + list((ROOT / "main").glob("*.cpp")) + \
            list((ROOT / "components").glob("muse*/**/*.c")) + list((ROOT.parent / "zephyr" / "src").glob("*.c"))
        for path in sources:
            for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
                if dump.search(line):
                    bad.append(f"{path.name}:{n}: {line.strip()[:100]}")
        self.assertEqual(bad, [])
        noisy = re.compile(r"^CONFIG_(ESP_COREDUMP_ENABLE_TO_UART|BT_NIMBLE_LOG_LEVEL_DEBUG|LOG_DEFAULT_LEVEL_DEBUG|"
                           r"LOG_DEFAULT_LEVEL_VERBOSE|LOG_MAXIMUM_LEVEL_DEBUG|LOG_MAXIMUM_LEVEL_VERBOSE|"
                           r"BOOTLOADER_LOG_LEVEL_DEBUG|BOOTLOADER_LOG_LEVEL_VERBOSE)=y", re.M)
        zephyr_noisy = re.compile(r"^CONFIG_(\w+_LOG_LEVEL_DBG|BT_DEBUG\w*|LOG_DEFAULT_LEVEL=4)=?y?", re.M)
        overlays = list(ROOT.glob("sdkconfig.defaults*")) + list((ROOT / "devices").glob("sdkconfig.*"))
        for path in overlays:
            for m in noisy.finditer(path.read_text(errors="replace")):
                bad.append(f"{path.name}: {m.group(0)}")
        z = ROOT.parent / "zephyr"
        for path in [z / "prj.conf"] + list((z / "boards").glob("*.conf")) + list(z.glob("overlay-*.conf")):
            for m in zephyr_noisy.finditer(path.read_text(errors="replace")):
                bad.append(f"{path.name}: {m.group(0)}")
        self.assertEqual(bad, [])

    def test_flash_backups_and_images_are_private(self) -> None:
        # Every flash backup or dump is created 0600 in a 0700
        # directory (on Windows, a folder only the user can read), never in a
        # repo or an artifact.
        self.assertIn("os.umask(0o077)", (ROOT / "tools" / "muse" / "paced_esptool.py").read_text())
        self.assertIn("umask 077", (ROOT / "tools" / "muse" / "board.sh").read_text())
        self.assertIn("0o600", (ROOT.parent / "zephyr" / "tools" / "flash_hex.py").read_text())
        for doc in [ROOT / "devices" / "README.md"] + list((ROOT / "devices").glob("*.md")):
            text = doc.read_text()
            for line in text.splitlines():
                if "read-flash" in line and ("esptool" in line):
                    posix = line.startswith("(umask 077; ") and '"$BK/' in line
                    windows = '"$BK\\' in line and "icacls $BK /inheritance:r" in text
                    self.assertTrue(posix or windows, f"{doc.name}: {line}")

    def test_ble_console_reveals_no_secret(self) -> None:
        # Anyone in BLE range reaches the
        # console while no phone is connected, so no console command prints a
        # token, a Link credential or a key, or reads raw flash, RAM or NVS.
        # Pin the whole command surface: a new command must be reviewed here.
        ble = (ROOT / "components" / "muse_gadget_ble" / "mg_ble.c").read_text()
        nus = ble[ble.index("static void nus_command(char *line)\n{"):]
        nus = nus[:nus.index("\n}\n")]
        # !SILENCE: prints nothing but on/not on this build; it turns
        # advertising off until a restart, to prove the stay-reachable check.
        # !SETUP ADV: prints on/off/not on this build; Link
        # setup advertised again for 10 min, RAM only. Open (no owner unlock).
        self.assertEqual(sorted(set(re.findall(r'strcmp\(line, "([^"]+)"\)', nus))),
                         ["!SAFEID", "!SETUP ADV", "!SETUP ADV 0", "!SILENCE", "status", "version"])
        self.assertIn("bool ok = s_platform.bench_setup_adv != NULL;", nus)
        self.assertIn("bool ok = s_platform.bench_silence != NULL;", nus)
        self.assertIn("bool ok = s_platform.bench_safe_identity != NULL;", nus)
        self.assertNotIn("s_unlock.unlocked", nus)
        self.assertNotIn("unlock first", nus)
        glue = (ROOT / "main" / "mg_glue.c").read_text()
        self.assertIn('static const char *const allowed[] = { "ui.md5=", "ui.lat=", "video.itvl=" };', glue)
        # SAFE's console: safe.off and safe=0, the same action (Muse on
        # with a planned restart in SAFE), and guard? (the boot guard's counts only), through
        # the boot guard's word handler, as on USB; nothing else.
        safe = glue[glue.index("static bool safe_console(char *line) {"):]
        safe = safe[:safe.index("\n}\n")]
        # A wireless line is untrusted: an unproven safe.off leaves SAFE but keeps the loss count.
        self.assertIn("if (!muse_bootguard_console_word_from(line, out, sizeof(out), false)) return false;", safe)
        word = (ROOT / "components" / "muse_restart" / "muse_bootguard.c").read_text()
        word = word[word.index("bool muse_bootguard_console_word_from("):]
        word = word[:word.index("\n}\n")]
        self.assertEqual(sorted(set(re.findall(r'strcmp\(line, "([^"]+)"\)', word))), ["guard?", "safe.off", "safe=0"])
        self.assertNotIn("token", word)
        self.assertEqual(glue.count('if (!strcmp(line, "guard?")) {'), 1)   # ble_console's
        # ota?: read-only, slots, states and the bootloader's rollback verdict.
        self.assertEqual(glue.count('if (!strcmp(line, "ota?")) {'), 2)
        # The SMP server (the SMP core, vendored): OS and image groups only.
        smp = (ROOT / "components" / "muse_gadget_ble" / "smp" / "mg_smp.c").read_text()
        self.assertEqual(sorted(set(re.findall(r"group == (GRP_\w+)", smp))), ["GRP_IMG", "GRP_OS"])
        z = ROOT.parent / "zephyr"
        znus = (z / "src" / "mg_nus.c").read_text()
        # pins: CONFIG_MG_AUTOMATION (the automation tools, bench builds only), every GPIO port's input
        # levels as three hex words; no secret, no state change. listen 0|1 and mode 0|1|2
        # (every build): link modes' switch-listen A/B and the client's link mode; no secret,
        # no setup or token state. bench 0|1 (every build, over Bluetooth too): this
        # connection's mic output without a token proof, per connection, off at boot, logged,
        # the LED's bench mark; it reveals nothing. In a release build all three change state
        # only on a token-proven connection and answer "locked" otherwise (mg_nus_policy.h).
        # reach test (bench builds only; "disabled" in a release build): advertising off until
        # the next boot and the link dropped, RAM only; "unavailable" without the reachability
        # restart; it prints nothing else.
        # tx dead test (bench builds only; "disabled" in a release build): the link's controller
        # credits held (RAM only) so the watchdog resets; "unavailable" without the watchdog; it
        # prints nothing else.
        # att dead test (bench builds only; "disabled" in a release build): every link until the
        # next boot counts as ATT-dead and drops (RAM only); "unavailable" without the
        # reachability restart; it prints nothing else.
        # update window (automation and bench-mode builds; "disabled" in a release build): the
        # update prompt's press, scripted: this connection's next transfer admitted for 60 s; it
        # reveals nothing.
        # Bench mode (CONFIG_MG_BENCH_MODE only, never shipped): talk down|up, pair confirm,
        # safe next, reset next script the human steps for boards without a console UART; they
        # reveal nothing (safe/reset next restart into SAFE or the setup reset at the next boot,
        # as the button's boot hold would).
        self.assertEqual(sorted(set(re.findall(r'strcmp\(cmd, "([^"]+)"\)', znus))),
                         ["att dead test", "battery", "bench 0", "bench 1", "listen 0", "listen 1",
                          "mode 0", "mode 1", "mode 2", "pair confirm", "pins", "queue", "reach test",
                          "reset", "reset next", "safe next", "settings", "status", "talk down",
                          "talk up", "tx dead test", "update window", "version"])
        # The default-deny lock: every word classified in mg_nus_words[]; the three recovery rows
        # change state (a release build needs the proof), the bench-mode words exist only there.
        zpol = (z / "src" / "mg_nus_policy.h").read_text()
        for w in ("reach test", "tx dead test", "att dead test", "update window"):
            self.assertIn('{"%s", MG_NUS_CHANGE}' % w, zpol)
        self.assertIn('#if defined(CONFIG_MG_AUTOMATION)\n\t} else if (strcmp(cmd, "pins") == 0) {', znus)
        # Every console line passes the word policy first (the policy itself: host test_nus_policy).
        self.assertIn("if (mg_nus_word_locked(cmd, IS_ENABLED(CONFIG_MG_AUTOMATION), mg_token_proof_matched(),\n"
                      "\t\t\t       IS_ENABLED(CONFIG_MG_BENCH_MODE))) {", znus)
        for conf in [z / "prj.conf"] + list((z / "boards").glob("*.conf")) + list(z.glob("overlay-*.conf")):
            text = conf.read_text()
            for grp in ("MCUMGR_GRP_FS", "MCUMGR_GRP_SHELL", "MCUMGR_GRP_SETTINGS", "MCUMGR_GRP_STAT",
                        "MCUMGR_GRP_ZBASIC", "SHELL_BT_NUS", "SHELL_BACKEND"):
                self.assertNotRegex(text, r"(?m)^CONFIG_%s\w*=y" % grp, conf.name)

    def test_settings_screens_show_no_token_characters(self) -> None:
        # Screenshots end up in transcripts: Settings shows whether there's an
        # SDK token, its length and SHA-256 prefix (muse_link_sdk_token_fp), and
        # no snprintf anywhere in the UI formats a prefix of one.
        screens = [p for p in (ROOT / "components/muse/muse_settings_ui.c",
                               ROOT / "components/muse/eink/muse_eink_settings.cpp") if p.exists()]
        self.assertTrue(screens)
        for path in screens:
            text = path.read_text()
            with self.subTest(screen=path.name):
                self.assertIn("muse_link_sdk_token_fp(", text)
                self.assertNotIn("muse_link_sdk_token()", text)
        prefix = re.compile(r'snprintf\([^;]*"[^"]*%\.(\d+|\*)s[^"]*"[^;]*\b(sdk|token)\w*\b[^;]*\);')
        for path in (ROOT / "components/muse").rglob("*.c*"):
            self.assertIsNone(prefix.search(path.read_text(errors="replace")), path.name)
        glue = (ROOT / "main/muse_glue.c").read_text()
        self.assertIn(".sdk_token_fp = op_sdk_token_fp,", glue)
        self.assertIn("token_fp_parts(identity_sdk_token()", glue)


if __name__ == "__main__":
    unittest.main()
