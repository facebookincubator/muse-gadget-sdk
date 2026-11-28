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

"""tools/stuck_watch.py, the freeze watchdog the host tests run under.

The first case is a real freeze: a parent blocked writing a driver's stdin while the driver is blocked writing its
stdout, both idle with no output. The fake below hangs that way on every OS (it writes without reading)."""

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WATCH = ROOT / "tools" / "stuck_watch.py"
sys.path.insert(0, str(ROOT / "tools"))
import stuck_watch  # noqa: E402

DEADLOCK = r"""
import subprocess, sys
child = subprocess.Popen([sys.executable, "-c",
    "import sys\nline = sys.stdin.readline()\nsys.stdout.write('x' * 4000000)\nsys.stdout.flush()\nsys.stdin.read()"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE)
child.stdin.write(b"go\n" + b"y" * 4000000)   # blocks: the child is blocked writing an answer nobody reads
"""

HOOK = r"""
import os, sys
with open(os.environ["HOOK_OUT"], "a") as f:
    f.write(sys.argv[-1] + "\n")
"""

PASSING = "import unittest\nclass T(unittest.TestCase):\n    def test(self): pass\n"


class StuckWatch(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / "hook.py").write_text(HOOK)
        self.env = dict(os.environ, STUCK_WATCH_DIR=str(self.d / "state"),
                        STUCK_WATCH_NOTIFY=f"{sys.executable} {self.d / 'hook.py'}", HOOK_OUT=str(self.d / "told"))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def seed(self, suite: str, seconds: float, n: int) -> None:
        (self.d / "state").mkdir(exist_ok=True)
        with open(self.d / "state" / f"{suite}.jsonl", "a") as f:
            for i in range(n):
                f.write(json.dumps({"suite": suite, "start": 1000 + i, "seconds": seconds, "rc": 0, "result": "PASS",
                                    "tool": "stuck_watch"}) + "\n")

    def watch(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(WATCH), *args], env=self.env, capture_output=True, text=True,
                              timeout=180)

    def records(self, suite: str) -> list:
        return [json.loads(line) for line in (self.d / "state" / f"{suite}.jsonl").read_text().splitlines()]

    def test_a_pipe_deadlock_is_stuck_captured_stopped_and_reported(self) -> None:
        self.seed("deadlock", 0.5, 4)   # 4 passing runs: still the cap
        (self.d / "deadlock.py").write_text(DEADLOCK)
        r = self.watch("--suite", "deadlock", "--cap", "1.5", "--", sys.executable, str(self.d / "deadlock.py"))
        self.assertEqual(r.returncode, 124, r.stdout + r.stderr)
        self.assertTrue(r.stdout.strip().splitlines()[-1].startswith("STUCK: deadlock after "), r.stdout)
        self.assertNotIn("PASS", r.stdout)
        self.assertEqual(self.records("deadlock")[-1]["result"], "STUCK")
        self.assertIsNone(stuck_watch.typical(str(self.d / "state"), "deadlock"))   # STUCK never counts
        self.seed("deadlock", 0.5, 1)
        self.assertEqual(stuck_watch.typical(str(self.d / "state"), "deadlock"), 0.5)
        cap = next((self.d / "state").glob("deadlock-STUCK-*.log")).read_text()
        procs = cap.split("-- processes (its group: CPU, state, elapsed) --\n")[1].split("\n-- idle --")[0]
        self.assertIn("deadlock.py", procs)   # the parent, and the driver it feeds
        self.assertIn(" -c ", procs)
        self.assertIn("-- idle --\nno output for ", cap)
        self.assertIn("-- what the group has open (lsof -g) --", cap)
        msg = (self.d / "told").read_text().strip()
        self.assertTrue(msg.startswith("STUCK (not a pass): deadlock on "), msg)
        # Stopped by its TERM: a zombie left behind (a container init that doesn't reap) never turns it into a KILL.
        self.assertRegex(msg, r"stopped process group \d+; ")
        pgid = int(re.search(r"stopped process group (\d+); ", msg).group(1))
        self.assertEqual(stuck_watch.group_pids(pgid), [])   # nothing of its group is left

    def test_pass_and_fail_are_recorded_and_no_hook_is_a_no_op(self) -> None:
        del self.env["STUCK_WATCH_NOTIFY"]
        r = self.watch("--suite", "quick", "--", sys.executable, "-c", "print('hello')")
        self.assertEqual((r.returncode, r.stdout), (0, "hello\n"))
        r = self.watch("--suite", "quick", "--", sys.executable, "-c", "raise SystemExit(3)")
        self.assertEqual(r.returncode, 3)
        self.assertEqual([x["result"] for x in self.records("quick")], ["PASS", "FAIL"])
        r = self.watch("--suite", "sleep", "--cap", "1", "--", sys.executable, "-c", "import time; time.sleep(60)")
        self.assertEqual(r.returncode, 124)
        self.assertFalse((self.d / "told").exists())

    def test_a_group_that_ignores_sigterm_gets_sigkill_after_the_grace(self) -> None:
        child = subprocess.Popen([sys.executable, "-c", "import signal, sys, time; "
                                  "signal.signal(signal.SIGTERM, signal.SIG_IGN); print('ready', flush=True); "
                                  "time.sleep(60)"], stdout=subprocess.PIPE, text=True, start_new_session=True)
        with child:
            self.assertEqual(child.stdout.readline().strip(), "ready")   # SIGTERM is ignored from here
            with unittest.mock.patch.object(stuck_watch, "GRACE_S", 1):
                said = stuck_watch.stop_group(child, child.pid)
        self.assertEqual(said, f"stopped process group {child.pid} (SIGKILL after the grace)")
        self.assertEqual(child.returncode, -9)
        self.assertEqual(stuck_watch.group_pids(child.pid), [])

    def test_unittest_runs_one_module_at_a_time(self) -> None:
        mods = self.d / "mods"
        mods.mkdir()
        (mods / "test_a.py").write_text(PASSING)
        (mods / "test_b.py").write_text(PASSING.replace("pass", "self.fail()"))
        (mods / "helper.py").write_text("")
        # A failed setUpClass: Python 3.12's unittest exits 5 ("no tests ran") though it printed FAILED; a failure.
        (mods / "test_d.py").write_text("import unittest\nclass T(unittest.TestCase):\n    @classmethod\n"
                                        "    def setUpClass(cls): raise RuntimeError('broken')\n"
                                        "    def test(self): pass\n")
        r = self.watch("--", sys.executable, "-m", "unittest", "discover", "-s", str(mods), "-p", "test_*.py", "-v")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("stuck_watch: 3 modules, 2 failed, 0 STUCK (failed: test_b test_d)", r.stdout)
        # A STUCK module makes the whole run 124, whatever else failed.
        (mods / "test_c.py").write_text(PASSING.replace("pass", "__import__('time').sleep(60)"))
        r = self.watch("--cap", "10", "--", sys.executable, "-m", "unittest", "discover", "-s", str(mods), "-p",
                       "test_*.py")
        self.assertEqual(r.returncode, 124, r.stdout + r.stderr)
        self.assertIn("2 failed, 1 STUCK (failed: test_b test_d) (STUCK: test_c)", r.stdout)
        self.assertEqual(self.records("esp32-host-test_a")[-1]["result"], "PASS")
        self.assertEqual(self.records("esp32-host-test_b")[-1]["result"], "FAIL")
        self.env["PYTHONPATH"] = str(mods)
        r = self.watch("--", sys.executable, "-m", "unittest", "test_a")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


    def test_a_group_that_empties_at_the_end_of_the_grace_gets_no_sigkill(self) -> None:
        child = subprocess.Popen([sys.executable, "-c", "pass"], start_new_session=True)
        child.wait()
        child.returncode = None   # as an unreaped leader would look
        self.addCleanup(setattr, child, "returncode", 0)
        end = stuck_watch.time.monotonic() + 0.3   # alive through the grace, empty just as it ends
        with unittest.mock.patch.object(stuck_watch, "GRACE_S", 0.3), \
                unittest.mock.patch.object(stuck_watch, "group_alive",
                                           side_effect=lambda pg: stuck_watch.time.monotonic() < end), \
                unittest.mock.patch.object(stuck_watch.os, "killpg") as killpg:
            child.wait = lambda timeout=None: 0
            said = stuck_watch.stop_group(child, child.pid)
        self.assertEqual(said, f"stopped process group {child.pid}")
        self.assertNotIn(unittest.mock.call(child.pid, stuck_watch.signal.SIGKILL), killpg.call_args_list)

    def test_ps_decides_only_for_a_group_it_lists(self) -> None:
        header = "  PID  PPID  PGID %CPU STAT ELAPSED ARGS"
        with unittest.mock.patch.object(stuck_watch.os, "killpg"):
            for out, alive in ((header + "\n", True),                                     # listed nowhere: kernel
                               (header + "\n   7     1    42  0.0 Z    00:01 x\n", False),  # zombies only
                               (header + "\n   7     1    42  0.0 Zl   00:01 x\n", True),   # Linux: threads live
                               (header + "\n   7     1    42  0.0 S    00:01 x\n", True),
                               ("(ps failed)\n", True)):
                with unittest.mock.patch.object(stuck_watch, "run_quiet", return_value=out):
                    self.assertEqual(stuck_watch.group_alive(42), alive, out)

    def test_waiting_without_waitid_or_kqueue_still_sees_the_exit_unreaped(self) -> None:
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(0.5)"], start_new_session=True)
        broken = unittest.mock.Mock()
        broken.kqueue.side_effect = OSError("no kqueue")
        with unittest.mock.patch.dict(stuck_watch.os.__dict__), unittest.mock.patch.object(stuck_watch, "select",
                                                                                         broken):
            stuck_watch.os.__dict__.pop("waitid", None)
            self.assertTrue(stuck_watch.wait_unreaped(child, 20))
            self.assertIsNone(child.returncode)   # seen exited, not reaped
        self.assertEqual(child.wait(), 0)

if __name__ == "__main__":
    unittest.main()
