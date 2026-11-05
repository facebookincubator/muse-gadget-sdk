/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Runs the production muse_voice_call.cpp against host fakes of FreeRTOS and
 * of the session's streams (test_muse_voice_call.py). The harness plays both
 * tasks in turn: the voice task through muse_voice_call.h, the session task
 * through muse_voice_call_priv.h, and a synthetic server answering on the
 * call's streams. Every request the call makes is printed as one JSON line
 * ({"op", "id", "path", "body", "end"}) for Python to check.
 */

#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "host_compat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#include "muse_voice_call_priv.h"

/* ---- FreeRTOS, ESP-IDF ---- */

struct fake_queue {
    size_t item, cap;
    std::vector<std::vector<uint8_t>> items;
};

struct fake_stream {
    size_t cap;
    std::vector<uint8_t> data;
};

static int64_t s_now = 1000000;
static uint32_t s_rand = 1;

extern "C" {
QueueHandle_t xQueueCreateWithCaps(size_t length, size_t item, uint32_t)
{
    return new fake_queue{ item, length, {} };
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t)
{
    if (q->items.size() >= q->cap) {
        return pdFALSE;
    }
    const uint8_t *p = static_cast<const uint8_t *>(item);
    q->items.emplace_back(p, p + q->item);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t)
{
    if (q->items.empty()) {
        return pdFALSE;
    }
    memcpy(item, q->items.front().data(), q->item);
    q->items.erase(q->items.begin());
    return pdTRUE;
}

StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t, uint32_t)
{
    return new fake_stream{ size, {} };
}

size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t)
{
    size_t n = len < s->cap - s->data.size() ? len : s->cap - s->data.size();
    const uint8_t *p = static_cast<const uint8_t *>(data);
    s->data.insert(s->data.end(), p, p + n);
    return n;
}

size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t)
{
    size_t n = len < s->data.size() ? len : s->data.size();
    if (n) {
        memcpy(data, s->data.data(), n);
    }
    s->data.erase(s->data.begin(), s->data.begin() + (long)n);
    return n;
}

size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t s)
{
    return s->cap - s->data.size();
}

size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s)
{
    return s->data.size();
}

void *heap_caps_malloc_prefer(size_t size, size_t, ...)
{
    return malloc(size);
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; i++) {
        s_rand = s_rand * 1103515245 + 12345;
        p[i] = (uint8_t)(s_rand >> 16);
    }
}

int64_t esp_timer_get_time(void)
{
    return s_now;
}

/* ---- The rest of Muse ---- */

static bool s_configured = true, s_reachable = true;

bool muse_hatch_ready(void)
{
    return s_configured;
}

bool muse_hatch_configured(void)
{
    return s_configured;
}

void muse_hatch_tail_words(const char *src, char *out, size_t cap)
{
    strlcpy(out, src, cap);
}

const char *muse_voice_call_device_id(void)
{
    return "homelink-00a1b2";
}

/* ---- The session ---- */

struct op_t {
    std::string op, path, body;
    int64_t id;
    bool end;
};

static std::vector<op_t> s_ops;
static int64_t s_next_id = 1;
static bool s_send_fails, s_open_fails;
static bool s_connected = true;   /* the session's connection is up */
static int s_end_after_sends = -1; /* the voice task hangs up after this many more media sends (mid-pump) */
static int s_wakes;
static std::vector<int64_t> s_open;   /* streams the session holds open */

static bool is_open(int64_t id)
{
    for (int64_t o : s_open) {
        if (o == id) {
            return true;
        }
    }
    return false;
}

static void close_id(int64_t id)
{
    for (size_t i = 0; i < s_open.size(); i++) {
        if (s_open[i] == id) {
            s_open.erase(s_open.begin() + (long)i);
            return;
        }
    }
}

int64_t muse_session_call_open(bool media, const char *path, const char *content_type, const char *body, bool end_body)
{
    assert(path && content_type);
    if (s_open_fails || !s_connected) {
        return 0;
    }
    int64_t id = s_next_id++;
    s_ops.push_back({ media ? "media" : "end_call", path, body ? body : "", id, end_body });
    s_open.push_back(id);
    return id;
}

bool muse_session_call_send(int64_t id, const uint8_t *data, size_t len, bool end_body)
{
    if (s_send_fails) {
        return false;
    }
    assert(is_open(id));   /* never a stream the session closed */
    s_ops.push_back({ "send", "", std::string(reinterpret_cast<const char *>(data ? data : (const uint8_t *)""), len),
                      id, end_body });
    if (s_end_after_sends > 0 && --s_end_after_sends == 0) {
        muse_voice_call_end();   /* the voice task, between two sends of one pump */
    }
    return true;
}

void muse_session_call_reset(int64_t id)
{
    if (id > 0) {
        s_ops.push_back({ "reset", "", "", id, false });
        close_id(id);
    }
}

bool muse_session_call_connect(void)
{
    if (s_reachable) {
        s_connected = true;
    }
    return s_reachable;
}

void muse_session_call_wake(void)
{
    s_wakes++;
}
}

/* ---- Helpers ---- */

static void print_ops(const char *scenario)
{
    for (const op_t &o : s_ops) {
        std::string esc;
        for (char c : o.body) {
            if (c == '"' || c == '\\') {
                esc += '\\';
                esc += c;
            } else if (c == '\n') {
                esc += "\\n";
            } else {
                esc += c;
            }
        }
        printf("{\"scenario\":\"%s\",\"op\":\"%s\",\"id\":%lld,\"path\":\"%s\",\"end\":%s,\"body\":\"%s\"}\n", scenario,
               o.op.c_str(), (long long)o.id, o.path.c_str(), o.end ? "true" : "false", esc.c_str());
    }
}

static int count(const char *op, int64_t id = 0)
{
    int n = 0;
    for (const op_t &o : s_ops) {
        n += o.op == op && (!id || o.id == id);
    }
    return n;
}

static int64_t media_id(void)
{
    for (auto it = s_ops.rbegin(); it != s_ops.rend(); ++it) {
        if (it->op == "media") {
            return it->id;
        }
    }
    return 0;
}

static int64_t end_call_id(void)
{
    for (auto it = s_ops.rbegin(); it != s_ops.rend(); ++it) {
        if (it->op == "end_call") {
            return it->id;
        }
    }
    return 0;
}

static void poll(int n = 1, int64_t step_us = 2000)
{
    for (int i = 0; i < n; i++) {
        s_now += step_us;
        assert(muse_voice_call_poll());
    }
}

/* One poll that should ask for the connection to go; the session then drops it. */
/* The connection goes: the session forgets its streams, then tells the call. */
static void lose_connection(void)
{
    s_connected = false;
    s_open.clear();
    muse_voice_call_disconnected();
}

static void poll_expect_drop(int64_t step_us = 2000)
{
    s_now += step_us;
    assert(!muse_voice_call_poll());
    lose_connection();
}

/* The server's side of a stream. */
static void head(int64_t id, int status, const char *date = nullptr)
{
    muse_voice_call_head(id, status, date, date ? strlen(date) : 0);
}

static void line(int64_t id, const std::string &json)
{
    std::string l = json + "\n";
    /* Split across two frames to exercise the line splitter. */
    size_t half = l.size() / 2;
    muse_voice_call_data(id, reinterpret_cast<const uint8_t *>(l.data()), half);
    muse_voice_call_data(id, reinterpret_cast<const uint8_t *>(l.data()) + half, l.size() - half);
}

static void stream_end(int64_t id, bool ok, int status)
{
    close_id(id);
    muse_voice_call_stream_end(id, ok, status);
}

/* The VM's answer to an end-call: 200 with the daemon's envelope, or not. */
static void answer_end(int64_t id, int status = 200, const char *body = "{\"ok\":true,\"result\":{\"ok\":true}}")
{
    if (status >= 400) {
        stream_end(id, false, status);
        return;
    }
    std::string b = body;
    muse_voice_call_data(id, reinterpret_cast<const uint8_t *>(b.data()), b.size());
    stream_end(id, true, status);
}

static std::string b64(const std::vector<int16_t> &pcm)
{
    std::vector<uint8_t> bytes;
    for (int16_t v : pcm) {
        bytes.push_back((uint8_t)v);
        bytes.push_back((uint8_t)((uint16_t)v >> 8));
    }
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    out.resize(muse_vc_b64_encode(bytes.data(), bytes.size(), &out[0]));
    return out;
}

/* One 40 ms CAVD audio event as the server sends it: 960 samples at 24 kHz. */
static std::string cavd(double amplitude, double hz = 440.0)
{
    static double phase;
    std::vector<int16_t> pcm(960);
    for (int16_t &v : pcm) {
        v = (int16_t)lround(amplitude * sin(phase));
        phase += 2 * M_PI * hz / 24000.0;
    }
    return "{\"type\":\"continuous_audio_video.delta\",\"audio_delta\":{\"audio_frames\":\"" + b64(pcm) +
           "\",\"format\":{\"rate\":24000,\"type\":\"\"},\"num_channels\":1,\"num_frames\":1},\"timestamp_ms\":0}";
}

static muse_voice_call_ev_t next_event(char *text, size_t cap)
{
    return muse_voice_call_event(text, cap);
}

static muse_voice_call_ev_t expect_event(muse_voice_call_ev_t want, char *text = nullptr)
{
    char buf[96];
    muse_voice_call_ev_t ev = next_event(buf, sizeof(buf));
    if (ev != want) {
        fprintf(stderr, "expected event %d, got %d (%s)\n", want, ev, buf);
        assert(ev == want);
    }
    if (text) {
        strcpy(text, buf);
    }
    return ev;
}

static size_t read_all(std::vector<int16_t> *out = nullptr, bool *flushed_any = nullptr)
{
    int16_t buf[320];
    size_t total = 0;
    for (;;) {
        bool flushed;
        size_t n = muse_voice_call_read(buf, 320, &flushed);
        if (flushed && flushed_any) {
            *flushed_any = true;
        }
        if (!n && !flushed) {
            return total;
        }
        if (out) {
            out->insert(out->end(), buf, buf + n);
        }
        total += n;
    }
}

static void mic(size_t samples, double amplitude = 6000)
{
    std::vector<int16_t> pcm(samples);
    for (size_t i = 0; i < samples; i++) {
        pcm[i] = (int16_t)lround(amplitude * sin(2 * M_PI * 300.0 * (double)i / 16000.0));
    }
    for (size_t off = 0; off < samples; off += 320) {
        muse_voice_call_audio(pcm.data() + off, samples - off < 320 ? samples - off : 320);
    }
}

static const char *SESSION_EVENT =
    "{\"type\":\"hatch.voice.session\",\"event_id\":\"e1\",\"call_id\":\"%s\","
    "\"session_id\":\"00000000-0000-4000-8000-000000000001\",\"thread_id\":\"00000000-0000-4000-8000-000000000002\","
    "\"submission_message_id\":\"m\",\"request_id\":\"r\"}";

static std::string call_id_of(int64_t media)
{
    for (const op_t &o : s_ops) {
        if (o.op == "media" && o.id == media) {
            return o.path.substr(o.path.find('=') + 1);
        }
    }
    return "";
}

/* Starts a call and brings it live; returns the media stream id. */
static int64_t live_call(const char *date = "Fri, 09 Oct 2026 06:35:58 GMT")
{
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    assert(id && is_open(id));
    head(id, 200, date);
    line(id, "{\"type\":\"session.created\",\"event_id\":\"e0\",\"session\":{}}");
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(id).c_str());
    line(id, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
    return id;
}

/* The server's half of a graceful ending: answers the end-call, then the half-close. */
static void serve_ending(int64_t media)
{
    poll();
    int64_t end = end_call_id();
    assert(end && is_open(end));
    head(end, 200);
    line(end, "{\"ok\":true,\"result\":{\"ok\":true}}");
    stream_end(end, true, 200);
    poll();
    if (is_open(media)) {
        assert(s_ops.back().op == "send" && s_ops.back().id == media && s_ops.back().end && s_ops.back().body.empty());
        stream_end(media, true, 200);
        poll();
    }
}

static void reset_world(void)
{
    s_ops.clear();
    s_open.clear();
    s_send_fails = s_open_fails = false;
    s_reachable = s_configured = s_connected = true;
    char text[96];
    while (next_event(text, sizeof(text)) != MUSE_VOICE_CALL_EV_NONE) {
    }
    read_all();
}

/* ---- Scenarios ---- */

static void happy(void)
{
    int64_t id = live_call();
    assert(muse_voice_call_busy());
    /* A second of speech goes up as 25 paced appends. */
    mic(16000);
    for (int i = 0; i < 100; i++) {
        poll(1, 10000);
    }
    assert(count("send", id) == 25);
    /* The greeting's audio: 0.4 s of tone, then keepalive silence. */
    for (int i = 0; i < 10; i++) {
        line(id, cavd(8000));
    }
    std::vector<int16_t> heard;
    assert(read_all(&heard) == 10 * 640);
    int peak = muse_vc_peak(heard.data(), heard.size());
    assert(peak > 7000 && peak <= 8100);
    line(id, cavd(0));
    assert(read_all(&heard) == 640);
    /* Captions, turn events, a cancelled response: the call goes on. */
    line(id, "{\"type\":\"response.created\",\"event_id\":\"e\",\"response\":{\"id\":\"r1\"}}");
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\"Hello \"}");
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\"there\"}");
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_REPLY, text);
    expect_event(MUSE_VOICE_CALL_EV_REPLY, text);
    assert(!strcmp(text, "Hello there"));
    line(id, "{\"type\":\"conversation.item.input_audio_transcription.completed\",\"event_id\":\"e\","
             "\"item_id\":\"i\",\"transcript\":\"What\\u2019s the weather?\"}");
    expect_event(MUSE_VOICE_CALL_EV_HEARD, text);
    assert(!strcmp(text, "What\xe2\x80\x99s the weather?"));
    line(id, "{\"type\":\"response.done\",\"event_id\":\"e\",\"response\":{\"id\":\"r1\",\"status\":\"cancelled\"}}");
    line(id, "{\"type\":\"some.future_event\",\"event_id\":\"e\",\"x\":[1,{\"y\":null}]}");
    line(id, "not json at all");
    poll();
    assert(muse_voice_call_busy() && count("end_call") == 0);
    /* Hang up: end-call, then half-close, then the media drains. */
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "CALL ENDED"));
    assert(!muse_voice_call_busy() && s_open.empty());
    print_ops("happy");
}

static void barge_in(void)
{
    int64_t id = live_call();
    for (int i = 0; i < 20; i++) {
        line(id, cavd(9000));
    }
    line(id, "{\"type\":\"input_audio_buffer.speech_started\",\"event_id\":\"e\",\"item_id\":\"i\",\"audio_start_ms\":5}");
    /* Audio the cancelled reply still sends before the reader acks is dropped. */
    line(id, cavd(9000));
    bool flushed = false;
    std::vector<int16_t> got;
    read_all(&got, &flushed);
    assert(flushed && got.empty());
    /* After the ack, audio flows again. */
    line(id, cavd(9000));
    flushed = false;
    assert(read_all(&got, &flushed) == 640 && !flushed);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void hold(void)
{
    int64_t id = live_call();
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":true}");
    expect_event(MUSE_VOICE_CALL_EV_HOLD);
    int before = count("send", id);
    mic(16000);
    poll(100, 10000);
    assert(count("send", id) == before);   /* nothing goes up while held */
    /* Held input isn't idle, however long. */
    poll(60, 1000000);
    assert(muse_voice_call_busy() && !count("end_call"));
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":false}");
    expect_event(MUSE_VOICE_CALL_EV_RESUME);
    mic(3200);
    poll(30, 10000);
    assert(count("send", id) == before + 5);   /* 0.2 s: five 40 ms appends */
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void rejected(void)
{
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    /* 403 before admission (the device route not allowed, say). */
    stream_end(id, false, 403);
    poll();
    int64_t end = end_call_id();
    assert(end && count("reset", id) == 0);   /* the stream is already gone */
    stream_end(end, false, 403);              /* refused too: unconfirmed, so the connection goes */
    poll_expect_drop();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "MUSE DIDN'T TAKE THE CALL (403)"));
    assert(!muse_voice_call_busy());
    print_ops("rejected");
}

static void dropped_before_head(void)
{
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    stream_end(id, false, 0);   /* a reset with no head: no phantom call */
    poll();
    assert(count("end_call") == 1);
    answer_end(end_call_id());
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "CALL FAILED") && !muse_voice_call_busy());
}

static void post_head_error(void)
{
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    head(id, 200);
    line(id, "{\"type\":\"error\",\"event_id\":\"e\",\"error\":{\"type\":\"backend_unavailable\",\"message\":\"later\"}}");
    poll();
    /* Before readiness an error means the call didn't come up: reset and end-call. */
    assert(count("reset", id) == 1 && count("end_call") == 1);
    answer_end(end_call_id());
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "backend_unavailable"));
}

/* The end-call ops sent so far for this call id. */
static std::vector<op_t> end_calls_for(const std::string &call_id)
{
    std::vector<op_t> out;
    for (const op_t &o : s_ops) {
        if (o.op == "end_call" && o.body.find("\"call_id\":\"" + call_id + "\"") != std::string::npos) {
            out.push_back(o);
        }
    }
    return out;
}

static void connection_lost(void)
{
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    s_now += 3000000;
    lose_connection();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "LOST CONNECTION TO MUSE"));
    /* Nothing goes on a dead connection; the call is kept for the next one. */
    assert(count("end_call") == 0 && !muse_voice_call_busy());
    poll(5, 1000000);
    assert(count("end_call") == 0);
    /* The session reconnects by itself: the old call's end-call goes, once. */
    s_connected = true;
    poll(2, 1000000);
    auto ends = end_calls_for(old_id);
    assert(ends.size() == 1 && count("end_call") == 1 && count("media") == 1);
    assert(ends[0].path == "/voice/end-call" && ends[0].end);
    assert(ends[0].body.find("\"thread_id\":\"00000000-0000-4000-8000-000000000002\"") != std::string::npos);
    assert(muse_voice_call_busy());
    answer_end(ends[0].id);
    poll(30, 1000000);
    assert(count("end_call") == 1 && !muse_voice_call_busy());   /* confirmed: nothing more */
    print_ops("connection_lost");
}

static void cleanup_before_new_call(void)
{
    /* Lost, then a new call before the session has tried the old end-call:
     * the old one goes first, under its own id, and the new call waits for it. */
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    muse_voice_call_start();
    poll();
    auto ends = end_calls_for(old_id);
    assert(ends.size() == 1 && count("media") == 1);   /* no new media yet */
    poll(5);
    assert(count("media") == 1);
    answer_end(ends[0].id);
    poll();
    int64_t fresh_media = media_id();
    assert(fresh_media != id && call_id_of(fresh_media) != old_id);
    assert(s_ops.back().op == "media");
    /* Nothing of the old call (audio, admission) was sent again. */
    for (const op_t &o : s_ops) {
        assert(!(o.op == "media" && o.id != id && o.id != fresh_media));
    }
    head(fresh_media, 200);
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(fresh_media).c_str());
    line(fresh_media, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
    muse_voice_call_end();
    serve_ending(fresh_media);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(end_calls_for(old_id).size() == 1 && end_calls_for(call_id_of(fresh_media)).size() == 1);
}

static void cleanup_stale_answer(void)
{
    /* The old end-call gets no answer in time: the new call goes ahead, and
     * the old answer arriving late touches nothing of the new call. */
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    muse_voice_call_start();
    poll();
    int64_t old_end = end_calls_for(old_id).at(0).id;
    poll(1, 10 * 1000000LL);
    assert(count("media") == 1);
    poll(1, 2 * 1000000LL);   /* past the answer deadline: reset, and the new call starts */
    assert(count("reset", old_end) == 1);
    int64_t fresh_media = media_id();
    assert(fresh_media != id);
    head(fresh_media, 200);
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(fresh_media).c_str());
    line(fresh_media, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
    /* The late answer to the old end-call, on the stream the session reset. */
    muse_voice_call_data(old_end, reinterpret_cast<const uint8_t *>("{\"ok\":true,\"result\":{\"ok\":true}}"), 32);
    muse_voice_call_stream_end(old_end, true, 200);
    poll(3);
    char text[96];
    assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE && muse_voice_call_busy());
    assert(end_calls_for(call_id_of(fresh_media)).empty());
    /* The new call's admission replaced the old one at Muse: no more old end-calls. */
    muse_voice_call_end();
    serve_ending(fresh_media);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    poll(60, 1000000);
    assert(end_calls_for(old_id).size() == 1);
}

static void cleanup_bounded(void)
{
    /* Unconfirmed every time (503, a reset, a lost connection): three tries in
     * all, the same body each time, then it's dropped. */
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    s_connected = true;
    poll();
    auto ends = end_calls_for(old_id);
    assert(ends.size() == 1);
    answer_end(ends[0].id, 503);
    poll(1, 6 * 1000000LL);
    ends = end_calls_for(old_id);
    assert(ends.size() == 2 && ends[1].body == ends[0].body);
    lose_connection();   /* with the second in flight */
    s_connected = true;
    poll(1, 6 * 1000000LL);
    ends = end_calls_for(old_id);
    assert(ends.size() == 3 && ends[2].body == ends[0].body);
    stream_end(ends[2].id, false, 0);   /* reset */
    for (int i = 0; i < 30; i++) {
        poll(1, 1000000);
        if (i == 10) {
            lose_connection();
            s_connected = true;
        }
    }
    assert(end_calls_for(old_id).size() == 3 && !muse_voice_call_busy());
    /* A refusal (4xx) is final at once. */
    s_ops.clear();
    id = live_call();
    old_id = call_id_of(id);
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    s_connected = true;
    poll();
    answer_end(end_calls_for(old_id).at(0).id, 403);
    poll(30, 1000000);
    assert(end_calls_for(old_id).size() == 1);
}

static void cleanup_expires(void)
{
    /* Never reconnected within the keep time: dropped without a send. */
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    poll(1, 11 * 60 * 1000000LL);
    s_connected = true;
    poll(5, 1000000);
    assert(end_calls_for(old_id).empty());
}

static void send_fails(void)
{
    int64_t id = live_call();
    (void)id;
    s_send_fails = true;
    mic(1600);
    s_now += 100000;
    assert(!muse_voice_call_poll());   /* the session drops the connection... */
    s_open.clear();
    muse_voice_call_disconnected();    /* ...which ends the call here */
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(!muse_voice_call_busy());
}

static void no_answer(void)
{
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    poll(1, 30 * 1000000LL);
    assert(muse_voice_call_busy());
    poll(1, 10000);
    assert(count("reset", id) == 1 && count("end_call") == 1);
    poll_expect_drop(11 * 1000000LL);   /* the end-call never answered either: the connection goes */
    assert(count("reset", end_call_id()) == 1);
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "MUSE DIDN'T ANSWER") && !muse_voice_call_busy() && s_open.empty());
}

static void idle(void)
{
    int64_t id = live_call();
    /* Keepalive silence for a minute: it doesn't count as anything said. */
    for (int i = 0; i < 46 * 25; i++) {
        line(id, cavd(0));
        read_all();
        s_now += 40000;
        assert(muse_voice_call_poll());
        if (count("end_call")) {
            break;
        }
    }
    assert(count("end_call") == 1);
    serve_ending(id);
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "CALL ENDED: ALL QUIET"));
    /* Audible speech does count: a minute of it keeps the call up. */
    id = live_call();
    for (int i = 0; i < 60 * 25; i++) {
        line(id, cavd(5000));
        read_all();
        s_now += 40000;
        assert(muse_voice_call_poll());
    }
    assert(!count("end_call", 0) || end_call_id() < id);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void stop(void)
{
    int64_t id = live_call();
    line(id, "{\"type\":\"response.done\",\"event_id\":\"e\",\"response\":{\"status\":\"cancelled\"}}");
    line(id, cavd(8000));
    /* item.call_id is the control's own id, not the call's: never compared. */
    const char *stop_line = "{\"type\":\"conversation.item.added\",\"event_id\":\"evt_0123\",\"item\":{\"id\":\"item_0123\","
                            "\"type\":\"session_control\",\"role\":\"assistant\",\"status\":\"completed\","
                            "\"call_id\":\"6f1c0000-0000-4000-8000-00000000c0de\",\"name\":\"stop\",\"arguments\":\"{}\"}}";
    line(id, stop_line);
    line(id, stop_line);   /* a duplicate changes nothing */
    line(id, cavd(8000));  /* the goodbye's last words, still arriving */
    poll();
    assert(count("end_call") == 1 && count("reset", id) == 0);
    /* What Muse said, before and after the stop, still plays out. */
    assert(read_all() == 2 * 640);
    serve_ending(id);
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "MUSE ENDED THE CALL"));
    print_ops("stop");
}

static void remote_end(void)
{
    int64_t id = live_call();
    stream_end(id, true, 200);   /* the server ended the media (preempted, say) */
    poll();
    assert(count("end_call") == 1);
    answer_end(end_call_id());
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "MUSE ENDED THE CALL") && !muse_voice_call_busy());
}

static void generations(void)
{
    /* Call A ends; B starts before A's end-call is answered. */
    int64_t a = live_call();
    muse_voice_call_end();
    poll();
    int64_t a_end = end_call_id();
    assert(a_end);
    uint32_t before = (uint32_t)s_ops.size();
    muse_voice_call_start();   /* B: waits for A to finish */
    poll(3);
    assert(media_id() == a);
    /* A finishes; B begins. A's ENDED is not B's. */
    answer_end(a_end);
    poll();
    stream_end(a, true, 200);
    poll();
    int64_t b = media_id();
    assert(b != a && s_ops.size() > before);
    char text[96];
    assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE);
    /* A's late frames are ignored; B goes live. */
    line(a, cavd(9000));
    head(a, 200);
    head(b, 200);
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(b).c_str());
    line(b, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
    assert(read_all() == 0);
    muse_voice_call_end();
    serve_ending(b);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(call_id_of(a) != call_id_of(b));
}

static void unreachable(void)
{
    s_reachable = false;
    muse_voice_call_start();
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "CAN'T REACH MUSE") && s_ops.empty() && !muse_voice_call_busy());
}

static void hang_up_before_start(void)
{
    /* A tap and a press before the session gets to it: no call is made. */
    muse_voice_call_start();
    muse_voice_call_end();
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(s_ops.empty() && !muse_voice_call_busy());
}

static void long_line(void)
{
    int64_t id = live_call();
    std::string big = "{\"type\":\"response.output_audio_transcript.done\",\"transcript\":\"" +
                      std::string(MUSE_VC_LINE_MAX, 'x') + "\"}";
    line(id, big);   /* skipped, bounded; the call goes on */
    line(id, cavd(8000));
    assert(read_all() == 640);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void backpressure(void)
{
    int64_t id = live_call();
    int lines = 0;
    while (!muse_voice_call_rx_full()) {
        line(id, cavd(8000));
        assert(++lines < 200);
    }
    assert(lines > 40);
    /* While it isn't full, one frame the size of the session's scratch, all
     * audio lines, is taken whole: nothing of it is dropped. */
    std::string burst;
    int burst_lines = 0;
    while (burst.size() + 2700 < MUSE_SESSION_FRAME_MAX) {
        burst += cavd(8000) + "\n";
        burst_lines++;
    }
    int16_t buf[320];
    bool flushed;
    size_t taken = 0;
    while (muse_voice_call_rx_full()) {
        taken += muse_voice_call_read(buf, 320, &flushed);
    }
    muse_voice_call_data(id, reinterpret_cast<const uint8_t *>(burst.data()), burst.size());
    taken += read_all();
    assert(taken == (size_t)(lines + burst_lines) * 640);
    /* The goodbye drain respects the same headroom. */
    assert(!muse_voice_call_rx_full());
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void end_call_unconfirmed(void)
{
    /* 503, or 200 without the ok envelope: the call may still be up on Muse's
     * side. Once the media is down the connection goes too, and the same
     * end-call, byte for byte, goes again on the next connection. */
    for (int variant = 0; variant < 2; variant++) {
        int64_t id = live_call();
        std::string old_id = call_id_of(id);
        muse_voice_call_end();
        poll();
        int64_t end = end_call_id();
        std::string first_body = end_calls_for(old_id).at(0).body;
        if (variant == 0) {
            answer_end(end, 503);
        } else {
            answer_end(end, 200, "{\"ok\":false,\"error\":{\"code\":\"internal\"}}");
        }
        poll();
        assert(s_ops.back().op == "send" && s_ops.back().id == id && s_ops.back().end);   /* half-close */
        stream_end(id, true, 200);
        poll_expect_drop();
        expect_event(MUSE_VOICE_CALL_EV_ENDED);
        assert(!muse_voice_call_busy());
        s_now += 5000000;
        s_connected = true;
        poll();
        auto ends = end_calls_for(old_id);
        assert(ends.size() == 2 && ends[1].body == first_body);
        answer_end(ends[1].id);
        poll(10, 1000000);
        assert(end_calls_for(old_id).size() == 2);
        s_ops.clear();
    }
}

static void hold_race(void)
{
    int64_t id = live_call();
    int before = count("send", id);
    /* Held and released between two polls: what the mic captured meanwhile stays put. */
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":true}");
    mic(640 * 3);
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":false}");
    poll(30, 10000);
    assert(count("send", id) == before);
    expect_event(MUSE_VOICE_CALL_EV_HOLD);
    expect_event(MUSE_VOICE_CALL_EV_RESUME);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void long_speech(void)
{
    /* The caller talks for 50 s without the server finding an end: not idle. */
    int64_t id = live_call();
    line(id, "{\"type\":\"input_audio_buffer.speech_started\",\"event_id\":\"e\",\"item_id\":\"i\"}");
    for (int i = 0; i < 50 * 25; i++) {
        mic(640);
        line(id, cavd(0));
        read_all();
        s_now += 40000;
        assert(muse_voice_call_poll());
    }
    assert(count("end_call") == 0);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void pre_ready_identity(void)
{
    /* Audio before readiness, then a readiness event without this call's id:
     * nothing reaches the speaker, the mic never goes up, and the call fails. */
    muse_voice_call_start();
    poll();
    int64_t id = media_id();
    head(id, 200);
    line(id, cavd(9000));
    line(id, "{\"type\":\"hatch.voice.session\",\"event_id\":\"e\",\"session_id\":\"s\"}");
    line(id, cavd(9000));
    mic(3200);
    poll(20, 10000);
    assert(read_all() == 0);
    int appends = 0;
    for (const op_t &o : s_ops) {
        appends += o.op == "send" && !o.body.empty();
    }
    assert(appends == 0 && count("reset", id) == 1 && count("end_call") == 1);
    answer_end(end_call_id());
    poll();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);   /* no READY before it */
    assert(!strcmp(text, "CALL FAILED"));
    /* The same with the right call id but no session id. */
    s_ops.clear();
    muse_voice_call_start();
    poll();
    id = media_id();
    head(id, 200);
    line(id, "{\"type\":\"hatch.voice.session\",\"call_id\":\"" + call_id_of(id) + "\"}");
    mic(3200);
    poll(20, 10000);
    assert(count("end_call") == 1 && read_all() == 0);
    answer_end(end_call_id());
    poll();
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
}

static void mute_partial(void)
{
    /* Half an append's worth of mic is waiting when the voice task hangs up
     * (mute), and more arrives after: none of it is sent. */
    int64_t id = live_call();
    int before = count("send", id);
    mic(320);                  /* 20 ms: half of a 40 ms append */
    poll();
    assert(count("send", id) == before);
    mic(320);                  /* would complete the append... */
    muse_voice_call_end();     /* ...but the hang-up is queued first */
    mic(320);
    poll();
    assert(count("send", id) == before);
    serve_ending(id);
    for (const op_t &o : s_ops) {
        assert(!(o.op == "send" && o.body.find("input_audio_buffer.append") != std::string::npos));
    }
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

static void cleanup_gate_inflight(void)
{
    /* A start arrives while the background try is already in flight: that try
     * is the start's one wait. When it times out (or fails), the call starts
     * at once, without a second try first. */
    for (int variant = 0; variant < 2; variant++) {
        s_ops.clear();
        int64_t id = live_call();
        std::string old_id = call_id_of(id);
        lose_connection();
        expect_event(MUSE_VOICE_CALL_EV_ENDED);
        s_connected = true;
        poll();
        auto ends = end_calls_for(old_id);
        assert(ends.size() == 1);   /* the background try */
        muse_voice_call_start();
        poll(3);
        assert(count("media") == 1 && end_calls_for(old_id).size() == 1);
        if (variant == 0) {
            poll(1, 11 * 1000000LL);   /* no answer in time */
            assert(count("reset", ends[0].id) == 1);
        } else {
            answer_end(ends[0].id, 503);
            poll();
        }
        assert(count("media") == 2 && end_calls_for(old_id).size() == 1);
        int64_t fresh_media = media_id();
        head(fresh_media, 200);
        char ev[512];
        snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(fresh_media).c_str());
        line(fresh_media, ev);
        poll();
        expect_event(MUSE_VOICE_CALL_EV_READY);
        muse_voice_call_end();
        serve_ending(fresh_media);
        expect_event(MUSE_VOICE_CALL_EV_ENDED);
        poll(30, 1000000);
        assert(end_calls_for(old_id).size() == 1);
    }
}

static void cleanup_4xx_then_lost(void)
{
    /* Muse refuses the end-call (4xx, final), and the connection drops while
     * the media is still draining: the refused call isn't kept for later. */
    int64_t id = live_call();
    std::string old_id = call_id_of(id);
    muse_voice_call_end();
    poll();
    answer_end(end_call_id(), 403);
    poll();
    assert(s_ops.back().op == "send" && s_ops.back().id == id && s_ops.back().end);   /* half-close */
    lose_connection();
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    s_connected = true;
    poll(30, 1000000);
    assert(end_calls_for(old_id).size() == 1);
}

/* ---- Push-to-talk ---- */

/* A CAVD audio event carrying the provider's response_id (or none). */
static std::string cavd_id(double amplitude, const char *rid)
{
    std::string ev = cavd(amplitude);
    if (!rid) {
        return ev;
    }
    return ev.substr(0, ev.size() - 1) + ",\"response_id\":\"" + rid + "\"}";
}

static std::string ev_speech(const char *item)
{
    return std::string("{\"type\":\"input_audio_buffer.speech_started\",\"event_id\":\"e\",\"item_id\":\"") + item +
           "\",\"audio_start_ms\":0}";
}

static std::string ev_turn(const char *submission, const char *item)
{
    return std::string("{\"type\":\"hatch.voice.turn_started\",\"event_id\":\"e\",\"submission_message_id\":\"") +
           submission + "\",\"item_id\":\"" + item + "\"}";
}

static std::string ev_settled(const char *submission)
{
    return std::string("{\"type\":\"hatch.voice.turn_settled\",\"event_id\":\"e\",\"response_message_id\":\"") +
           submission + "\"}";
}

static std::string ev_suppressed(const char *item)
{
    return std::string("{\"type\":\"hatch.voice.input_suppressed\",\"event_id\":\"e\",\"item_id\":\"") + item + "\"}";
}

static std::string ev_created(const char *mapper_id)
{
    return std::string("{\"type\":\"response.created\",\"event_id\":\"e\",\"response\":{\"id\":\"") + mapper_id + "\"}}";
}

static std::string ev_done(const char *mapper_id)
{
    return std::string("{\"type\":\"response.done\",\"event_id\":\"e\",\"response\":{\"id\":\"") + mapper_id +
           "\",\"status\":\"cancelled\"}}";
}

/* Each append's samples, in order, from the media stream's sends. */
static std::vector<std::vector<int16_t>> appends(int64_t media)
{
    std::vector<std::vector<int16_t>> out;
    for (const op_t &o : s_ops) {
        if (o.op != "send" || o.id != media || o.body.find("input_audio_buffer.append") == std::string::npos) {
            continue;
        }
        muse_vc_span_t audio;
        assert(muse_vc_json_get(o.body.data(), o.body.size(), "audio", &audio) == MUSE_VC_J_STRING);
        std::vector<uint8_t> bytes(audio.n);
        long n = muse_vc_b64_decode(audio, bytes.data(), bytes.size());
        assert(n == MUSE_VC_FRAME * 2);
        std::vector<int16_t> pcm(MUSE_VC_FRAME);
        memcpy(pcm.data(), bytes.data(), (size_t)n);
        out.push_back(pcm);
    }
    return out;
}

/* Per-append peaks. */
static std::vector<int> append_peaks(int64_t media)
{
    std::vector<int> peaks;
    for (const auto &a : appends(media)) {
        peaks.push_back(muse_vc_peak(a.data(), a.size()));
    }
    return peaks;
}

/* Held mic: n samples of a 300 Hz tone, pushed in 20 ms reads; returns how many were taken. */
static size_t held(size_t samples, double amplitude = 6000)
{
    std::vector<int16_t> pcm(samples);
    for (size_t i = 0; i < samples; i++) {
        pcm[i] = (int16_t)lround(amplitude * sin(2 * M_PI * 300.0 * (double)i / 16000.0));
    }
    size_t taken = 0;
    for (size_t off = 0; off < samples; off += 320) {
        size_t n = samples - off < 320 ? samples - off : 320;
        if (muse_voice_call_audio(pcm.data() + off, n)) {
            taken += n;
        }
    }
    return taken;
}

static muse_voice_call_ptt_status_t ptt_status(void)
{
    muse_voice_call_ptt_status_t st;
    muse_voice_call_ptt_status(&st);
    return st;
}

/* Brings the media up to live (head and session event). */
static void make_live(int64_t id)
{
    head(id, 200, "Fri, 09 Oct 2026 06:35:58 GMT");
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(id).c_str());
    line(id, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
}

/* A push-to-talk call, live, released with nothing said; returns the media id. */
static int64_t ptt_live_call(void)
{
    muse_voice_call_start_ptt();
    poll();
    int64_t id = media_id();
    assert(id && is_open(id));
    make_live(id);
    muse_voice_call_set_talking(false);
    poll();
    return id;
}

/* Reads the speaker side; returns its peak (0 if nothing). */
static int heard_peak(size_t *samples = nullptr, bool *flushed = nullptr)
{
    /* The voice task reads, the session task runs, the voice task reads. */
    std::vector<int16_t> got;
    read_all(&got, flushed);
    poll();
    read_all(&got, flushed);
    if (samples) {
        *samples = got.size();
    }
    return got.empty() ? 0 : muse_vc_peak(got.data(), got.size());
}

static void ptt_hang_up(int64_t id)
{
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(!st.capture && !st.live && !st.talking && !st.connecting);
}

/* Held audio before the call is up waits for it: none of the mic from before
 * the press, or after the release, goes up; the release is followed by quiet,
 * for as long as the call stays up. */
static void ptt_startup(void)
{
    mic(8000, 12000);                   /* idle mic before any press: not this call's */
    muse_voice_call_start_ptt();
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(st.connecting && st.talking && st.capture && !st.live);
    assert(held(16000) == 16000);       /* one second said while connecting */
    poll();
    int64_t id = media_id();
    assert(id);
    st = ptt_status();
    assert(st.connecting && st.capture && st.sending);
    muse_voice_call_set_talking(false);
    assert(held(3200, 12000) == 0);     /* after the release: refused */
    assert(!ptt_status().capture);
    poll(5);
    assert(count("send", id) == 0);     /* nothing before live */
    make_live(id);
    for (int i = 0; i < 300; i++) {
        poll(1, 10000);
    }
    std::vector<int> peaks = append_peaks(id);
    /* 3 s: 1 s held (25 appends, sent ahead as fast as pacing allows),
     * then quiet from the moment it ran out, paced to real time. */
    assert(peaks.size() >= 97 && peaks.size() <= 102);
    size_t loud = 0;
    for (size_t i = 0; i < peaks.size(); i++) {
        assert(peaks[i] <= 6100);       /* never the idle or post-release mic */
        if (peaks[i] > 1000) {
            assert(i == loud);          /* held audio first, contiguous */
            loud++;
        }
    }
    assert(loud == 25 || loud == 26);
    st = ptt_status();
    assert(st.live && !st.talking && !st.sending && !st.connecting);   /* tail is up; keepalive quiet isn't "sending" */
    size_t before = peaks.size();
    poll(100, 10000);
    assert(append_peaks(id).size() >= before + 20);   /* quiet keeps going while the call is up */
    ptt_hang_up(id);
    print_ops("ptt_startup");
}

/* More held audio than the start buffer takes: the hold's first seconds go
 * up, the rest is refused for that hold and the status says so. */
static void ptt_overflow(void)
{
    muse_voice_call_start_ptt();
    poll();
    int64_t id = media_id();
    size_t taken = held(16000 * 8);
    assert(taken >= 16000 * 5 && taken <= 16000 * 6);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(st.overflow && !st.capture && st.talking);
    make_live(id);
    poll(50, 10000);
    assert(held(3200) == 0);            /* the rest of this hold stays out, even with room */
    for (int i = 0; i < 900; i++) {
        poll(1, 10000);
    }
    muse_voice_call_set_talking(false);
    poll(200, 10000);
    size_t loud = 0;
    for (int p : append_peaks(id)) {
        loud += p > 1000;
    }
    assert(loud * MUSE_VC_FRAME / 3 * 2 >= taken - 640 && loud * MUSE_VC_FRAME / 3 * 2 <= taken + 640);
    /* The next hold starts clean. */
    muse_voice_call_set_talking(true);
    st = ptt_status();
    assert(!st.overflow && st.capture);
    assert(held(1600) == 1600);
    muse_voice_call_set_talking(false);
    ptt_hang_up(id);
}

/* A reply answering the last hold plays; a press stops it at once and for
 * good, however its audio is relabelled afterwards; a later hold's own
 * reply plays, its start kept while Muse confirms the turn. */
static void ptt_press_during_reply(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    held(8000);
    poll(30, 10000);
    line(id, ev_speech("i1"));
    muse_voice_call_set_talking(false);
    poll(150, 10000);
    line(id, ev_turn("s1", "i1"));
    line(id, ev_created("resp_a"));
    for (int i = 0; i < 10; i++) {
        line(id, cavd_id(8000, "p1"));
    }
    poll();
    size_t n = 0;
    assert(heard_peak(&n) > 7000 && n == 10 * 640);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(st.reply_open && st.reply_audio && st.heard && !st.reply_done);
    /* More of it queues up, then the press: gone before the press returns. */
    for (int i = 0; i < 10; i++) {
        line(id, cavd_id(8000, "p1"));
    }
    muse_voice_call_set_talking(true);
    bool flushed = false;
    assert(heard_peak(&n, &flushed) == 0 && n == 0 && flushed);
    st = ptt_status();
    assert(!st.reply_open && !st.reply_audio && !st.heard);
    /* Old audio still arriving, before the session even sees the press. */
    line(id, cavd_id(8000, "p1"));
    poll();
    assert(heard_peak() == 0);
    held(8000);
    poll(30, 10000);
    /* The server relabels: a fresh mapper response, a delayed turn for the
     * old item, old provider audio, unlabelled audio, a new id with no speech. */
    line(id, ev_created("resp_b"));
    line(id, ev_turn("s1b", "i1"));
    line(id, cavd_id(8000, "p1"));
    line(id, cavd_id(8000, nullptr));
    line(id, cavd_id(8000, "p2"));
    muse_voice_call_set_talking(false);
    poll(5);
    assert(heard_peak() == 0);
    line(id, cavd_id(8000, "p2"));
    line(id, ev_done("resp_b"));
    line(id, ev_settled("s1b"));
    poll(5);
    assert(heard_peak() == 0);
    assert(!ptt_status().reply_open && !ptt_status().reply_done);
    /* A hold with speech: its reply's first audio arrives before turn_started. */
    muse_voice_call_set_talking(true);
    held(8000);
    poll(30, 10000);
    line(id, ev_speech("i2"));
    line(id, cavd_id(8000, "p1"));      /* old trailing audio after the new speech */
    line(id, cavd_id(8000, "p2"));      /* stale since the last press */
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    for (int i = 0; i < 5; i++) {
        line(id, cavd_id(3000, "p3"));
    }
    poll(5);
    assert(heard_peak() == 0);          /* waits for its turn */
    line(id, ev_turn("s2", "i2"));
    line(id, ev_created("resp_c"));
    line(id, cavd_id(8000, "p1"));      /* old audio after the new turn: still dropped */
    line(id, cavd_id(3000, "p3"));
    poll(3);
    std::vector<int16_t> got;
    read_all(&got);
    poll();
    read_all(&got);
    assert(got.size() == 6 * 640);
    assert(muse_vc_peak(got.data(), got.size()) < 3100);   /* only p3, its start kept */
    line(id, ev_done("resp_c"));
    poll();
    st = ptt_status();
    assert(st.reply_open && st.reply_audio && st.reply_done);
    ptt_hang_up(id);
}

/* A silent press mid-reply: the reply never comes back, whatever arrives,
 * until a later hold with speech gets its own reply. */
static void ptt_silent_press(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    held(8000);
    poll(10, 10000);
    line(id, ev_speech("i1"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, ev_turn("s1", "i1"));
    for (int i = 0; i < 5; i++) {
        line(id, cavd_id(8000, "p1"));
    }
    poll();
    assert(heard_peak() > 7000);
    muse_voice_call_set_talking(true);   /* silent: no speech_started follows */
    poll(20, 10000);
    muse_voice_call_set_talking(false);
    poll(150, 10000);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(!st.heard && !st.sending && !st.reply_open);
    for (int round = 0; round < 3; round++) {
        line(id, cavd_id(8000, "p1"));
        line(id, ev_done("resp_a"));
        line(id, ev_created("resp_x"));
        line(id, cavd_id(8000, "p1"));
        line(id, cavd_id(8000, nullptr));
        line(id, cavd_id(8000, "p5"));
        line(id, ev_turn("s1x", "i1"));
        line(id, cavd_id(8000, "p6"));
        line(id, ev_settled("s1"));
        poll(100, 10000);
        assert(heard_peak() == 0);
    }
    /* The next hold with speech gets its own reply. */
    muse_voice_call_set_talking(true);
    held(8000);
    poll(10, 10000);
    line(id, ev_speech("i2"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, ev_turn("s2", "i2"));      /* first, this time: the next new id plays at once */
    line(id, cavd_id(8000, "p6"));      /* seen before the press: stale */
    poll();
    assert(heard_peak() == 0);
    line(id, cavd_id(5000, "p7"));
    poll();
    int peak = heard_peak();
    assert(peak > 4500 && peak < 5100);
    ptt_hang_up(id);
}

/* Fast presses and releases, some in one poll: only held audio goes up,
 * and nothing of the reply survives any press. */
static void ptt_rapid_edges(void)
{
    int64_t id = ptt_live_call();
    size_t taken = 0;
    for (int i = 0; i < 40; i++) {
        muse_voice_call_set_talking(true);
        muse_voice_call_set_talking(true);   /* repeated edges are ignored */
        taken += held(320);
        muse_voice_call_set_talking(false);
        muse_voice_call_set_talking(false);
        assert(held(320, 12000) == 0);
        line(id, cavd_id(8000, "p1"));
        if (i % 3 == 0) {
            poll(1, 10000);
        }
        if (i % 5 == 0) {
            assert(heard_peak() == 0);
        }
    }
    poll(300, 10000);
    assert(heard_peak() == 0);
    assert(taken == 40 * 320);
    /* Every held sample went up (resampled 16 -> 24 kHz), with quiet
     * between the holds, and nothing louder than the held mic. */
    size_t voiced = 0;
    for (const auto &a : appends(id)) {
        assert(muse_vc_peak(a.data(), a.size()) <= 6100);
        for (int16_t v : a) {
            voiced += v != 0;
        }
    }
    assert(voiced >= 40 * 480 - 400 && voiced <= 40 * 480 + 200);
    ptt_hang_up(id);
}

/* Presses and releases while the call opens: both holds go up, nothing between. */
static void ptt_edges_while_opening(void)
{
    muse_voice_call_start_ptt();
    held(4800);
    muse_voice_call_set_talking(false);
    assert(held(4800, 12000) == 0);
    poll();
    int64_t id = media_id();
    muse_voice_call_set_talking(true);
    held(4800);
    poll();
    head(id, 200);
    poll();
    assert(ptt_status().connecting);
    muse_voice_call_set_talking(false);
    assert(held(4800, 12000) == 0);
    char ev[512];
    snprintf(ev, sizeof(ev), SESSION_EVENT, call_id_of(id).c_str());
    line(id, ev);
    poll();
    expect_event(MUSE_VOICE_CALL_EV_READY);
    poll(200, 10000);
    size_t loud = 0;
    for (int p : append_peaks(id)) {
        assert(p <= 6100);
        loud += p > 1000;
    }
    /* 9600 held samples at 16 kHz = 14400 at 24 kHz = 15 appends. */
    assert(loud == 15 || loud == 16);
    ptt_hang_up(id);
}

/* The connection drops mid-hold: the call ends, nothing held is replayed,
 * and on reconnect only the old call's end-call goes, before a new call. */
static void ptt_disconnect(void)
{
    int64_t id = ptt_live_call();
    std::string old_id = call_id_of(id);
    muse_voice_call_set_talking(true);
    held(16000 * 3);
    poll(10, 10000);
    lose_connection();
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "LOST CONNECTION TO MUSE"));
    assert(!ptt_status().capture && held(320) == 0);
    muse_voice_call_set_talking(false);
    size_t sends = count("send");
    s_connected = true;
    poll(2, 1000000);
    auto ends = end_calls_for(old_id);
    assert(ends.size() == 1 && count("media") == 1 && (size_t)count("send") == sends);
    answer_end(ends[0].id);
    /* A new PTT call: none of the old hold's audio. */
    muse_voice_call_start_ptt();
    held(3200, 3000);
    muse_voice_call_set_talking(false);
    poll();
    int64_t fresh = media_id();
    assert(fresh != id);
    make_live(fresh);
    poll(200, 10000);
    size_t loud = 0;
    for (int p : append_peaks(fresh)) {
        assert(p <= 3100);
        loud += p > 1000;
    }
    assert(loud == 5 || loud == 6);
    ptt_hang_up(fresh);
    assert(end_calls_for(old_id).size() == 1);
}

/* Ending a PTT call while held, and a new call's generation fence. */
static void ptt_end_and_generations(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    held(3200);
    muse_voice_call_end();
    assert(held(320) == 0);
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(!st.capture && !st.talking && !st.live);
    /* A hands-free call's unsent mic, then a PTT call replacing it: none of
     * the old mic goes up in the new call. */
    s_ops.clear();
    int64_t hf = live_call();
    mic(8000, 12000);
    muse_voice_call_start_ptt();
    held(3200, 3000);
    muse_voice_call_set_talking(false);
    poll();
    serve_ending(hf);
    expect_event(MUSE_VOICE_CALL_EV_NONE);
    poll();
    int64_t next = media_id();
    assert(next != hf);
    make_live(next);
    poll(100, 10000);
    size_t loud = 0;
    for (int p : append_peaks(next)) {
        assert(p <= 3100);
        loud += p > 1000;
    }
    assert(loud == 5 || loud == 6);
    ptt_hang_up(next);
    /* Start, end before the session sees it, start again: one media, the
     * second call's hold only. */
    s_ops.clear();
    muse_voice_call_start_ptt();
    held(3200, 12000);
    muse_voice_call_end();
    char text[96];
    muse_voice_call_start_ptt();
    held(3200, 3000);
    muse_voice_call_set_talking(false);
    poll();
    assert(count("media") == 1);
    expect_event(MUSE_VOICE_CALL_EV_NONE, text);
    int64_t fresh = media_id();
    make_live(fresh);
    poll(100, 10000);
    for (int p : append_peaks(fresh)) {
        assert(p <= 3100);
    }
    ptt_hang_up(fresh);
}

/* A reply waiting for its turn: suppressed input discards it; an id with no
 * eligible speech, and audio without an id, never play; a reply that
 * outgrows the wait buffer is dropped whole and reported. */
static void ptt_waiting(void)
{
    int64_t id = ptt_live_call();
    line(id, cavd_id(8000, "q0"));      /* no hold has spoken yet */
    poll();
    assert(heard_peak() == 0);
    muse_voice_call_set_talking(true);
    held(3200);
    poll();
    line(id, ev_speech("i1"));
    muse_voice_call_set_talking(false);
    poll(30, 10000);
    line(id, cavd_id(8000, "q1"));
    line(id, ev_suppressed("i1"));
    line(id, ev_turn("s1", "i1"));      /* too late: the item was side speech */
    line(id, cavd_id(8000, "q1"));
    poll();
    assert(heard_peak() == 0);
    /* Too much before the turn: dropped whole, reported. */
    muse_voice_call_set_talking(true);
    held(3200);
    poll();
    line(id, ev_speech("i2"));
    muse_voice_call_set_talking(false);
    poll(30, 10000);
    for (int i = 0; i < 140; i++) {     /* 5.6 s */
        line(id, cavd_id(8000, "q2"));
    }
    poll();
    assert(ptt_status().reply_lost);
    line(id, ev_turn("s2", "i2"));
    line(id, cavd_id(8000, "q2"));
    poll(3);
    assert(heard_peak() == 0);
    /* A reply that played while held (the server endpointed mid-hold) waits
     * for the release, then plays. */
    muse_voice_call_set_talking(true);
    poll();
    assert(!ptt_status().reply_lost);
    held(3200);
    line(id, ev_speech("i3"));
    line(id, ev_turn("s3", "i3"));
    line(id, cavd_id(4000, "q3"));
    line(id, cavd_id(4000, "q3"));
    poll(3);
    assert(heard_peak() == 0);          /* nothing reaches the speaker while held */
    muse_voice_call_set_talking(false);
    poll(3);
    size_t n = 0;
    int peak = heard_peak(&n);
    assert(n == 2 * 640 && peak > 3500 && peak < 4100);
    /* Same again, but the caller keeps talking (new speech) before letting
     * go: that reply is barged in on, and its audio never plays. */
    muse_voice_call_set_talking(true);
    poll();
    held(3200);
    line(id, ev_speech("i4"));
    line(id, ev_turn("s4", "i4"));
    line(id, cavd_id(8000, "q4"));
    poll();
    line(id, ev_speech("i5"));
    line(id, cavd_id(8000, "q4"));
    muse_voice_call_set_talking(false);
    poll(10, 10000);
    assert(heard_peak() == 0);
    ptt_hang_up(id);
}

/* Muse holds the input mid-hold: nothing is taken or sent until it lets go;
 * a hands-free call shows no push-to-talk status. */
static void ptt_server_hold(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":true}");
    expect_event(MUSE_VOICE_CALL_EV_HOLD);
    poll();
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(st.hold && !st.capture && st.talking);
    size_t before = append_peaks(id).size();
    assert(held(3200, 12000) == 0);
    poll(50, 10000);
    assert(append_peaks(id).size() == before);
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":false}");
    expect_event(MUSE_VOICE_CALL_EV_RESUME);
    poll();
    assert(ptt_status().capture);
    assert(held(3200) == 3200);
    muse_voice_call_set_talking(false);
    poll(100, 10000);
    for (int p : append_peaks(id)) {
        assert(p <= 6100);
    }
    ptt_hang_up(id);
    /* Hands-free: no push-to-talk status, set_talking does nothing. */
    int64_t hf = live_call();
    muse_voice_call_set_talking(true);
    st = ptt_status();
    assert(!st.live && !st.talking && !st.capture && !st.connecting);
    muse_voice_call_end();
    serve_ending(hf);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

/* ---- Reply gate, directly ---- */

static void gate_ev(muse_vc_reply_gate_t *g, muse_vc_ev_kind_t kind, const char *item, const char *sub = nullptr)
{
    muse_vc_event_t ev = {};
    ev.kind = kind;
    ev.item_id = { item, strlen(item) };
    if (sub) {
        ev.submission_id = { sub, strlen(sub) };
    }
    muse_vc_reply_gate_event(g, &ev);
}

static muse_vc_gate_verdict_t gate_audio(muse_vc_reply_gate_t *g, const char *id)
{
    muse_vc_event_t ev = {};
    ev.kind = MUSE_VC_EV_AUDIO;
    ev.audio_cavd = ev.audio_rate_ok = true;
    ev.response_id = { id, strlen(id) };
    return muse_vc_reply_gate_audio(g, &ev);
}

/* Old ids stay stale however many new ones come; past the table's size the
 * gate fails closed (drops everything new) and says so. */
static void ptt_gate_capacity(void)
{
    static muse_vc_reply_gate_t g;
    muse_vc_reply_gate_init(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "old-item");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "old-item", "s0");
    assert(gate_audio(&g, "old-provider") == MUSE_VC_GATE_PLAY);
    muse_vc_reply_gate_press(&g);
    assert(gate_audio(&g, "old-provider") == MUSE_VC_GATE_DROP);
    for (int i = 0; i < MUSE_VC_GATE_IDS - 2; i++) {
        char id[32];
        snprintf(id, sizeof(id), "other-%d", i);
        assert(gate_audio(&g, id) == MUSE_VC_GATE_DROP);
    }
    assert(!muse_vc_reply_gate_full(&g));
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "new-item");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "new-item", "s1");
    assert(gate_audio(&g, "old-provider") == MUSE_VC_GATE_DROP);   /* still stale, 127 ids later */
    assert(gate_audio(&g, "other-3") == MUSE_VC_GATE_DROP);
    assert(gate_audio(&g, "fresh") == MUSE_VC_GATE_PLAY);          /* the 128th id */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "next-item");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "next-item", "s2");
    assert(gate_audio(&g, "one-too-many") == MUSE_VC_GATE_DROP);
    assert(muse_vc_reply_gate_full(&g));
    assert(gate_audio(&g, "old-provider") == MUSE_VC_GATE_DROP);
    assert(gate_audio(&g, "fresh") == MUSE_VC_GATE_DROP);           /* stale after the press */
    /* In a call: the call ends, visibly. */
    reset_world();
    int64_t id = ptt_live_call();
    for (int i = 0; i < MUSE_VC_GATE_IDS; i++) {
        char rid[32];
        snprintf(rid, sizeof(rid), "p-%d", i);
        line(id, cavd_id(8000, rid));
    }
    poll();
    assert(heard_peak() == 0);
    assert(count("end_call") == 0);
    line(id, cavd_id(8000, "p-overflow"));
    serve_ending(id);
    char text[96];
    expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    assert(!strcmp(text, "CALL FULL, CALL AGAIN"));
    assert(count("end_call") == 1);
}

/* The current-turn rule: the latest item's own turn confirms it, once;
 * then any fresh id plays. A turn naming another item blocks fresh ids (and
 * drops what waits) until the current item is named again. */
static void ptt_gate_latest_item(void)
{
    muse_session_call_wake();   /* (a pure gate case: the harness expects the session woken) */
    static muse_vc_reply_gate_t g;
    muse_vc_reply_gate_init(&g);
    muse_vc_reply_gate_press(&g);
    /* Unconfirmed: a fresh id waits; a newer one waits instead (flushing it). */
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-A");
    assert(gate_audio(&g, "a0") == MUSE_VC_GATE_HOLD);
    assert(muse_vc_reply_gate_take_flush(&g) == 0);
    assert(gate_audio(&g, "a1") == MUSE_VC_GATE_HOLD);
    assert(muse_vc_reply_gate_take_flush(&g) == MUSE_VC_GATE_FLUSH_WAITING);
    assert(gate_audio(&g, "a0") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA");
    assert(gate_audio(&g, "a1") == MUSE_VC_GATE_PLAY);
    /* Confirmed: a continuation with no turn of its own plays and takes
     * over; the older reply goes stale and its queue is flushed. */
    assert(gate_audio(&g, "a2") == MUSE_VC_GATE_PLAY);
    assert(muse_vc_reply_gate_take_flush(&g) == MUSE_VC_GATE_FLUSH_PLAYING);
    assert(gate_audio(&g, "a1") == MUSE_VC_GATE_DROP);
    assert(gate_audio(&g, "a2") == MUSE_VC_GATE_PLAY);   /* its later audio too */
    /* Its alias turn for the current item confirms it. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA2");
    assert(gate_audio(&g, "a2") == MUSE_VC_GATE_PLAY);
    /* Older work's turn: a confirmed reply goes on; fresh ids after it don't. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-Z", "sZ");
    assert(muse_vc_reply_gate_take_flush(&g) == 0);
    assert(gate_audio(&g, "a2") == MUSE_VC_GATE_PLAY);
    assert(gate_audio(&g, "z1") == MUSE_VC_GATE_DROP);
    /* The current item's alias turn: fresh ids play again; z1 stays stale. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA3");
    assert(gate_audio(&g, "a3") == MUSE_VC_GATE_PLAY);    /* this turn's own */
    assert(gate_audio(&g, "z1") == MUSE_VC_GATE_DROP);
    /* A continuation whose audio comes before its alias, which names older
     * work: it was that work's reply: stale, flushed. */
    assert(gate_audio(&g, "c9") == MUSE_VC_GATE_PLAY);
    muse_vc_reply_gate_take_flush(&g);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-Z", "sZ2");
    assert(muse_vc_reply_gate_take_flush(&g) == MUSE_VC_GATE_FLUSH_PLAYING);
    assert(gate_audio(&g, "c9") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA4");
    assert(gate_audio(&g, "a4") == MUSE_VC_GATE_PLAY);
    /* Speech on B, then A's delayed turn before B's: B's waiting audio is
     * dropped (it may be A's reply), and nothing plays until B's turn. */
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-B");
    assert(gate_audio(&g, "a4") == MUSE_VC_GATE_DROP);    /* barged in on */
    assert(gate_audio(&g, "b1") == MUSE_VC_GATE_HOLD);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA3");
    assert(!muse_vc_reply_gate_waiting(&g));
    assert(gate_audio(&g, "b1") == MUSE_VC_GATE_DROP);
    assert(gate_audio(&g, "x1") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-B", "sB");
    assert(gate_audio(&g, "b2") == MUSE_VC_GATE_PLAY);
    /* A turn without a whole identity does nothing. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-Q");
    assert(gate_audio(&g, "b3") == MUSE_VC_GATE_PLAY);
    /* A press: everything stale; unconfirmed again until a new turn. */
    muse_vc_reply_gate_press(&g);
    assert(gate_audio(&g, "b3") == MUSE_VC_GATE_DROP && gate_audio(&g, "n1") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-B", "sB2");   /* the old item's turn, no new speech */
    assert(gate_audio(&g, "n2") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-C");
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-D");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-C", "sC");   /* the earlier item of this hold */
    assert(gate_audio(&g, "c1") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_INPUT_SUPPRESSED, "item-D");     /* the latest was side speech */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-C", "sC2");
    assert(gate_audio(&g, "c2") == MUSE_VC_GATE_DROP);       /* C doesn't become current again */
}

static void gate_response(muse_vc_reply_gate_t *g, muse_vc_ev_kind_t kind, const char *id)
{
    muse_vc_event_t ev = {};
    ev.kind = kind;
    ev.mapper_response_id = { id, strlen(id) };
    muse_vc_reply_gate_event(g, &ev);
}

/* Older work reporting back once the current reply has finished (a spoken
 * approval: ask, answer "yes", the task's result) plays after it; while the
 * current reply is still going, or after a press with no speech, it doesn't. */
static void ptt_gate_follow_up(void)
{
    muse_session_call_wake();   /* (a pure gate case: the harness expects the session woken) */
    static muse_vc_reply_gate_t g;
    muse_vc_reply_gate_init(&g);
    muse_vc_reply_gate_press(&g);
    /* The request, and Muse asking for approval. */
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-A");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mA");
    assert(gate_audio(&g, "a1") == MUSE_VC_GATE_PLAY);
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mA");
    /* "Yes": a press, a new item, a short acknowledgement. */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-B");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-B", "sB");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mB");
    assert(gate_audio(&g, "b1") == MUSE_VC_GATE_PLAY);
    /* A's result before B's reply finished: dropped, as before. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA2");
    assert(gate_audio(&g, "z1") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-B", "sB2");   /* B current again */
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mB2");
    assert(gate_audio(&g, "b2") == MUSE_VC_GATE_PLAY);
    muse_vc_reply_gate_take_flush(&g);
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mB2");
    /* B's reply done: A's result plays, B's queued audio isn't flushed and
     * B takes no new audio; A's reply shows captions and owns its completion. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA3");
    assert(muse_vc_reply_gate_take_flush(&g) == 0);
    assert(muse_vc_reply_gate_playing(&g));
    assert(muse_vc_reply_gate_captions(&g));
    assert(gate_audio(&g, "b2") == MUSE_VC_GATE_PLAY);   /* B's trailing audio still plays */
    assert(gate_audio(&g, "a9") == MUSE_VC_GATE_PLAY);
    assert(muse_vc_reply_gate_take_flush(&g) == 0);       /* what B queued plays first */
    assert(gate_audio(&g, "b2") == MUSE_VC_GATE_DROP);   /* then B stops: never interleaved */
    assert(gate_audio(&g, "a9") == MUSE_VC_GATE_PLAY);
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mA3");
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mA3");
    {
        muse_vc_event_t done = {};
        done.kind = MUSE_VC_EV_RESPONSE_DONE;
        done.mapper_response_id = { "mA3", 3 };
        assert(muse_vc_reply_gate_done(&g, &done));
    }
    /* A press stops it for good, and with no speech nothing opens again. */
    muse_vc_reply_gate_press(&g);
    assert(gate_audio(&g, "a9") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-B", "sB3");
    assert(gate_audio(&g, "n1") == MUSE_VC_GATE_DROP);
    /* Speech that hasn't been confirmed: older work's turn still blocks. */
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-C");
    assert(gate_audio(&g, "c1") == MUSE_VC_GATE_HOLD);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-A", "sA4");
    assert(gate_audio(&g, "c1") == MUSE_VC_GATE_DROP && gate_audio(&g, "x1") == MUSE_VC_GATE_DROP);

    /* Older work whose audio came before its turn: that continuation was its
     * reply, and plays on once the turn names it. */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-D");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-D", "sD");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mD");
    assert(gate_audio(&g, "d1") == MUSE_VC_GATE_PLAY);
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mD");
    assert(gate_audio(&g, "e1") == MUSE_VC_GATE_PLAY);   /* no turn of its own yet */
    muse_vc_reply_gate_take_flush(&g);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-E", "sE");
    assert(muse_vc_reply_gate_take_flush(&g) == 0);
    assert(gate_audio(&g, "e1") == MUSE_VC_GATE_PLAY);
    assert(muse_vc_reply_gate_captions(&g));
    /* The latest hold's own continuation alias: D is current again. */
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-D", "sD2");
    assert(muse_vc_reply_gate_take_flush(&g) == 0);   /* E's reply stops after what it queued */
    assert(gate_audio(&g, "e1") == MUSE_VC_GATE_DROP);
    assert(muse_vc_reply_gate_captions(&g));
    assert(gate_audio(&g, "d2") == MUSE_VC_GATE_PLAY);

    /* An earlier item of the same hold is never adopted. */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-F");
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-G");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-G", "sG");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mG");
    assert(gate_audio(&g, "g1") == MUSE_VC_GATE_PLAY);
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mG");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-F", "sF");
    assert(gate_audio(&g, "f1") == MUSE_VC_GATE_DROP);

    /* On the ordinary path a finished reply's trailing audio stops once the
     * next reply plays, without a flush. */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-H");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-H", "sH");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mH");
    assert(gate_audio(&g, "h1") == MUSE_VC_GATE_PLAY);
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mH");
    muse_vc_reply_gate_take_flush(&g);   /* (anything left from the cases above) */
    assert(gate_audio(&g, "h1") == MUSE_VC_GATE_PLAY);
    assert(gate_audio(&g, "h2") == MUSE_VC_GATE_PLAY);
    assert(muse_vc_reply_gate_take_flush(&g) == 0);
    assert(gate_audio(&g, "h1") == MUSE_VC_GATE_DROP);
    assert(gate_audio(&g, "h2") == MUSE_VC_GATE_PLAY);

    /* A turn whose first response played nothing (a tool call) hasn't
     * answered yet: older work still waits its turn. */
    muse_vc_reply_gate_press(&g);
    gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, "item-J");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-J", "sJ");
    gate_response(&g, MUSE_VC_EV_RESPONSE_CREATED, "mJ");
    gate_response(&g, MUSE_VC_EV_RESPONSE_DONE, "mJ");
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-H", "sH2");
    assert(gate_audio(&g, "k1") == MUSE_VC_GATE_DROP);

    /* A long hold: the newest item is still the one a turn confirms, even
     * past the list's size. */
    muse_vc_reply_gate_press(&g);
    const char *items[] = {"item-1", "item-2", "item-3", "item-4", "item-5"};
    for (const char *it : items) {
        gate_ev(&g, MUSE_VC_EV_SPEECH_STARTED, it);
    }
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-4", "s4");
    assert(gate_audio(&g, "x4") == MUSE_VC_GATE_DROP);
    gate_ev(&g, MUSE_VC_EV_TURN_STARTED, "item-5", "s5");
    assert(gate_audio(&g, "x5") == MUSE_VC_GATE_PLAY);
}

/* The current reply's completion is its own: an older response.done or
 * turn_settled doesn't mark it done. */
static void ptt_stale_completion(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    line(id, ev_created("mapper-old"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(3000, "provider-old"));
    assert(heard_peak() > 2500);
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i2"));
    line(id, ev_turn("s2", "i2"));
    line(id, ev_created("mapper-new"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(3000, "provider-new"));
    assert(heard_peak() > 2500);
    assert(!ptt_status().reply_done);
    line(id, ev_done("mapper-old"));
    line(id, ev_settled("s1"));
    line(id, ev_turn("s1b", "i1"));     /* a delayed alias for the old item */
    line(id, ev_created("mapper-alias"));
    line(id, ev_done("mapper-alias"));
    line(id, ev_settled("s1b"));
    poll();
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(st.reply_open && !st.reply_done);
    line(id, ev_done("mapper-new"));
    poll();
    assert(ptt_status().reply_done);
    /* Astra's case: an older submission settles while the current reply
     * (no response.created seen for either) plays. */
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i4"));
    line(id, ev_turn("s4", "i4"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(3000, "provider-4"));
    assert(heard_peak() > 2500);
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i5"));
    line(id, ev_turn("s5", "i5"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(3000, "provider-5"));
    assert(heard_peak() > 2500);
    assert(!ptt_status().reply_done);
    line(id, ev_settled("s4"));
    poll();
    st = ptt_status();
    assert(st.reply_open && !st.reply_done);
    /* And by settlement alone. */
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i3"));
    line(id, ev_turn("s3", "i3"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(3000, "provider-3"));
    assert(heard_peak() > 2500);
    line(id, ev_settled("s2"));
    poll();
    assert(!ptt_status().reply_done);
    line(id, ev_settled("s3"));
    poll();
    assert(ptt_status().reply_done);
    ptt_hang_up(id);
}

/* A long input hold, then resume: quiet goes on at one append per 40 ms,
 * no burst to catch up. */
static void ptt_hold_clock_gap(void)
{
    int64_t id = ptt_live_call();
    poll(100, 10000);
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":true}");
    expect_event(MUSE_VOICE_CALL_EV_HOLD);
    poll(1, 5000000);
    line(id, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":false}");
    expect_event(MUSE_VOICE_CALL_EV_RESUME);
    size_t before = append_peaks(id).size();
    poll(100, 0);                       /* no time passes */
    size_t burst = append_peaks(id).size() - before;
    assert(burst <= 1);
    poll(100, 10000);                   /* 1 s */
    size_t paced = append_peaks(id).size() - before;
    assert(paced >= 24 && paced <= 27);
    /* Same after a stalled loop with no hold. */
    before = append_peaks(id).size();
    poll(1, 3000000);
    poll(50, 0);
    assert(append_peaks(id).size() - before <= 2);
    ptt_hang_up(id);
}

/* A PTT start replaced before the session takes it (start_ptt, end, start):
 * the hands-free call after it plays normally; and a PTT call after a
 * replaced PTT start still fences its own presses. */
static void ptt_abandoned_start(void)
{
    muse_voice_call_start_ptt();
    muse_voice_call_end();
    muse_voice_call_start();
    poll();
    assert(count("media") == 1);
    int64_t id = media_id();
    make_live(id);
    for (int i = 0; i < 5; i++) {
        line(id, cavd(6000));
    }
    size_t n = 0;
    assert(heard_peak(&n) > 5500 && n == 5 * 640);
    muse_voice_call_ptt_status_t st = ptt_status();
    assert(!st.live && !st.talking);
    muse_voice_call_end();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    /* start_ptt replaced by start_ptt: the second's press fence works. */
    s_ops.clear();
    muse_voice_call_start_ptt();
    muse_voice_call_start_ptt();
    held(1600);
    muse_voice_call_set_talking(false);
    poll();
    assert(count("media") == 1);
    id = media_id();
    make_live(id);
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    muse_voice_call_set_talking(false);
    poll(60, 10000);
    line(id, cavd_id(5000, "p1"));
    int peak = heard_peak();
    assert(peak > 4500 && peak < 5100);
    line(id, cavd_id(5000, "p1"));
    muse_voice_call_set_talking(true);
    bool flushed = false;
    assert(heard_peak(&n, &flushed) == 0 && n == 0 && flushed);
    muse_voice_call_set_talking(false);
    ptt_hang_up(id);
}

/* 2 s held before the call is live and 2 s after, then a release: the held
 * audio goes up as fast as pacing allows, and quiet follows at once, not
 * once real time catches up with it. */
static void ptt_quiet_after_early_audio(void)
{
    muse_voice_call_start_ptt();
    assert(held(32000) == 32000);
    poll();
    int64_t id = media_id();
    make_live(id);
    for (int i = 0; i < 200; i++) {     /* 2 s live, holding */
        held(160);
        poll(1, 10000);
    }
    muse_voice_call_set_talking(false);
    int64_t released = s_now;
    size_t loud_end = 0;
    for (int i = 0; i < 300 && !loud_end; i++) {
        poll(1, 10000);
        std::vector<int> peaks = append_peaks(id);
        size_t last_loud = 0;
        for (size_t k = 0; k < peaks.size(); k++) {
            if (peaks[k] > 1000) {
                last_loud = k + 1;
            }
        }
        if (last_loud && peaks.size() >= last_loud + 5) {
            loud_end = last_loud;
        }
    }
    assert(loud_end >= 99 && loud_end <= 101);   /* 4 s of held audio */
    assert(s_now - released <= 400000);           /* 5 quiet appends follow within 0.4 s */
    ptt_hang_up(id);
}

/* The old call never takes the new call's audio: a server hold on the old
 * call, after the voice task started a new one and pushed its first words,
 * drains only the old call's mic. */
static void ptt_cutover(void)
{
    int64_t old = live_call();
    mic(3200, 12000);                   /* the old call's own unsent mic */
    muse_voice_call_start_ptt();
    assert(held(4800, 3000) == 4800);   /* the new call's first words */
    line(old, "{\"type\":\"hatch.voice.switch_input_hold\",\"event_id\":\"e\",\"held\":true}");
    muse_voice_call_set_talking(false);
    poll();
    serve_ending(old);
    poll();
    int64_t fresh = media_id();
    assert(fresh != old);
    make_live(fresh);
    poll(100, 10000);
    size_t loud = 0;
    for (int p : append_peaks(fresh)) {
        assert(p <= 3100);              /* none of the old call's mic */
        loud += p > 1000;
    }
    assert(loud == 7 || loud == 8);     /* all 4800 samples: 7.5 appends */
    for (int p : append_peaks(old)) {
        assert(p > 3100 || p < 1000);   /* the old call never sent the new call's audio */
    }
    ptt_hang_up(fresh);
}

/* Many fast edges while the call connects, more than any queue held: the
 * last one wins, for release and for press. */
static void ptt_many_edges_while_connecting(void)
{
    for (int last_held = 0; last_held < 2; last_held++) {
        s_ops.clear();
        muse_voice_call_start_ptt();
        for (int i = 0; i < 20; i++) {
            held(160);
            muse_voice_call_set_talking(false);
            muse_voice_call_set_talking(true);
        }
        held(160);
        if (!last_held) {
            muse_voice_call_set_talking(false);
        }
        poll();
        int64_t id = media_id();
        make_live(id);
        poll(150, 10000);
        muse_voice_call_ptt_status_t st = ptt_status();
        assert(st.talking == (bool)last_held && st.capture == (bool)last_held);
        assert(st.sending == (bool)last_held);   /* released: held audio and tail are up */
        size_t before = append_peaks(id).size();
        poll(50, 10000);
        size_t more = append_peaks(id).size() - before;
        if (last_held) {
            assert(more == 0);          /* held, nothing more said: nothing to send */
            muse_voice_call_set_talking(false);
            poll(50, 10000);
            assert(append_peaks(id).size() > before + 10);
        } else {
            assert(more >= 11 && more <= 14);   /* quiet keeps going */
        }
        ptt_hang_up(id);
    }
}

/* A turn_started with a missing or conflicting identity fails the call;
 * the reply audio waiting for it never plays. */
static void ptt_bad_turn_identity(void)
{
    for (int variant = 0; variant < 2; variant++) {
        s_ops.clear();
        int64_t id = ptt_live_call();
        muse_voice_call_set_talking(true);
        poll();
        line(id, ev_speech("i1"));
        if (variant == 1) {
            line(id, ev_turn("s1", "i0"));   /* s1 belongs to i0 */
        }
        muse_voice_call_set_talking(false);
        poll(30, 10000);
        for (int i = 0; i < 5; i++) {
            line(id, cavd_id(8000, "p1"));
        }
        if (variant == 0) {
            line(id, "{\"type\":\"hatch.voice.turn_started\",\"event_id\":\"e\",\"item_id\":\"i1\"}");
        } else {
            line(id, ev_turn("s1", "i1"));  /* the same submission, another item */
        }
        line(id, cavd_id(8000, "p1"));
        for (int i = 0; i < 20; i++) {
            poll(1, 10000);
            assert(heard_peak() == 0);
        }
        poll();
        int64_t end = end_call_id();
        if (end && is_open(end)) {
            answer_end(end);
        }
        poll(3);
        if (is_open(id)) {
            stream_end(id, true, 200);
        }
        poll(3);
        assert(heard_peak() == 0);
        char text[96];
        expect_event(MUSE_VOICE_CALL_EV_ENDED, text);
    }
}

/* Several provider responses in one confirmed turn (a tool continuation
 * with no alias turn): the newest takes over at once, the older one's
 * queued and trailing audio is dropped, so they never interleave. A press
 * sends response.cancel for the open reply, once, before its mic audio. */
static void ptt_playing_while_waiting(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    line(id, ev_created("resp_one"));
    muse_voice_call_set_talking(false);
    poll(30, 10000);
    line(id, cavd_id(2000, "p1"));
    size_t n = 0;
    assert(heard_peak(&n) > 1800 && n == 640);
    line(id, cavd_id(2000, "p1"));      /* queued, not read yet */
    line(id, cavd_id(6000, "p2"));      /* the continuation: takes over */
    line(id, cavd_id(2000, "p1"));      /* p1's trailing audio: dropped */
    line(id, cavd_id(6000, "p2"));
    std::vector<int16_t> got;
    bool flushed = false;
    read_all(&got, &flushed);
    poll();
    read_all(&got, &flushed);
    assert(flushed && got.size() == 2 * 640);
    assert(muse_vc_peak(got.data() + 4, got.size() - 4) > 5500);
    for (size_t k = 8; k < got.size(); k += 320) {
        assert(muse_vc_peak(got.data() + k, 160) > 2500);   /* only p2: no p1 chunk in between */
    }
    /* Press mid-reply: response.cancel for resp_one first, then mic audio. */
    size_t before = s_ops.size();
    muse_voice_call_set_talking(true);
    held(1600);
    poll(5, 10000);
    std::vector<std::string> sent;
    for (size_t k = before; k < s_ops.size(); k++) {
        if (s_ops[k].op == "send" && s_ops[k].id == id) {
            sent.push_back(s_ops[k].body);
        }
    }
    assert(!sent.empty() && sent[0] == "{\"type\":\"response.cancel\",\"response_id\":\"resp_one\"}\n");
    size_t cancels = 0;
    for (const std::string &b : sent) {
        cancels += b.find("response.cancel") != std::string::npos;
    }
    assert(cancels == 1);
    muse_voice_call_set_talking(false);
    poll(5, 10000);
    /* A press after the reply's response.done sends none. */
    line(id, ev_speech("i2"));
    line(id, ev_turn("s2", "i2"));
    line(id, ev_created("resp_two"));
    line(id, ev_done("resp_two"));
    before = s_ops.size();
    muse_voice_call_set_talking(true);
    poll(3, 10000);
    for (size_t k = before; k < s_ops.size(); k++) {
        assert(s_ops[k].body.find("response.cancel") == std::string::npos);
    }
    muse_voice_call_set_talking(false);
    poll(3, 10000);
    /* Nor one with no reply ever created (silent press, no turn). */
    before = s_ops.size();
    muse_voice_call_set_talking(true);
    poll(3, 10000);
    for (size_t k = before; k < s_ops.size(); k++) {
        assert(s_ops[k].body.find("response.cancel") == std::string::npos);
    }
    muse_voice_call_set_talking(false);
    ptt_hang_up(id);
}

/* Reply audio queued while held, then the voice task hangs up: none of it
 * plays while the call ends. */
static void ptt_end_while_held_reply(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    for (int i = 0; i < 5; i++) {
        line(id, cavd_id(8000, "p1"));   /* the server endpointed mid-hold: waits for the release */
    }
    poll();
    assert(heard_peak() == 0);
    muse_voice_call_end();
    for (int i = 0; i < 5; i++) {
        poll();
        assert(heard_peak() == 0);
    }
    serve_ending(id);
    assert(heard_peak() == 0);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

/* Reply captions come only while Muse answers the last hold; a press clears
 * the old reply's caption. */
static void ptt_captions(void)
{
    int64_t id = ptt_live_call();
    char text[96];
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\"stale words\"}");
    poll();
    assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE);
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    muse_voice_call_set_talking(false);
    poll(30, 10000);
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\"Hello\"}");
    expect_event(MUSE_VOICE_CALL_EV_REPLY, text);
    assert(!strcmp(text, "Hello"));
    muse_voice_call_set_talking(true);
    poll();
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\" there\"}");
    assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE);
    line(id, ev_speech("i2"));
    line(id, ev_turn("s2", "i2"));
    muse_voice_call_set_talking(false);
    poll(30, 10000);
    line(id, "{\"type\":\"response.output_audio_transcript.delta\",\"event_id\":\"e\",\"delta\":\"New\"}");
    expect_event(MUSE_VOICE_CALL_EV_REPLY, text);
    assert(!strcmp(text, "New"));       /* not "Hello New" */
    ptt_hang_up(id);
}

/* The voice task hangs up while the session is mid-pump (after it took
 * its commands): no append goes after that, held or quiet; the ended call's
 * leftover mic never reaches the next call, whose own first words do. */
static void ptt_end_mid_pump(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    held(16000 * 2, 12000);             /* 2 s held, queued */
    size_t before = append_peaks(id).size();
    s_end_after_sends = 2;
    s_now += 2000000;                   /* pacing would allow many appends in this poll */
    assert(muse_voice_call_poll());
    s_end_after_sends = -1;
    size_t after_pump = append_peaks(id).size();
    assert(after_pump == before + 2);   /* the two before the hang-up, none after */
    poll();
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(append_peaks(id).size() == after_pump);
    /* Hands-free: the same. */
    s_ops.clear();
    int64_t hf = live_call();
    mic(16000, 12000);
    s_end_after_sends = 1;
    s_now += 2000000;
    assert(muse_voice_call_poll());
    s_end_after_sends = -1;
    assert(append_peaks(hf).size() == 1);
    serve_ending(hf);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(append_peaks(hf).size() == 1);
    /* The next call: only its own held audio. */
    s_ops.clear();
    muse_voice_call_start_ptt();
    assert(held(4800, 3000) == 4800);
    muse_voice_call_set_talking(false);
    poll();
    int64_t fresh = media_id();
    make_live(fresh);
    poll(100, 10000);
    size_t loud = 0;
    for (int p : append_peaks(fresh)) {
        assert(p <= 3100);              /* none of the ended calls' 12000 */
        loud += p > 1000;
    }
    assert(loud == 7 || loud == 8);     /* all 4800 samples */
    ptt_hang_up(fresh);
}

/* Muse's stop drains confirmed reply audio queued beyond s_out as well. */
static void ptt_stop_drains_deferred(void)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    muse_voice_call_set_talking(false);
    poll();
    assert(heard_peak() == 0);   /* complete the press fence */
    for (int i = 0; i < 120; i++) {
        line(id, cavd_id(8000, "p1"));   /* 76800 samples: s_out holds only 64000 */
    }
    int16_t first[640];
    bool flushed;
    size_t before = muse_voice_call_read(first, 640, &flushed);
    assert(before == 640 && !flushed && muse_vc_peak(first, before) > 7000);
    const char *stop_line = "{\"type\":\"conversation.item.added\",\"item\":{\"type\":\"session_control\",\"name\":\"stop\"}}";
    line(id, stop_line);
    line(id, stop_line);   /* duplicates and trailing non-audio must not discard the goodbye */
    line(id, ev_done("mapper1"));
    line(id, cavd_id(8000, "p1"));   /* the confirmed reply's trailing frame */
    poll();
    assert(count("end_call") == 1 && count("reset", id) == 0);
    size_t after = 0;
    assert(heard_peak(&after) > 7000);
    fprintf(stderr, "ptt_stop_drain: before=%zu after=%zu expected_total=%d\n", before, after, 121 * 640);
    assert(before + after == 121 * 640);
    serve_ending(id);
    expect_event(MUSE_VOICE_CALL_EV_ENDED);
}

/* Stops never promote an unconfirmed or held reply, nor override a local end. */
static void ptt_stop_discards_ineligible(void)
{
    for (int variant = 0; variant < 6; variant++) {
        s_ops.clear();
        int64_t id = ptt_live_call();
        muse_voice_call_set_talking(true);
        poll();
        line(id, ev_speech("i1"));
        if (variant != 0) {
            line(id, ev_turn("s1", "i1"));
        }
        if (variant != 1) {
            muse_voice_call_set_talking(false);
            poll();
        }
        assert(heard_peak() == 0);
        for (int i = 0; i < 120; i++) {
            line(id, cavd_id(8000, "p1"));
        }
        if (variant == 2) {
            muse_voice_call_end();
            poll();
        } else if (variant == 3) {
            lose_connection();
        }
        line(id, "{\"type\":\"conversation.item.added\",\"item\":{\"type\":\"session_control\",\"name\":\"stop\"}}");
        if (variant == 4) {
            muse_voice_call_end();   /* local hang-up after Muse's stop still wins */
            poll();
        } else if (variant == 5) {
            lose_connection();
        }
        line(id, cavd_id(8000, "p1"));
        size_t after = 0;
        int peak = heard_peak(&after);
        /* Local end preserves the pre-existing s_out policy, not s_defer. */
        if (variant >= 2) {
            assert(after <= 64000);
        } else {
            assert(peak == 0 && after == 0);
        }
        fprintf(stderr, "ptt_stop_ineligible: variant=%d after=%zu\n", variant, after);
        if (variant != 3 && variant != 5) {
            serve_ending(id);
            expect_event(MUSE_VOICE_CALL_EV_ENDED);
        } else {
            expect_event(MUSE_VOICE_CALL_EV_ENDED);
            s_connected = true;
            poll(2, 1000000);
            answer_end(end_call_id());   /* settle the fake reconnect cleanup before the next case */
            poll();
            assert(!muse_voice_call_busy());
        }
    }
}

/* A 20 ms speaker consumes the queued reply after the end-call ACK. */
static void ptt_stop_no_progress_deadline(void)
{
    for (int eof = 0; eof < 2; eof++) {
        s_ops.clear();
        int64_t id = ptt_live_call();
        muse_voice_call_set_talking(true);
        poll();
        line(id, ev_speech("i1"));
        line(id, ev_turn("s1", "i1"));
        muse_voice_call_set_talking(false);
        poll();
        assert(heard_peak() == 0);
        for (int i = 0; i < 125; i++) line(id, cavd_id(8000, "p1"));
        line(id, "{\"type\":\"conversation.item.added\",\"item\":{\"type\":\"session_control\",\"name\":\"stop\"}}");
        poll();
        answer_end(end_call_id());
        poll();
        if (eof) { stream_end(id, true, 200); poll(); }
        char text[96];
        assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE && muse_voice_call_busy());
        int64_t start = s_now;
        poll(1, 4900000);
        assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE && muse_voice_call_busy());
        poll(1, 200000);
        expect_event(MUSE_VOICE_CALL_EV_ENDED);
        assert(!muse_voice_call_busy());
        size_t samples = read_all();
        assert(samples <= 64000);   /* the deferred ring expired, not silently kept */
        poll(3);
        assert(read_all() == 0);
        fprintf(stderr, "ptt_ack_deadline: eof=%d elapsed_us=%lld samples=%zu busy=0\n",
                eof, (long long)(s_now - start), samples);
    }
}

static void ptt_stop_paced_interrupts(void)
{
    for (int variant = 0; variant < 6; variant++) {
        s_ops.clear();
        int64_t id = ptt_live_call();
        muse_voice_call_set_talking(true);
        poll();
        line(id, ev_speech("i1"));
        if (variant != 4) line(id, ev_turn("s1", "i1"));
        if (variant != 5) { muse_voice_call_set_talking(false); poll(); }
        assert(heard_peak() == 0);
        for (int i = 0; i < 125; i++) line(id, cavd_id(8000, "p1"));
        line(id, "{\"type\":\"conversation.item.added\",\"item\":{\"type\":\"session_control\",\"name\":\"stop\"}}");
        poll();
        answer_end(end_call_id());
        poll();
        stream_end(id, true, 200);
        poll();
        if (variant == 0) { muse_voice_call_end(); poll(); }
        else if (variant == 1) { muse_voice_call_set_talking(true); poll(); }
        else if (variant == 2) { muse_voice_call_start(); poll(); }
        else if (variant == 3) lose_connection();
        line(id, cavd_id(12000, "p1"));
        size_t samples = read_all();
        poll();
        samples += read_all();
        assert(samples <= (variant == 0 || variant == 3 ? 64000u : 0u));
        fprintf(stderr, "ptt_ack_interrupt: variant=%d samples=%zu\n", variant, samples);
        if (variant == 2) {
            char text[96];
            assert(next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_NONE);
            int64_t fresh = media_id();
            assert(fresh && fresh != id);
            make_live(fresh);
            assert(read_all() == 0);
            muse_voice_call_end();
            serve_ending(fresh);
        }
        expect_event(MUSE_VOICE_CALL_EV_ENDED);
        assert(!muse_voice_call_busy());
        s_connected = true;
        poll();
    }
}

static void ptt_stop_paced(int order, int frames)
{
    int64_t id = ptt_live_call();
    muse_voice_call_set_talking(true);
    poll();
    line(id, ev_speech("i1"));
    line(id, ev_turn("s1", "i1"));
    muse_voice_call_set_talking(false);
    poll();
    assert(heard_peak() == 0);
    for (int i = 0; i < frames; i++) line(id, cavd_id(8000, "p1"));
    const char *stop_line = "{\"type\":\"conversation.item.added\",\"item\":{\"type\":\"session_control\",\"name\":\"stop\"}}";
    line(id, stop_line);
    poll();
    if (order == 2) stream_end(id, true, 200);   /* EOF before ACK */
    answer_end(end_call_id());
    poll();
    line(id, cavd_id(12000, "p1"));   /* half-closed: new wire audio must be refused */
    line(id, stop_line);
    line(id, ev_done("mapper1"));
    if (order == 1) { stream_end(id, true, 200); poll(); }   /* ACK before EOF */
    size_t samples = 0;
    bool early_end = false, ended = false;
    int64_t ended_at = 0, start = s_now;
    for (int i = 0; i < 500; i++) {
        char text[96];
        if (next_event(text, sizeof(text)) == MUSE_VOICE_CALL_EV_ENDED) {
            ended = true;
            ended_at = s_now;
            early_end |= samples < (size_t)frames * 640;
        }
        /* The production voice consumer has only a 2 s tail after ENDED. */
        if (ended && s_now - ended_at >= 2000000) break;
        int16_t pcm[320];
        bool flushed;
        size_t n = muse_voice_call_read(pcm, 320, &flushed);
        assert(!flushed);
        if (n) assert(muse_vc_peak(pcm, n) < 9000);   /* none of the late 12000 tone */
        samples += n;
        poll(1, 20000);
        if (samples == (size_t)frames * 640 && (!muse_voice_call_busy() || order == 0)) break;
    }
    fprintf(stderr, "ptt_ack_drain: order=%d frames=%d samples=%zu expected=%d early_end=%d elapsed_us=%lld\n",
            order, frames, samples, frames * 640, early_end, (long long)(s_now - start));
    assert(samples == (size_t)frames * 640 && !early_end);
    if (order == 0) { stream_end(id, true, 200); poll(); }
    if (!ended) expect_event(MUSE_VOICE_CALL_EV_ENDED);
    assert(!muse_voice_call_busy() && read_all() == 0);
}

static void ptt_stop_paced_ack_only(void) { ptt_stop_paced(0, 125); }
static void ptt_stop_paced_ack_eof(void) { ptt_stop_paced(1, 125); }
static void ptt_stop_paced_eof_ack(void) { ptt_stop_paced(2, 125); }
static void ptt_stop_paced_output_only(void) { ptt_stop_paced(1, 75); }

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(muse_voice_call_init());
    assert(muse_voice_call_ready());
    void (*const cases[])(void) = { happy, barge_in, hold, rejected, dropped_before_head, post_head_error,
                                    connection_lost, send_fails, no_answer, idle, stop, remote_end, generations,
                                    unreachable, hang_up_before_start, long_line, backpressure,
                                    end_call_unconfirmed, hold_race, long_speech, pre_ready_identity,
                                    mute_partial, cleanup_before_new_call, cleanup_stale_answer, cleanup_bounded,
                                    cleanup_expires, cleanup_gate_inflight, cleanup_4xx_then_lost, ptt_startup,
                                    ptt_overflow, ptt_press_during_reply, ptt_silent_press, ptt_rapid_edges,
                                    ptt_edges_while_opening, ptt_disconnect, ptt_end_and_generations, ptt_waiting,
                                    ptt_server_hold, ptt_gate_capacity, ptt_gate_latest_item, ptt_gate_follow_up,
                                    ptt_stale_completion, ptt_hold_clock_gap, ptt_abandoned_start,
                                    ptt_quiet_after_early_audio, ptt_cutover, ptt_many_edges_while_connecting,
                                    ptt_bad_turn_identity, ptt_playing_while_waiting, ptt_captions,
                                    ptt_end_while_held_reply, ptt_end_mid_pump, ptt_stop_drains_deferred,
                                    ptt_stop_discards_ineligible, ptt_stop_paced_ack_only, ptt_stop_paced_ack_eof,
                                    ptt_stop_paced_eof_ack, ptt_stop_paced_output_only, ptt_stop_no_progress_deadline,
                                    ptt_stop_paced_interrupts };
    int which = atoi(argv[1]);
    if (which < 0 || which >= (int)(sizeof(cases) / sizeof(cases[0]))) {
        return 2;
    }
    reset_world();
    cases[which]();
    assert(s_wakes > 0);
    return 0;
}
