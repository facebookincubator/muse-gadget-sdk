# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers and turn completion with host-side event sinks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class ChatSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        handlers = source[source.index('static int find_msg('):source.index('static void on_chat_ack(')]
        reset = source[source.index('static bool turn_start('):source.index('static void turn_begin(')]
        done = source[source.index('static void turn_done('):source.index('/* Ends any turn')]
        check = source[source.index('static void check_turn('):source.index('/* ---- Inbound dispatch')]
        tts = source.index('static void start_tts(void)')
        start_tts = source[tts:source.index('\n}\n', tts) + 3]
        code = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
''' + constants + types + r'''
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
static int captions, console_events;
static int done_events;
static int64_t clock_us = 12345;
static char done_text[EV_TEXT], console_type[32], console_fields[128];
enum mark_t { M_TEXT, M_DONE };
static void mark(mark_t) {}
static int64_t now_us() { return clock_us; }
static void emit(muse_hatch_ev_t type, const char *text) {
    if (s_turn.text) return;
    if (type == MUSE_HATCH_EV_REPLY) captions++;
    if (type == MUSE_HATCH_EV_DONE) {
        done_events++;
        strlcpy(done_text, text ? text : "", sizeof(done_text));
    }
}
void muse_hatch_console(const char *type, const char *, const char *fields, ...) {
    console_events++;
    strlcpy(console_type, type, sizeof(console_type));
    va_list args;
    va_start(args, fields);
    if (fields) vsnprintf(console_fields, sizeof(console_fields), fields, args);
    else console_fields[0] = '\0';
    va_end(args);
}
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) {
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static int fail_events;
static char fail_text[EV_TEXT];
static void turn_fail(const char *text) {
    fail_events++;
    strlcpy(fail_text, text, sizeof(fail_text));
    turn_finish();
}
static bool ensure_connected() { return true; }
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
static void on_dictation_end(bool) {}
static void log_marks() {}
''' + done + reset + handlers + start_tts + check + r'''
static void begin(bool typed = false) {
    clock_us = 12345;
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.chat_us = clock_us;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
    done_events = fail_events = 0;
    done_text[0] = fail_text[0] = console_type[0] = console_fields[0] = '\0';
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void rejected_deltas() {
    for (bool typed : {false, true}) {
        begin(typed);
        event("delta.message_start", "other", "elsewhere");
        event("delta.text_append", "other", "", "Wrong reply");
        event("delta.message_done", "other");
        event("message.assistant", "other", "", "Wrong final");
        assert(!s_turn.nmsgs && !captions && !console_events);
        assert(!s_turn.last_content_us && !s_turn.last_event_us);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Our reply");
        event("delta.message_done", "reply");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].len == 9);
        assert(typed ? console_events == 2 : captions == 1);
        begin(typed);
        event("message.assistant", "other", "", "New turn");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done);
    }
}
static void valid_parents() {
    begin();
    event("message.assistant", "first", "note", "First");
    event("message.assistant", "second", "parent", "Second");
    event("message.assistant", "third", "first", "Third");
    event("message.assistant", "fourth", "", "Live");
    assert(s_turn.nmsgs == 4);
    for (int i = 0; i < s_turn.nmsgs; i++) assert(s_turn.msgs[i].done);
    cJSON *payload = cJSON_Parse("{\"parent_message_id\":\"elsewhere\"}");
    assert(bind_msg("fallback", payload) == -1);
    cJSON_Delete(payload);
    event("message.assistant", "fallback", "", "Wrong final");
    assert(s_turn.nmsgs == 4);
}
static void bounded_rejections() {
    begin();
    event("delta.message_start", "reply", "note");
    char id[20];
    for (int i = 0; i < 9; i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id, sizeof(id), "other%d", i);
        event("delta.message_start", id, "elsewhere");
    }
    event("message.assistant", "other0", "", "Wrong final");
    event("message.assistant", id, "", "Overflow final");
    assert(s_turn.nmsgs == 1 && !captions && !console_events);
    event("delta.text_append", "reply", "", "Our reply");
    event("delta.message_done", "reply");
    event("message.assistant", "second", "note", "Second");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[0].done && s_turn.msgs[1].done);
}
static char s_texts[MAX_MSGS * TEXT_MAX];
/* A voice reply streamed as these pieces; returns the text kept for its captions. */
static const char *voice_reply(std::initializer_list<const char *> pieces) {
    s_turn.texts = s_texts;   /* turn_start() keeps it */
    begin();
    event("delta.message_start", "long", "note");
    for (const char *piece : pieces) event("delta.text_append", "long", "", piece);
    event("delta.message_done", "long");
    return s_texts;
}
static void cut_inside_a_character() {
    /* The cut falls inside a two-byte character: it's left out whole, and so is
     * everything after it, even what would fit in the byte that frees. */
    static char head[TEXT_MAX];
    memset(head, 'a', TEXT_MAX - 2);
    head[TEXT_MAX - 2] = '\0';
    const char *kept = voice_reply({head, "\xc3\xa9" "bbb", "z"});
    assert(strlen(kept) == TEXT_MAX - 2 && !strpbrk(kept, "\xc3" "bz"));
    assert(s_turn.msgs[0].len == TEXT_MAX - 2 + 5 + 1);
    s_turn.texts = nullptr;
}
static void character_split_across_pieces() {
    /* A piece ends with a character's first byte, exactly at the cut; its last
     * byte comes in the next piece and doesn't fit. */
    static char head[TEXT_MAX];
    memset(head, 'a', TEXT_MAX - 2);
    head[TEXT_MAX - 2] = '\xc3';
    head[TEXT_MAX - 1] = '\0';
    const char *kept = voice_reply({head, "\xa9" "bbb"});
    assert(strlen(kept) == TEXT_MAX - 2 && !strpbrk(kept, "\xc3" "b"));
    s_turn.texts = nullptr;
}
static void paced_to_the_kept_text() {
    /* Speaker off, a reply is shown at reading pace: for the text kept, not
     * for the whole reply, which would crawl the kept part across its time. */
    static char reply[TEXT_MAX + 1000];
    memset(reply, 'a', sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';
    const char *kept = voice_reply({reply});
    start_tts();
    const msg_t &m = s_turn.msgs[0];
    assert(m.tts == TTS_ACTIVE && m.len == sizeof(reply) - 1 && strlen(kept) == TEXT_MAX - 1);
    assert(m.pcm_frames == (uint32_t)((TEXT_MAX - 1) * MIC_RATE / TEXT_CHARS_PER_S));
    voice_reply({"Short reply"});   /* nothing left out: paced as before */
    start_tts();
    assert(s_turn.msgs[0].pcm_frames == (uint32_t)(11 * MIC_RATE / TEXT_CHARS_PER_S));
    s_turn.texts = nullptr;
}
static void capped_voice(bool partial) {
    begin();
    s_turn.agent_busy = true;
    if (partial) {
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Partial reply");
    }
    clock_us = s_turn.start_us + TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !done_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !console_events);
    if (!partial) { /* no reply at all: the turn fails instead of ending */
        assert(fail_events == 1 && !strcmp(fail_text, "NO REPLY FROM MUSE") && !done_events);
        return;
    }
    assert(done_events == 1 && !fail_events);
    assert(!strcmp(done_text, "REPLY INCOMPLETE"));
    assert(s_turn.msgs[0].len == 13 && !s_turn.msgs[0].done);
}
static void completed_voice() {
    begin();
    event("message.assistant", "reply", "note", "Complete reply");
    s_turn.msgs[0].tts = TTS_FINISHED;
    clock_us += SETTLE_US + 1;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1 && !done_text[0]);
    assert(!console_events);
}
static void capped_typed() {
    begin(true);
    s_turn.agent_busy = true;
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !console_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !done_events && console_events == 1);
    assert(!strcmp(console_type, "done"));
    assert(!strcmp(console_fields, "\"messages\":0,\"complete\":false"));
}
static void completed_voice_at_cap(bool busy, int64_t quiet_us) {
    begin();
    clock_us = s_turn.start_us + TURN_CAP_US - quiet_us;
    event("message.assistant", "reply", "note", "Complete reply");
    s_turn.msgs[0].tts = TTS_FINISHED;
    s_turn.agent_busy = busy;
    clock_us = s_turn.start_us + TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !done_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1);
    /* A busy agent may still be replying: the cap cut it off. */
    assert(!strcmp(done_text, busy ? "REPLY INCOMPLETE" : ""));
    assert(!console_events);
}
static void capped_voice_with_pending_playback(tts_t state) {
    begin();
    event("message.assistant", "reply", "note", "Complete reply");
    assert(s_turn.msgs[0].done);
    s_turn.msgs[0].tts = state;
    clock_us = s_turn.start_us + TURN_CAP_US + 1;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1);
    assert(!strcmp(done_text, "REPLY INCOMPLETE"));
    assert(!console_events);
}
static void typed_reply_at_cap(bool finished, bool busy) {
    /* A finished typed reply waiting out the settle ends complete at the cap;
     * one still streaming, or with the agent still working, ends incomplete. */
    begin(true);
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US - 1000000;
    if (finished) {
        event("message.assistant", "reply", "note", "Complete reply");
        s_turn.msgs[0].tts = TTS_FINISHED;
    } else {
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Partial reply");
    }
    s_turn.agent_busy = busy;
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY);
    console_events = 0;
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !done_events && console_events == 1);
    assert(!strcmp(console_type, "done"));
    assert(!strcmp(console_fields, finished && !busy ? "\"messages\":1,\"complete\":true"
                                                     : "\"messages\":1,\"complete\":false"));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    case 3: cut_inside_a_character(); break;
    case 4: character_split_across_pieces(); break;
    case 5: paced_to_the_kept_text(); break;
    case 6: capped_voice(false); break;
    case 7: capped_voice(true); break;
    case 8: completed_voice(); break;
    case 9: capped_typed(); break;
    case 10: completed_voice_at_cap(false, 1000000); break;
    case 11: completed_voice_at_cap(true, 1000000); break;
    case 12: completed_voice_at_cap(true, 4000000); break;
    case 13: capped_voice_with_pending_playback(TTS_QUEUED); break;
    case 14: capped_voice_with_pending_playback(TTS_ACTIVE); break;
    case 15: typed_reply_at_cap(true, false); break;
    case 16: typed_reply_at_cap(true, true); break;
    case 17: typed_reply_at_cap(false, false); break;
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', str(JSON / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'cjson.o'), '-o', str(out / 'session')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)

    def test_long_reply_cut_inside_a_character_keeps_whole_characters(self):
        self.run_case(3)

    def test_long_reply_character_split_across_pieces_is_left_out(self):
        self.run_case(4)

    def test_long_reply_is_paced_to_the_text_kept(self):
        self.run_case(5)

    def test_busy_voice_without_messages_fails_at_cap(self):
        self.run_case(6)

    def test_partial_voice_reply_is_incomplete_at_cap(self):
        self.run_case(7)

    def test_completed_voice_reply_has_empty_done_text(self):
        self.run_case(8)

    def test_typed_cap_keeps_complete_false(self):
        self.run_case(9)

    def test_completed_voice_at_cap_is_complete_only_when_the_agent_is_idle(self):
        for name, case in [('idle settle', 10), ('busy settle', 11), ('busy hold', 12)]:
            with self.subTest(window=name):
                self.run_case(case)

    def test_capped_voice_with_queued_or_active_playback_is_incomplete(self):
        for name, case in [('queued', 13), ('active', 14)]:
            with self.subTest(playback=name):
                self.run_case(case)

    def test_typed_reply_at_cap_is_complete_only_when_finished_and_idle(self):
        for name, case in [('finished idle', 15), ('finished busy', 16), ('streaming', 17)]:
            with self.subTest(reply=name):
                self.run_case(case)
