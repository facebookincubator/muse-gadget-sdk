#!/usr/bin/env python3
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

"""The haptics core (haptics/mg_haptics.c) against its reference
(haptics/haptics_ref.py) and protocol/test-vectors/mg-haptic-v1.json: every
scenario replayed through the C core, built with ASan and UBSan, must give the
vectors' trace byte for byte. A board's vendored copy runs the same test:

    python3 haptics/tests/test_mg_haptics.py [CORE_DIR [MGCOMMANDS_H [VECTORS]]]
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CORE = os.path.dirname(HERE)
MGC = os.path.join(REPO, 'protocol', 'mgcommands.h')
VECTORS = os.path.join(REPO, 'protocol', 'test-vectors', 'mg-haptic-v1.json')

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_haptics.h"

static mgh_t h;
static mgh_env_t env;
static uint32_t now;

static void put(uint32_t t, const char *op, const mgh_status_t *s, unsigned n, const char *extra)
{
    printf("%u %s", t, op);
    for (unsigned i = 0; i < n; i++) printf(" 1e0f%02x%02x%02x", s[i].id, s[i].status, s[i].reason);
    if (extra) printf(" %s", extra);
    printf(" | %d\n", mgh_motor(&h));
}

static void advance(uint32_t t)
{
    while (h.active) {
        mgh_status_t s[1];
        uint32_t nxt;
        mgh_tick(&h, now, s, &nxt);
        uint64_t due = (uint64_t)now + nxt;
        if (!h.active || due > t) break;
        unsigned n = mgh_tick(&h, (uint32_t)due, s, &nxt);
        now = (uint32_t)due;
        put(now, "step", s, n, NULL);
    }
    now = t;
}

static unsigned unhex(const char *x, uint8_t *b, unsigned cap)
{
    unsigned n = 0;
    while (x[0] && x[1] && n < cap) { unsigned v; sscanf(x, "%2x", &v); b[n++] = (uint8_t)v; x += 2; }
    return n;
}

_Static_assert(sizeof(mgh_t) <= 96, "mgh_t grew");

int main(void)
{
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char op[16];
        unsigned t;
        if (sscanf(line, "%15s %u", op, &t) != 2) continue;
        if (!strcmp(op, "caps")) {
            unsigned k, f, m, st, mx;
            sscanf(line, "%*s %*u %u %u %u %u %u", &k, &f, &m, &st, &mx);
            static mgh_caps_t c;
            c.kind = (uint8_t)k; c.flags = (uint8_t)f; c.min_on_ms = (uint8_t)m; c.strength_steps = (uint8_t)st;
            c.max_on_ms = (uint16_t)mx;
            mgh_init(&h, &c);
            memset(&env, 0, sizeof env);
            now = 0;
            continue;
        }
        if (!strcmp(op, "end")) { advance(t); continue; }
        advance(t);
        mgh_status_t s[2];
        unsigned n = 0;
        char extra[64] = "";
        if (!strcmp(op, "env")) {
            unsigned a, b, c, d, e;
            sscanf(line, "%*s %*u %u %u %u %u %u", &a, &b, &c, &d, &e);
            env.enabled = (uint8_t)a; env.updating = (uint8_t)b; env.low_battery = (uint8_t)c;
            env.asleep = (uint8_t)d; env.in_turn = (uint8_t)e;
        } else if (!strcmp(op, "rx")) {
            char hx[300];
            uint8_t b[150];
            sscanf(line, "%*s %*u %299s", hx);
            unsigned len = unhex(hx, b, sizeof b);
            uint16_t err = 0;
            if (len < 2 || b[0] != 0x1E || (b[1] != mg_device_action_haptic_play && b[1] != mg_device_action_haptic_stop)) {
                err = mg_error_code_unsupported;
            } else if (b[1] == mg_device_action_haptic_stop) {
                err = mgh_parse_stop(b + 2, len - 2);
                if (!err) n = mgh_stop(&h, now, s);
            } else {
                mgh_req_t r;
                err = mgh_parse_play(h.caps, b + 2, len - 2, &r);
                if (!err) n = mgh_play(&h, &r, MGH_SRC_CLIENT, &env, now, s);
            }
            if (err) snprintf(extra, sizeof extra, "error %u", err);
        } else if (!strcmp(op, "cue")) {
            unsigned src, rep, cnt;
            int pos;
            sscanf(line, "%*s %*u %u %u %u%n", &src, &rep, &cnt, &pos);
            mgh_req_t r;
            memset(&r, 0, sizeof r);
            r.repeat = (uint8_t)rep;
            r.count = (uint8_t)cnt;
            const char *p = line + pos;
            for (unsigned k = 0; k < cnt; k++) { int m; unsigned v; sscanf(p, "%u%n", &v, &m); r.steps[k] = (uint16_t)v; p += m; }
            n = mgh_play(&h, &r, (uint8_t)src, &env, now, s);
        } else if (!strcmp(op, "stop_test")) {
            mgh_stop_test(&h, now);
        } else if (!strcmp(op, "interrupt")) {
            unsigned reason;
            sscanf(line, "%*s %*u %u", &reason);
            n = mgh_interrupt(&h, (uint8_t)reason, now, s);
        } else if (!strcmp(op, "link_drop")) {
            mgh_link_drop(&h, now);
        } else if (!strcmp(op, "duty_add")) {
            unsigned ms, ago = 0;
            sscanf(line, "%*s %*u %u %u", &ms, &ago);
            mgh_duty_add(&h, now - ago, ms);
        } else if (!strcmp(op, "used")) {
            snprintf(extra, sizeof extra, "used %u %u", mgh_duty_used(&h, now, MG_HAPTIC_DUTY_WINDOW_MS),
                     mgh_duty_used(&h, now, 60000u));
        }
        put(now, op, s, n, extra[0] ? extra : NULL);
    }
    return 0;
}
'''

SRC = {'test': 1, 'setup': 2, 'talk': 3}
PRESETS = {1: [50], 2: [50, 120, 50], 3: [400], 4: [400, 200, 400, 1000], 5: [40, 100, 120],
           6: [120, 80, 120, 80, 120]}


def script(sc):
    c = sc['caps']
    out = ['caps 0 %d %d %d %d %d' % (c['kind'], c['flags'], c['min_on_ms'], c['strength_steps'], c['max_on_ms'])]
    env = {'enabled': 0, 'updating': 0, 'low_battery': 0, 'asleep': 0, 'in_turn': 0}
    last = 0
    for ev in sc['events']:
        t, op = ev['t'], ev['op']
        last = t
        if op == 'env':
            env.update({k: ev[k] for k in env if k in ev})
            out.append('env %d %d %d %d %d %d' % (t, env['enabled'], env['updating'], env['low_battery'], env['asleep'],
                                                  env['in_turn']))
        elif op == 'rx':
            out.append('rx %d %s' % (t, ev['hex']))
        elif op == 'cue':
            steps = PRESETS[ev['preset']] if 'preset' in ev else ev['steps']
            out.append('cue %d %d %d %d %s' % (t, SRC[ev['src']], ev.get('repeat', 1), len(steps),
                                              ' '.join(map(str, steps))))
        elif op in ('interrupt',):
            out.append('interrupt %d %d' % (t, ev['reason']))
        elif op == 'duty_add':
            out.append('duty_add %d %d %d' % (t, ev['ms'], ev.get('ago', 0)))
        else:
            out.append('%s %d' % (op, t))
    out.append('end %d' % ((last + 11000) & 0xFFFFFFFF))
    return out


def expected(sc):
    lines = []
    for t, op, outs, motor in sc['trace']:
        lines.append(' '.join(['%d %s' % (t, op)] + outs) + ' | %d' % motor)
    return lines


class Core(unittest.TestCase):
    core, mgc, vectors = CORE, MGC, VECTORS

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='mgh-')
        d = cls.tmp.name
        with open(os.path.join(d, 'h.c'), 'w') as f:
            f.write(HARNESS)
        cmd = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               '-DMGH_MGCOMMANDS_H="%s"' % cls.mgc, '-I', cls.core,
               os.path.join(d, 'h.c'), os.path.join(cls.core, 'mg_haptics.c'), '-o', os.path.join(d, 'h')]
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        cls.exe = os.path.join(d, 'h')
        with open(cls.vectors) as f:
            cls.data = json.load(f)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_every_scenario_matches_the_vectors(self):
        self.assertGreaterEqual(len(self.data['scenarios']), 70)
        for sc in self.data['scenarios']:
            with self.subTest(sc['name']):
                got = subprocess.run([self.exe], input='\n'.join(script(sc)) + '\n', capture_output=True, text=True,
                                     check=True).stdout.strip().split('\n')
                got = [g.replace(' ', ' ').strip() for g in got if g.strip()]
                self.assertEqual(got, expected(sc))

    def test_the_vectors_are_the_references(self):
        if not os.path.exists(os.path.join(CORE, 'haptics_ref.py')):
            self.skipTest('a vendored copy: the reference model is not in this tree')
        sys.path.insert(0, CORE)
        import haptics_ref
        self.assertEqual(json.loads(json.dumps(haptics_ref.build())), self.data)


if __name__ == '__main__':
    if len(sys.argv) > 1:
        Core.core = os.path.abspath(sys.argv[1])
    if len(sys.argv) > 2:
        Core.mgc = os.path.abspath(sys.argv[2])
    if len(sys.argv) > 3:
        Core.vectors = os.path.abspath(sys.argv[3])
    unittest.main(argv=sys.argv[:1])
