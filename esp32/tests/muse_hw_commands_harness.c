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

// Host harness for main/muse_hw_commands.c: colours, tones, WAV decoding and
// resampling, and what link.register advertises. The runner extracts the
// production code between the "Host-tested" markers into muse_hw_commands.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

// What the extracted code expects from the rest of muse_hw_commands.c.
#define RATE 16000
#define TONES_MAX 32
#define TONES_MAX_MS 15000
#define WAIT_MAX_MS 60000
typedef struct {
    const char *name;
    int width, height;
} muse_board_t;
static const muse_board_t s_watcher = { "Seeed SenseCAP Watcher", 412, 412 };
static const muse_board_t *muse_board = &s_watcher;
static bool s_has_led = true;
static bool muse_hw_has_led(void) { return s_has_led; }
// components/muse/muse_ui.h's, which pulls in LVGL.
#define MUSE_UI_BUTTONS_MAX 6
typedef struct {
    char id[16];
    char label[24];
    uint32_t color;
} muse_ui_button_t;
#define SCAN_WAIT_MS 12000
static void muse_hw_camera_register(cJSON *commands) { (void)commands; }
static void muse_hw_io_register(cJSON *commands) { (void)commands; }
static void muse_hw_storage_register(cJSON *commands) { (void)commands; }
static void muse_hw_pet_register(cJSON *commands) { (void)commands; }

#include "muse_hw_commands.inc"

// tests/link_fakes' cJSON has only what Link uses.
static const char *str_of(const cJSON *item) {
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double num_of(const cJSON *item) {
    return item && (item->type & 0xff) == cJSON_Number ? item->valuedouble : -1;
}

static int count(const cJSON *object) {
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) n++;
    return n;
}

static resampler_t make_resampler(size_t cap) {
    resampler_t r = { .cap = cap };
    r.out = calloc(cap, sizeof(int16_t));
    assert(r.out);
    return r;
}

static void test_colors(void) {
    uint32_t c = 0;
    assert(parse_color("#ff8000", &c) && c == 0xff8000);
    assert(parse_color("00FF7f", &c) && c == 0x00ff7f);
    assert(parse_color("red", &c) && c == 0xff0000);
    assert(parse_color("White", &c) && c == 0xffffff);
    c = 123;
    assert(!parse_color("#ff80", &c) && c == 123);
    assert(!parse_color("#ff80001", &c));
    assert(!parse_color("zzzzzz", &c));
    assert(!parse_color("", &c));
    assert(!parse_color(NULL, &c));
    assert(clamp(-5, 0, 100) == 0 && clamp(500, 0, 100) == 100 && clamp(42, 0, 100) == 42);
}

static void test_tones(void) {
    resampler_t r = make_resampler(RATE * 16);
    assert(synth_tones("440:100, 0:50,880:100", &r) == NULL);
    assert(r.n == 1600 + 800 + 1600);
    assert(r.out[0] == 0);                         // faded in
    for (size_t i = 1600; i < 2400; i++) {
        assert(r.out[i] == 0);                     // the rest is silent
    }
    int peak = 0;
    for (size_t i = 0; i < 1600; i++) {
        peak = abs(r.out[i]) > peak ? abs(r.out[i]) : peak;
    }
    assert(peak > 14000 && peak < 16000);          // 0.45 of full scale

    const char *bad[] = { "440", "440:", ":100", "10:100", "9000:100", "440:0", "440:16000",
                          "440:8000,440:8000", "", " , " };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        r.n = 0;
        assert(synth_tones(bad[i], &r) != NULL);
    }
    char many[33 * 8 + 1] = "";
    for (int i = 0; i < 33; i++) strcat(many, "440:10,");
    r.n = 0;
    assert(synth_tones(many, &r) != NULL);         // over 32 tones
    many[32 * 7] = '\0';
    r.n = 0;
    assert(synth_tones(many, &r) == NULL && r.n == 32 * 160);
    free(r.out);
}

static void test_resample(void) {
    int16_t in[4800];
    for (int i = 0; i < 4800; i++) in[i] = (int16_t)(i * 4);

    // 8 kHz up: twice as many samples, halfway values between.
    resampler_t r = make_resampler(RATE);
    resample(&r, 8000, in, 800);
    assert(r.n >= 1597 && r.n <= 1600);
    assert(r.out[0] == 0 && r.out[1] == 2 && r.out[2] == 4);
    for (size_t i = 1; i < r.n; i++) assert(r.out[i] >= r.out[i - 1]);
    free(r.out);

    // 48 kHz down: a third, fed in pieces the way the decoders feed it.
    r = make_resampler(RATE);
    for (int off = 0; off < 4800; off += 1152) {
        resample(&r, 48000, in + off, 4800 - off < 1152 ? 4800 - off : 1152);
    }
    assert(r.n >= 1598 && r.n <= 1600);
    assert(r.out[1] == 12 && r.out[100] == 1200);
    free(r.out);

    // 16 kHz passes through; the output stops at its capacity.
    r = make_resampler(100);
    resample(&r, 16000, in, 4800);
    assert(r.n == 100 && r.out[99] == 99 * 4);
    free(r.out);
}

static size_t wav(uint8_t *buf, int format, int channels, int rate, int bits, const void *pcm, uint32_t bytes) {
    uint8_t *p = buf;
    memcpy(p, "RIFF", 4);
    uint32_t riff = 4 + 8 + 16 + 8 + 4 + 8 + bytes;
    memcpy(p + 4, &riff, 4);
    memcpy(p + 8, "WAVE", 4);
    p += 12;
    // A chunk decoders must skip.
    memcpy(p, "LIST", 4);
    uint32_t four = 4;
    memcpy(p + 4, &four, 4);
    memcpy(p + 8, "INFO", 4);
    p += 12;
    memcpy(p, "fmt ", 4);
    uint32_t sixteen = 16;
    memcpy(p + 4, &sixteen, 4);
    uint16_t f16[] = { (uint16_t)format, (uint16_t)channels };
    memcpy(p + 8, f16, 4);
    uint32_t r32 = (uint32_t)rate, byte_rate = (uint32_t)(rate * channels * bits / 8);
    memcpy(p + 12, &r32, 4);
    memcpy(p + 16, &byte_rate, 4);
    uint16_t align_bits[] = { (uint16_t)(channels * bits / 8), (uint16_t)bits };
    memcpy(p + 20, align_bits, 4);
    p += 24;
    memcpy(p, "data", 4);
    memcpy(p + 4, &bytes, 4);
    memcpy(p + 8, pcm, bytes);
    return (size_t)(p + 8 + bytes - buf);
}

static void test_wav(void) {
    static uint8_t buf[70000];
    static int16_t stereo[2 * 8000];
    for (int i = 0; i < 8000; i++) {
        stereo[2 * i] = 1000;
        stereo[2 * i + 1] = 3000;
    }
    // 16-bit stereo 8 kHz: mixed to mono and doubled in rate.
    size_t len = wav(buf, 1, 2, 8000, 16, stereo, sizeof(stereo));
    resampler_t r = make_resampler(RATE * 2);
    assert(decode_wav(buf, len, &r) == NULL);
    assert(r.n >= 15990 && r.n <= 16000);
    assert(r.out[0] == 2000 && r.out[r.n / 2] == 2000 && r.out[r.n - 1] == 2000);
    free(r.out);

    // 8-bit mono at 16 kHz: unsigned samples centred on 128.
    uint8_t mono8[1600];
    memset(mono8, 128 + 64, sizeof(mono8));
    len = wav(buf, 1, 1, 16000, 8, mono8, sizeof(mono8));
    r = make_resampler(RATE);
    assert(decode_wav(buf, len, &r) == NULL);
    assert(r.n == 1600 && r.out[0] == 64 << 8 && r.out[1599] == 64 << 8);
    free(r.out);

    // A data chunk longer than the file (streamed WAVs) plays what's there.
    len = wav(buf, 1, 2, 8000, 16, stereo, sizeof(stereo));
    uint32_t huge = 0xffffffffu;
    memcpy(buf + len - sizeof(stereo) - 4, &huge, 4);
    r = make_resampler(RATE * 2);
    assert(decode_wav(buf, len, &r) == NULL && r.n >= 15990);
    free(r.out);

    r = make_resampler(RATE);
    len = wav(buf, 3, 1, 16000, 32, stereo, 64);     // float
    assert(decode_wav(buf, len, &r) != NULL);
    len = wav(buf, 1, 1, 16000, 24, stereo, 64);     // 24-bit
    assert(decode_wav(buf, len, &r) != NULL);
    len = wav(buf, 1, 3, 16000, 16, stereo, 64);     // three channels
    assert(decode_wav(buf, len, &r) != NULL);
    len = wav(buf, 1, 1, 96000, 16, stereo, 64);     // too fast
    assert(decode_wav(buf, len, &r) != NULL);
    assert(decode_wav((const uint8_t *)"RIFF\0\0\0\0WAVEfmt ", 16, &r) != NULL);   // no data
    assert(decode_wav((const uint8_t *)"ID3\3\0\0\0\0\0\0\0\0", 12, &r) != NULL);  // not WAV
    assert(decode_wav(buf, 8, &r) != NULL);                                       // truncated
    assert(r.n == 0);
    free(r.out);
}

static void test_base64_and_wav(void) {
    char out[64];
    assert(base64((const uint8_t *)"", 0, out) == 0 && !strcmp(out, ""));
    assert(base64((const uint8_t *)"f", 1, out) == 4 && !strcmp(out, "Zg=="));
    assert(base64((const uint8_t *)"fo", 2, out) == 4 && !strcmp(out, "Zm8="));
    assert(base64((const uint8_t *)"foo", 3, out) == 4 && !strcmp(out, "Zm9v"));
    assert(base64((const uint8_t *)"foobar", 6, out) == 8 && !strcmp(out, "Zm9vYmFy"));
    const uint8_t bin[] = { 0xff, 0xfe, 0x00, 0x80, 0x7f };
    assert(!strcmp((base64(bin, 5, out), out), "//4AgH8="));

    uint8_t h[44];
    wav_header(h, 16000, 16000);
    assert(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
    uint32_t riff, data, rate, byte_rate;
    uint16_t fmt, ch, align, bits;
    memcpy(&riff, h + 4, 4);
    memcpy(&fmt, h + 20, 2);
    memcpy(&ch, h + 22, 2);
    memcpy(&rate, h + 24, 4);
    memcpy(&byte_rate, h + 28, 4);
    memcpy(&align, h + 32, 2);
    memcpy(&bits, h + 34, 2);
    memcpy(&data, h + 40, 4);
    assert(riff == 36 + 32000 && fmt == 1 && ch == 1 && rate == 16000 && byte_rate == 32000);
    assert(align == 2 && bits == 16 && data == 32000);

    int16_t pcm[7] = { 100, 200, -100, -300, 32767, 32767, 5 };
    assert(halve_rate(pcm, 7) == 3);
    assert(pcm[0] == 150 && pcm[1] == -200 && pcm[2] == 32767);
}

static void test_buttons(void) {
    muse_ui_button_t b[MUSE_UI_BUTTONS_MAX];
    int n = -1;
    assert(parse_buttons("yes:Yes:green, no:No:#ff0000,later:Later", 0x123456, b, &n) == NULL && n == 3);
    assert(!strcmp(b[0].id, "yes") && !strcmp(b[0].label, "Yes") && b[0].color == 0x00ff00);
    assert(!strcmp(b[1].id, "no") && b[1].color == 0xff0000);
    assert(!strcmp(b[2].label, "Later") && b[2].color == 0x123456);
    assert(parse_buttons("", 0, b, &n) == NULL && n == 0);
    const char *bad[] = { "yes", ":Yes", "yes:", "yes:Yes:nocolour", "a:1,b:2,c:3,d:4,e:5,f:6,g:7",
                          "averyveryverylongid:x", "y:a label far too long to fit" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(parse_buttons(bad[i], 0, b, &n) != NULL);
    }
}

static void check_params(cJSON *group) {
    assert(cJSON_IsObject(group));
    cJSON *p;
    cJSON_ArrayForEach(p, group) {
        const char *type = str_of(cJSON_GetObjectItem(p, "type"));
        assert(type && (!strcmp(type, "string") || !strcmp(type, "integer") || !strcmp(type, "boolean")));
        assert(str_of(cJSON_GetObjectItem(p, "description")));
    }
}

static void test_cpu_load(void) {
    // 1 kHz ticks over a 2 s window: 500 busy ones are a quarter.
    assert(busy_load_pct(500, 2000000, 1000) == 25);
    assert(busy_load_pct(0, 2000000, 1000) == 0);
    assert(busy_load_pct(2000, 2000000, 1000) == 100);
    assert(busy_load_pct(2003, 2000000, 1000) == 100);      // a tick or two past the window
    assert(busy_load_pct(5, 2000000, 1000) == 0);           // rounds
    assert(busy_load_pct(10, 2000000, 1000) == 1);
    // Light sleep stretched the window; its skipped ticks are idle.
    assert(busy_load_pct(600, 60000000, 1000) == 1);
    assert(busy_load_pct(50, 2000000, 100) == 25);          // a 100 Hz tick
    // No window to tell.
    assert(busy_load_pct(10, 0, 1000) == -1);
    assert(busy_load_pct(10, 2000000, 0) == -1);
}

static void test_registration(void) {
    static const char *const names[] = {
        "device.status", "display.show_text", "display.set_brightness", "display.show_ui", "display.power",
        "led.set", "input.read", "audio.play_url", "audio.beep", "audio.set_volume", "audio.record",
        "wifi.scan", "device.reboot", "device.power_off", "audio.listen",
    };
    cJSON *commands = cJSON_CreateObject();
    muse_hw_commands_register(commands);
    assert(count(commands) == (int)(sizeof(names) / sizeof(names[0])));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        cJSON *c = cJSON_GetObjectItem(commands, names[i]);
        assert(c);
        const char *desc = str_of(cJSON_GetObjectItem(c, "description"));
        assert(desc && strlen(desc) > 20 && strlen(desc) < 768);   // device.status's lists every field
        check_params(cJSON_GetObjectItem(c, "required"));
        check_params(cJSON_GetObjectItem(c, "optional"));
    }
    // The ones that answer later say how long to wait.
    assert(num_of(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "input.read"), "timeout_ms"))
           > WAIT_MAX_MS);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "audio.play_url"), "timeout_ms"));
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "audio.listen"), "timeout_ms"));
    assert(!cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "device.status"), "timeout_ms"));
    assert(strstr(str_of(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "input.read"),
                                                           "description")), "412x412"));
    // Questions about the CPU go to device.status, answered in the conversation.
    const char *status = str_of(cJSON_GetObjectItem(cJSON_GetObjectItem(commands, "device.status"), "description"));
    assert(strstr(status, "load_percent") && strstr(status, "yourself") && strstr(status, "sub-agent"));
    printf("device.status description: %zu bytes\n", strlen(status));
    // link.register prints into at most 16 KB, and Link's own commands take
    // about 5 KB of it.
    char *json = cJSON_PrintUnformatted(commands);
    printf("hardware commands: %zu bytes\n", strlen(json));
    assert(strlen(json) < 9000);
    free(json);
    cJSON_Delete(commands);

    // No light, no led.set; before Muse has its board, nothing.
    s_has_led = false;
    commands = cJSON_CreateObject();
    muse_hw_commands_register(commands);
    assert(!cJSON_GetObjectItem(commands, "led.set") && cJSON_GetObjectItem(commands, "input.read"));
    cJSON_Delete(commands);
    s_has_led = true;
    muse_board = NULL;
    commands = cJSON_CreateObject();
    muse_hw_commands_register(commands);
    assert(count(commands) == 0);
    cJSON_Delete(commands);
    muse_board = &s_watcher;
}

int main(void) {
    test_colors();
    test_tones();
    test_resample();
    test_wav();
    test_base64_and_wav();
    test_buttons();
    test_cpu_load();
    test_registration();
    printf("ok\n");
    return 0;
}
