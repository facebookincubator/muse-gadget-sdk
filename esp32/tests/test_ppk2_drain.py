# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Reader cadence regressions; no serial device is opened."""
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.power.ppk2_session import PPK2Session, SafetyAcknowledgment


class DrainCadenceTest(unittest.TestCase):
    def session(self):
        return PPK2Session(port="fake", voltage_mv=3872,
                           acknowledgment=SafetyAcknowledgment(True, True, True, True))

    def test_busy_reads_do_not_sleep(self):
        session = self.session()
        session._stop = mock.Mock()
        session._stop.is_set.side_effect = [False, False, False, True]
        session.pump = mock.Mock()
        session._run()
        self.assertEqual(session.pump.call_count, 3)
        session._stop.wait.assert_not_called()

    def test_only_empty_reads_yield(self):
        session = self.session()
        session._stop = mock.Mock()
        session._stop.is_set.side_effect = [False, False, False, True]

        def empty():
            session.empty_reads += 1

        calls = [0]
        def pump():
            calls[0] += 1
            if calls[0] == 3:
                empty()
        session.pump = mock.Mock(side_effect=pump)
        session._run()
        session._stop.wait.assert_called_once_with(0.0002)

    def test_startup_reasserts_stop_when_first_command_did_not_quiesce(self):
        now = [0.0]
        session = PPK2Session(port="fake", voltage_mv=3872,
                              acknowledgment=SafetyAcknowledgment(True, True, True, True),
                              clock=lambda: now[0], sleep=lambda seconds: now.__setitem__(0, now[0] + seconds))
        session.backend = mock.Mock()
        session.backend.read.side_effect = lambda: b"old" if session.backend.stop_measuring.call_count < 2 else b""
        session.backend.stop_measuring()
        session._stale_drain()
        self.assertEqual(session.backend.stop_measuring.call_count, 2)
        self.assertLess(now[0], 0.3)
        session.backend.power.assert_not_called()
        session.backend.start_measuring.assert_not_called()

    def test_unresponsive_startup_is_still_bounded_and_never_enables_output(self):
        now = [0.0]
        session = PPK2Session(port="fake", voltage_mv=3872, stale_timeout_s=0.3,
                              acknowledgment=SafetyAcknowledgment(True, True, True, True),
                              clock=lambda: now[0], sleep=lambda seconds: now.__setitem__(0, now[0] + seconds))
        session.backend = mock.Mock()
        session.backend.read.return_value = b"old"
        session.backend.stop_measuring()
        with self.assertRaises(TimeoutError):
            session._stale_drain()
        self.assertLessEqual(session.backend.stop_measuring.call_count, 4)
        session.backend.power.assert_not_called()
        session.backend.start_measuring.assert_not_called()

    def test_faulted_drain_yields_but_never_closes_source(self):
        session = self.session()
        session._stop = mock.Mock()
        session._stop.is_set.side_effect = [False, True]
        session.fault = "fake output failure"
        session.pump = mock.Mock()
        session.backend = mock.Mock()
        session._run()
        session._stop.wait.assert_called_once_with(0.02)
        session.backend.power.assert_not_called()
        session.backend.close.assert_not_called()


if __name__ == "__main__":
    unittest.main()
