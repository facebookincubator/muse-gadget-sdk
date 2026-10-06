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

"""The session's side of speaking replies with Home Assistant (CONFIG_HA_TTS):
the production start_tts .. check_turn, built on the host against a fake fetch
and a fake clock, with the real MP3 decoder on components/muse/test_reply.mp3.
It covers how a turn's cap, the stall watchdog and the fallbacks to reading
pace behave as HA succeeds, stalls, fails, or is still busy with an old fetch."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SESSION = ROOT / "components/muse/muse_chat_session.cpp"
MP3 = ROOT / "components/muse/test_reply.mp3"

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "host_compat.h"
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "muse_chat_priv.h"
extern "C" {
#include "muse_ha_tts.h"
#include "muse_ha_tts_text.h"
}

#define CONFIG_HA_TTS 1
static std::string s_log;   /* every warning, for the checks */
static void log_line(const char *level, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (level[0] == 'W') {
        s_log += line;
        s_log += '\n';
    }
}
#define ESP_LOGI(tag, ...) log_line("I", __VA_ARGS__)
#define ESP_LOGW(tag, ...) log_line("W", __VA_ARGS__)
[[maybe_unused]] static const char *TAG = "test";

/* The clock, the reply buffer and the rest of the session around the slice. */
static int64_t s_now;
static int64_t now_us() { return s_now; }
typedef void *StreamBufferHandle_t;
static StreamBufferHandle_t s_out;
/* The reply buffer holds 2 s (OUT_BYTES) and the speaker drains it in real
 * time, so reading pace takes as long as it does on the board. */
static int64_t s_out_level;
static bool s_out_stuck;            /* the speaker takes nothing */
static uint64_t s_played;
static size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t) { return s_out_stuck ? 0 : 64000 - s_out_level; }
static size_t xStreamBufferSend(StreamBufferHandle_t, const void *, size_t n, int)
{
    s_out_level += n;
    s_played += n / 2;
    return n;
}
static std::atomic<uint32_t> s_gen{0};
static int16_t *s_pcm, *s_pcm16;
enum mark_t { M_RELEASE, M_SENT, M_ACK, M_TEXT, M_DONE, M_TTS, M_MP3, M_AUDIO };
static void mark(mark_t) {}
static void log_marks() {}
static void on_dictation_end(bool) {}
struct stream_t { int msg; };
static void close_stream(stream_t *) {}
static bool s_speaker = true;
extern "C" bool muse_settings_speaker_on(void) { return s_speaker; }
'''

AFTER_TYPES = r'''
static turn_t s_turn;
static int s_done, s_failed;
static bool s_complete;
static void turn_done(bool complete) { s_done++; s_complete = complete; s_turn.phase = P_IDLE; }
static void turn_fail(const char *) { s_failed++; s_turn.phase = P_IDLE; }
static void resampler_init(resampler_t *, int, int) {}
static size_t resample(resampler_t *, const int16_t *in, size_t n, int16_t *out)
{
    memcpy(out, in, n * sizeof(int16_t));
    return n;
}
static void show_reply_start(const msg_t &) {}

/* The fetch, as the session sees it through muse_ha_tts.h. */
static muse_ha_tts_state_t s_ha = MUSE_HA_TTS_IDLE;
static std::vector<uint8_t> s_ha_data;
static size_t s_ha_read, s_ha_bytes;
static int s_fetches, s_cancels;
extern "C" bool muse_ha_tts_fetch(const char *text)
{
    if (s_ha == MUSE_HA_TTS_RUNNING || !text || !text[0]) return false;
    s_fetches++;
    s_ha = MUSE_HA_TTS_RUNNING;
    s_ha_data.clear();
    s_ha_read = s_ha_bytes = 0;
    return true;
}
extern "C" size_t muse_ha_tts_read(void *buf, size_t cap)
{
    size_t n = s_ha_data.size() - s_ha_read < cap ? s_ha_data.size() - s_ha_read : cap;
    memcpy(buf, s_ha_data.data() + s_ha_read, n);
    s_ha_read += n;
    return n;
}
extern "C" muse_ha_tts_state_t muse_ha_tts_state(void) { return s_ha; }
extern "C" size_t muse_ha_tts_bytes(void) { return s_ha_bytes; }
extern "C" void muse_ha_tts_cancel(void) { s_cancels++; }
extern "C" void muse_ha_tts_start(void) {}
static void ha_send(const std::vector<uint8_t> &d, size_t from, size_t to)
{
    s_ha_data.insert(s_ha_data.end(), d.begin() + from, d.begin() + to);
    s_ha_bytes += to - from;
}
'''

DRIVER = r'''
#define S(x) ((int64_t)((x) * 1000000.0))
static std::vector<uint8_t> s_mp3;
static uint8_t *s_mp3_buf;

/* A voice turn that began at 0 and whose one reply is complete at `at`. */
static void reply_at(double at, const char *text = "The kitchen light is on, and the door is locked.")
{
    for (auto &sp : s_turn.speech) muse_ha_tts_speech_free(&sp, free);
    s_turn = turn_t{};
    s_turn.mp3 = s_mp3_buf;
    s_turn.tts_msg = -1;
    s_turn.phase = P_WAIT_REPLY;
    s_now = S(at);
    s_turn.last_event_us = s_turn.last_content_us = s_now;
    msg_t &m = s_turn.msgs[0];
    strlcpy(m.id, "reply", sizeof(m.id));
    m.len = strlen(text);
    m.done = true;
    m.tts = TTS_QUEUED;
    s_turn.nmsgs = 1;
    assert(muse_ha_tts_speech_append(&s_turn.speech[0], text, 8192, realloc));
    s_done = s_failed = s_fetches = s_cancels = 0;
    s_complete = false;
    s_played = 0;
    s_out_level = 0;
    s_out_stuck = false;
    s_log.clear();
}

/* One pass of the session task's loop, as in hatch_task. */
static void step(double dt = 0.01)
{
    s_now += S(dt);
    if (!s_out_stuck) {
        s_out_level -= S(dt) * 32 / 1000;   /* 16 kHz, 2 bytes a sample */
        if (s_out_level < 0) s_out_level = 0;
    }
    if (s_turn.phase != P_WAIT_REPLY) return;
    start_tts();
    decode();
    check_turn();
}
static void run_to(double until) { while (s_now < S(until) && s_turn.phase == P_WAIT_REPLY) step(); }
static bool open_() { return s_turn.phase == P_WAIT_REPLY; }
static bool logged(const char *s) { return s_log.find(s) != std::string::npos; }

/* A reply that comes just before the 180 s cap is still spoken in full. */
static void late_reply_spoken()
{
    reply_at(179);
    step();
    assert(s_fetches == 1 && s_turn.ha_waiting && !s_turn.spoke);
    run_to(183);                        /* HA still rendering, past TURN_CAP_US */
    assert(open_() && !s_cancels);
    ha_send(s_mp3, 0, s_mp3.size());
    s_ha = MUSE_HA_TTS_OK;
    run_to(220);
    assert(s_done == 1 && s_complete && s_played > 0 && s_turn.spoke);
    assert(!logged("time cap") && !s_cancels);
}

/* A late reply whose fetch stalls: the stretch ends with the watchdog, and the
 * turn, past its hard cap by then, ends at once. */
static void late_reply_stall()
{
    reply_at(179);
    step();
    run_to(190);
    assert(open_() && !s_cancels);      /* inside HA_STALL_US */
    run_to(200);
    assert(s_cancels == 1 && logged("stalled"));
    assert(s_done == 1 && !s_complete && logged("time cap"));
    assert(s_now < S(179 + 15 + 1));    /* ended when the watchdog gave up, not later */
}

/* A late reply whose fetch fails before any audio: back to the 180 s cap. */
static void late_reply_failed()
{
    reply_at(179);
    step();
    run_to(181);
    assert(open_());                    /* waiting on HA, so stretched */
    s_ha = MUSE_HA_TTS_FAILED;
    step();
    step();
    assert(s_done == 1 && !s_complete && logged("time cap") && !s_turn.spoke);
}

/* An old fetch still unwinding: the reply is shown, never waits, gets no stretch. */
static void busy_falls_back()
{
    reply_at(179);
    s_ha = MUSE_HA_TTS_RUNNING;         /* left over from the turn before */
    step();
    assert(s_fetches == 0 && s_turn.silent && !s_turn.ha_waiting && logged("busy"));
    run_to(200);
    assert(s_done == 1 && !s_complete && logged("time cap"));
    assert(s_now <= S(180.02));         /* at the cap itself: no stretch */
    /* Early in a turn it's paced through to the end instead. */
    reply_at(10);
    s_ha = MUSE_HA_TTS_RUNNING;
    run_to(40);
    assert(s_done == 1 && s_complete && s_fetches == 0 && !s_turn.spoke);
}

/* A fetch that fails before any audio keeps the 180 s cap and paces the text. */
static void early_failure()
{
    reply_at(10);
    step();
    s_ha = MUSE_HA_TTS_FAILED;
    step();
    assert(s_turn.silent && !s_turn.ha_waiting && !s_turn.spoke);
    run_to(40);
    assert(s_done == 1 && s_complete);
}

/* A full decode buffer is the decoder's backlog, not a stall. */
static void full_buffer_is_not_a_stall()
{
    reply_at(10);
    step();
    ha_send(s_mp3, 0, 1000);
    step();
    assert(s_turn.spoke && !s_turn.ha_waiting);
    s_turn.mp3_len = 512 * 1024;        /* MP3_BUF: the decoder can't keep up */
    s_out_stuck = true;                 /* and the speaker isn't taking any */
    run_to(60);
    assert(open_() && !s_cancels);
}

/* Audio cut off partway: what came plays, then the rest is paced. */
static void partial_then_paced()
{
    reply_at(10, "One sentence that is spoken, then another sentence that the cut-off download never brought.");
    step();
    ha_send(s_mp3, 0, s_mp3.size() / 3);
    s_ha = MUSE_HA_TTS_FAILED;
    run_to(15);
    assert(logged("stopped partway") && s_turn.silent);
    run_to(60);
    assert(s_done == 1 && s_complete && s_played > 0);
}

/* Speaker off: nothing is fetched. */
static void speaker_off()
{
    s_speaker = false;
    reply_at(10);
    step();
    assert(s_fetches == 0 && s_turn.silent && !s_turn.ha_waiting);
    s_speaker = true;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[2], "rb");
    assert(f);
    for (int c; (c = fgetc(f)) != EOF;) s_mp3.push_back((uint8_t)c);
    fclose(f);
    s_mp3_buf = (uint8_t *)malloc(512 * 1024);
    s_pcm = (int16_t *)malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    s_pcm16 = (int16_t *)malloc((MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t));
    switch (atoi(argv[1])) {
    case 0: late_reply_spoken(); break;
    case 1: late_reply_stall(); break;
    case 2: late_reply_failed(); break;
    case 3: busy_falls_back(); break;
    case 4: early_failure(); break;
    case 5: full_buffer_is_not_a_stall(); break;
    case 6: partial_then_paced(); break;
    case 7: speaker_off(); break;
    default: return 2;
    }
    return 0;
}
'''


def between(source: str, start: str, end: str) -> str:
    return source[source.index(start):source.index(end)]


class HaTtsSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        if not cc or not cxx or shutil.which(cc[0]) is None or shutil.which(cxx[0]) is None:
            raise unittest.SkipTest("C/C++ compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = SESSION.read_text()
        code = (
            HARNESS
            + between(source, "#define MIC_RATE", "/* ---- Voice task")
            + between(source, "enum phase_t", "/* 10 KB")
            + AFTER_TYPES
            + between(source, "static void start_tts(void)", "/* ---- Inbound dispatch ---- */")
            + DRIVER
        )
        (out / "session.cpp").write_text(code)
        flags = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined",
                 "-I", str(ROOT / "tests"), "-I", str(ROOT / "components/muse"),
                 "-I", str(ROOT / "components/minimp3/include")]
        commands = [
            [*cc, "-std=gnu11", "-include", str(ROOT / "tests/host_compat.h"), *flags, "-c",
             str(ROOT / "components/muse/muse_ha_tts_text.c"), "-o", str(out / "text.o")],
            [*cxx, "-std=gnu++17", *flags, str(out / "session.cpp"), str(out / "text.o"),
             "-o", str(out / "session")],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / "session"

    def run_case(self, case: int) -> None:
        result = subprocess.run([str(self.binary), str(case), str(MP3)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_late_reply_is_spoken_past_the_cap(self) -> None:
        self.run_case(0)

    def test_late_reply_stall_ends_the_stretch_with_the_watchdog(self) -> None:
        self.run_case(1)

    def test_late_reply_failure_goes_back_to_the_hard_cap(self) -> None:
        self.run_case(2)

    def test_old_fetch_unwinding_falls_back_without_a_stretch(self) -> None:
        self.run_case(3)

    def test_early_failure_keeps_the_cap_and_paces_the_text(self) -> None:
        self.run_case(4)

    def test_full_decode_buffer_is_not_a_stall(self) -> None:
        self.run_case(5)

    def test_partial_audio_then_the_rest_is_paced(self) -> None:
        self.run_case(6)

    def test_speaker_off_fetches_nothing(self) -> None:
        self.run_case(7)


if __name__ == "__main__":
    unittest.main()
