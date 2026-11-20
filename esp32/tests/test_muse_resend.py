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

"""When a voice note that didn't reach Muse is sent again
(CONFIG_MUSE_RESEND_NOTES): muse_resend.c's rules on the host, and how the
session and the build wire them. test_muse_ptt_ui.py runs muse_voice.c with
them."""

import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / 'components/muse'
ENV = {**os.environ,
       'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
       'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include "muse_resend.h"

int main(int argc, char **argv)
{
    const char *what = argv[1];
    if (what[0] == 'f') {          /* failure STATUS BODY_SENT */
        printf("%d\n", muse_resend_failure(atoi(argv[2]), atoi(argv[3])));
    } else if (what[0] == 'o') {   /* outcome REPORTED FED */
        printf("%d\n", muse_resend_outcome((muse_chat_fail_t)atoi(argv[2]), atoi(argv[3])));
    } else if (what[0] == 'd') {   /* delay TRIES */
        printf("%lld\n", (long long)muse_resend_delay_us(atoi(argv[2])));
    } else {                       /* room MAX_COUNT MAX_BYTES BYTES SIZE... */
        size_t sizes[16];
        int count = argc - 5;
        for (int i = 0; i < count; i++) sizes[i] = strtoul(argv[5 + i], NULL, 10);
        printf("%d\n", muse_resend_make_room(sizes, count, atoi(argv[2]), strtoul(argv[3], NULL, 10),
                                             strtoul(argv[4], NULL, 10)));
    }
    return 0;
}
'''

NONE, RETRY, REFUSED, UNKNOWN = range(4)


class ResendRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        driver = Path(cls.tmp.name) / 'driver.c'
        driver.write_text(DRIVER)
        cls.binary = Path(cls.tmp.name) / 'resend'
        result = subprocess.run(
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
             '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-I', str(MUSE), str(driver), str(MUSE / 'muse_resend.c'), '-o', str(cls.binary)],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_rule(self, *args):
        ran = subprocess.run([str(self.binary), *map(str, args)], capture_output=True, text=True, env=ENV)
        self.assertEqual(ran.returncode, 0, ran.stderr)
        return int(ran.stdout)

    def test_refusals_are_4xx_but_408_and_429_whenever_they_come(self):
        for status in (400, 401, 403, 404, 413, 422, 499):
            for body_sent in (0, 1):
                self.assertEqual(self.run_rule('failure', status, body_sent), REFUSED, status)
        for status in (408, 429, 500, 502, 503):
            self.assertEqual(self.run_rule('failure', status, 0), RETRY, status)

    def test_a_note_that_went_up_whole_is_never_sent_twice(self):
        # No ack, or a server error after the last chunk: Muse may have it.
        for status in (0, 200, 408, 429, 500, 503):
            self.assertEqual(self.run_rule('failure', status, 1), UNKNOWN, status)
        self.assertEqual(self.run_rule('failure', 0, 0), RETRY)

    def test_voice_task_outcome_keeps_the_report_or_assumes_the_worst(self):
        for reported in (RETRY, REFUSED, UNKNOWN):
            for fed in (0, 1):
                self.assertEqual(self.run_rule('outcome', reported, fed), reported)
        self.assertEqual(self.run_rule('outcome', NONE, 0), RETRY)
        self.assertEqual(self.run_rule('outcome', NONE, 1), UNKNOWN)

    def test_tries_wait_5_15_30_then_60_seconds(self):
        waits = [self.run_rule('delay', tries) // 1000000 for tries in range(0, 12)]
        self.assertEqual(waits, [5, 5, 15, 30] + [60] * 8)

    def test_room_drops_the_oldest_by_count_and_by_bytes(self):
        room = lambda max_count, max_bytes, new, *sizes: self.run_rule('room', max_count, max_bytes, new, *sizes)
        self.assertEqual(room(3, 1000, 100), 0)
        self.assertEqual(room(3, 1000, 100, 100, 100), 0)
        self.assertEqual(room(3, 1000, 100, 100, 100, 100), 1)        # by count
        self.assertEqual(room(3, 1000, 500, 400, 300), 1)             # by bytes
        self.assertEqual(room(3, 1000, 1000, 400, 300), 2)            # everything, then it fits alone
        self.assertEqual(room(3, 1000, 1001, 400), -1)                # too big even alone
        self.assertEqual(room(1, 1000, 10, 10), 1)
        self.assertEqual(room(0, 1000, 10), -1)


class ResendWiring(unittest.TestCase):
    def test_option_is_opt_in_and_bounded(self):
        kconfig = (MUSE / 'Kconfig').read_text()
        block = kconfig.split('config MUSE_RESEND_NOTES\n', 1)[1].split('\n    config ', 1)[0]
        self.assertIn('depends on MUSE_HATCH', block)
        self.assertIn('default n', block)
        for name, low, high in (('MUSE_RESEND_NOTES_MAX', 1, 8), ('MUSE_RESEND_NOTES_KB', 512, 4096)):
            option = kconfig.split(f'config {name}\n', 1)[1].split('\n    config ', 1)[0]
            self.assertIn('depends on MUSE_RESEND_NOTES', option)
            self.assertIn(f'range {low} {high}', option)
        # The longest note (15 s of 16 kHz PCM16) always fits the smallest budget.
        self.assertLessEqual(15 * 16000 * 2, 512 * 1024)

    def test_only_builds_with_the_option_compile_the_rules(self):
        cmake = (MUSE / 'CMakeLists.txt').read_text()
        hatch = cmake.split('if(CONFIG_MUSE_HATCH)', 1)[1].split('else()', 1)[0]
        self.assertRegex(hatch, r'if\(CONFIG_MUSE_RESEND_NOTES\)\s+list\(APPEND srcs "muse_resend.c"\)')
        self.assertEqual(cmake.count('muse_resend.c'), 1)

    def test_session_reports_when_the_whole_note_went_up(self):
        # note_up turns on only once the last chunk is out, and the request's
        # status is kept before the turn fails, so the voice task learns
        # whether Muse may already have the note.
        session = (MUSE / 'muse_chat_session.cpp').read_text()
        record = session[session.index('static bool record_note('):session.index('static void send_chat(')]
        last = record.index('if (!send_note_part(true)) {')
        self.assertGreater(record.index('s_turn.note_up = true;'), last)
        self.assertEqual(session.count('s_turn.note_up = true;'), 1)
        chat_end = session[session.index('    case K_CHAT:\n        if (ok)'):]
        chat_end = chat_end[:chat_end.index('break;')]
        self.assertLess(chat_end.index('s_turn.note_status = s->status;'),
                        chat_end.index('turn_fail("MUSE DIDN\'T TAKE IT");'))
        self.assertIn('ev.fail = muse_resend_failure(s_turn.note_status, s_turn.note_up);', session)
        # A new turn forgets the last one's failure; turn_start zeroes the turn.
        begin = session[session.index('extern "C" void muse_hatch_turn_begin(void)'):]
        self.assertIn('s_last_fail = MUSE_CHAT_FAIL_NONE;', begin[:begin.index('}')])
        self.assertIn('s_turn = turn_t{};', session)

    def test_everything_is_behind_the_option(self):
        # Builds without it are unchanged: each use sits inside its #if.
        for name in ('muse_chat_session.cpp', 'muse_voice.c'):
            source = (MUSE / name).read_text()
            depth, guarded = [], []
            for line in source.splitlines():
                if line.startswith('#if'):
                    depth.append('CONFIG_MUSE_RESEND_NOTES' in line and '!' not in line)
                elif line.startswith('#else'):
                    depth[-1] = False
                elif line.startswith('#endif'):
                    depth.pop()
                elif re.search(r'muse_resend_|muse_chat_turn_failure|note_up|note_status|MAY_HAVE_IT', line):
                    guarded.append((any(depth), line.strip()))
            self.assertTrue(guarded, name)
            for inside, line in guarded:
                self.assertTrue(inside, f'{name}: {line}')


if __name__ == '__main__':
    unittest.main()
