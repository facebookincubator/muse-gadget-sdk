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

#include "muse_hw_commands.h"
#include "muse_hw_commands_priv.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"   // after FreeRTOS.h, which its portmacro.h needs
#include "minimp3.h"

#include "muse_board.h"
#include "muse_hw.h"
#include "muse_input.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_ui.h"
#include "muse_voice.h"
#include "muse_wifi.h"

static const char *TAG = "link.muse_hw";

#define RATE 16000                      // what the speaker plays (muse_voice.h)
#define DOWNLOAD_MAX (1024 * 1024)      // audio.play_url: bytes fetched (a minute of 128 kbps MP3)
#define CLIP_MAX_S 30                   // ... and seconds played; with the download, ~2.5 MB of PSRAM at most
#define FETCH_TIMEOUT_MS 20000
#define TONES_MAX 32
#define TONES_MAX_MS 15000
#define TEXT_MAX 600
#define WAIT_MAX_MS 60000
#define CAPTURE_MAX_S 600
#define EVENTS_MAX 32
#define AUDIO_STACK (32 * 1024)         // minimp3 keeps ~16 KB of scratch on the stack
#define RECORD_MAX_S 5                  // audio.record at 16 kHz: ~213 KB of base64, under a result's 256 KB
#define RECORD_MAX_S_8K 10              // ... and at 8 kHz
#define SCAN_WAIT_MS 12000
#define SCAN_MAX 20
#define CPU_SAMPLE_US (2 * 1000000LL)   // device.status's CPU load covers this much
#define CPU_LOAD !CONFIG_FREERTOS_SMP    // it reads the IDF kernel's current tasks

typedef hw_reply_to_t reply_to_t;

// ---- Results ---------------------------------------------------------------

static cJSON *error_result(const char *code, const char *message) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", false);
    cJSON *error = cJSON_AddObjectToObject(result, "error");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    return result;
}

static cJSON *ok_result(cJSON *payload) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", payload ? payload : cJSON_CreateObject());
    return result;
}

static cJSON *async_result(void) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "_async", true);
    return result;
}

#define CONSOLE_SESSION HW_CONSOLE_SESSION

static void console_print(const char *request_id, cJSON *result) {
    cJSON *line = cJSON_CreateObject();
    cJSON_AddStringToObject(line, "id", request_id);
    cJSON_AddItemToObject(line, "result", result);
    char *json = cJSON_PrintUnformatted(line);
    cJSON_Delete(line);
    if (json) {
        printf("@hw %s\n", json);
        fflush(stdout);
        cJSON_free(json);
    }
}

// Every result goes through Link, whose hook (on_result) takes the console's
// and the scripts', including those of Link's own commands (display.draw_url).
static void reply(const reply_to_t *to, cJSON *result) {
    noise_ctrl_send_command_result(to->session_generation, to->request_id, result);
}

static bool on_result(noise_ctrl_session_generation_t session_generation, const char *request_id,
                      cJSON *result) {
    if (session_generation == CONSOLE_SESSION) {
        console_print(request_id, result);
        return true;
    }
#if CONFIG_MUSE_SCRIPTS
    if (session_generation == HW_SCRIPT_SESSION) {
        muse_script_result(request_id, result);
        return true;
    }
#endif
    return false;
}

static void set_reply_to(reply_to_t *to, const char *request_id,
                         noise_ctrl_session_generation_t session_generation) {
    to->session_generation = session_generation;
    strlcpy(to->request_id, request_id, sizeof(to->request_id));
}

// ---- Parameters ------------------------------------------------------------

static bool get_int(cJSON *params, const char *key, int *out) {
    cJSON *v = params ? cJSON_GetObjectItem(params, key) : NULL;
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valueint;
    return true;
}

static const char *get_str(cJSON *params, const char *key) {
    cJSON *v = params ? cJSON_GetObjectItem(params, key) : NULL;
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

// ---- Host-tested: formats and registration (tests/test_muse_hw.py) --------

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// "#rrggbb", "rrggbb" or a colour name.
static bool parse_color(const char *s, uint32_t *rgb) {
    static const struct {
        const char *name;
        uint32_t rgb;
    } names[] = {
        { "black", 0x000000 },  { "white", 0xffffff },  { "red", 0xff0000 },
        { "green", 0x00ff00 },  { "blue", 0x0000ff },   { "yellow", 0xffd000 },
        { "orange", 0xff6000 }, { "purple", 0x9000ff }, { "pink", 0xff3080 },
        { "cyan", 0x00ffff },   { "magenta", 0xff00ff }, { "gray", 0x808080 },
        { "grey", 0x808080 },
    };
    if (!s) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcasecmp(s, names[i].name) == 0) {
            *rgb = names[i].rgb;
            return true;
        }
    }
    if (*s == '#') s++;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (strlen(s) != 6 || *end) return false;
    *rgb = (uint32_t)v;
    return true;
}

// A core's load in percent over window_us, from the ticks (tick_hz a second)
// that found it busy; -1 if there's no window to tell.
static int busy_load_pct(uint32_t busy_ticks, int64_t window_us, uint32_t tick_hz) {
    if (window_us <= 0 || !tick_hz) return -1;
    int64_t pct = ((int64_t)busy_ticks * 100000000 / tick_hz + window_us / 2) / window_us;
    return pct > 100 ? 100 : (int)pct;
}

// Mono PCM at any rate into 16 kHz, by linear interpolation.
typedef struct {
    int16_t *out;
    size_t n, cap;
    double step, pos;  // input samples per output sample; next output's input position
    int16_t prev;
    bool primed;
} resampler_t;

static void resample(resampler_t *r, int in_rate, const int16_t *in, size_t n) {
    r->step = (double)in_rate / RATE;
    for (size_t i = 0; i < n; i++) {
        if (!r->primed) {
            r->prev = in[i];
            r->primed = true;
            r->pos = 0;
            continue;
        }
        // Outputs that fall between prev (at 0) and in[i] (at 1).
        while (r->pos <= 1.0 && r->n < r->cap) {
            r->out[r->n++] = (int16_t)(r->prev + (in[i] - r->prev) * r->pos);
            r->pos += r->step;
        }
        r->pos -= 1.0;
        r->prev = in[i];
    }
}

static const char *decode_wav(const uint8_t *d, size_t len, resampler_t *r) {
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return "not a WAV file";
    int channels = 0, rate = 0, bits = 0, format = 0;
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t size = d[off + 4] | d[off + 5] << 8 | d[off + 6] << 16 | (uint32_t)d[off + 7] << 24;
        const uint8_t *body = d + off + 8;
        if (!memcmp(d + off, "fmt ", 4) && size >= 16 && off + 8 + 16 <= len) {
            format = body[0] | body[1] << 8;
            channels = body[2] | body[3] << 8;
            rate = body[4] | body[5] << 8 | body[6] << 16 | body[7] << 24;
            bits = body[14] | body[15] << 8;
        } else if (!memcmp(d + off, "data", 4)) {
            if (format != 1 || (bits != 16 && bits != 8) || channels < 1 || channels > 2 || rate < 4000 ||
                rate > 48000) {
                return "WAV must be 8 or 16-bit PCM, mono or stereo, 4-48 kHz";
            }
            size_t bytes = size > len - (off + 8) ? len - (off + 8) : size;
            size_t frames = bytes / (bits / 8) / channels;
            int16_t chunk[256];
            for (size_t f = 0; f < frames && r->n < r->cap;) {
                size_t k = 0;
                for (; k < 256 && f < frames; k++, f++) {
                    int sum = 0;
                    for (int c = 0; c < channels; c++) {
                        size_t i = f * channels + c;
                        sum += bits == 16 ? (int16_t)(body[2 * i] | body[2 * i + 1] << 8) : (body[i] - 128) << 8;
                    }
                    chunk[k] = (int16_t)(sum / channels);
                }
                resample(r, rate, chunk, k);
            }
            return NULL;
        }
        off += 8 + size + (size & 1);
    }
    return "WAV has no audio data";
}

// "523:150, 0:50, 784:300" into 16 kHz PCM, each tone faded in and out.
static const char *synth_tones(const char *spec, resampler_t *r) {
    struct {
        int hz, ms;
    } tones[TONES_MAX];
    int n = 0, total_ms = 0;
    for (const char *p = spec; *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *end;
        long hz = strtol(p, &end, 10);
        if (end == p || *end != ':') return "tones are hz:ms, comma-separated";
        p = end + 1;
        long ms = strtol(p, &end, 10);
        if (end == p) return "tones are hz:ms, comma-separated";
        p = end;
        if (n == TONES_MAX) return "at most 32 tones";
        if ((hz && (hz < 20 || hz > 8000)) || ms <= 0 || ms > TONES_MAX_MS) return "a tone is 20-8000 hz (0 rests) and 1-15000 ms";
        total_ms += (int)ms;
        tones[n].hz = (int)hz;
        tones[n++].ms = (int)ms;
    }
    if (!n) return "no tones";
    if (total_ms > TONES_MAX_MS) return "the tones are over 15 s";
    const int fade = RATE / 200;  // 5 ms
    for (int t = 0; t < n; t++) {
        int len = tones[t].ms * RATE / 1000;
        for (int i = 0; i < len && r->n < r->cap; i++) {
            float env = 1.0f;
            if (i < fade) env = (float)i / fade;
            if (len - i < fade) env = (float)(len - i) / fade;
            float v = tones[t].hz ? sinf(2.0f * (float)M_PI * tones[t].hz * i / RATE) : 0.0f;
            r->out[r->n++] = (int16_t)(v * env * 0.45f * 32767);
        }
    }
    return NULL;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 4 characters for each 3 bytes, padded, NUL-terminated: out holds (n + 2) / 3 * 4 + 1.
static size_t base64(const uint8_t *in, size_t n, char *out) {
    char *o = out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *o++ = B64[v >> 18 & 63];
        *o++ = B64[v >> 12 & 63];
        *o++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
        *o++ = i + 2 < n ? B64[v & 63] : '=';
    }
    *o = '\0';
    return (size_t)(o - out);
}

// A 44-byte header for `frames` of mono 16-bit PCM at `rate`.
static void wav_header(uint8_t h[44], uint32_t frames, uint32_t rate) {
    uint32_t data = frames * 2;
    const uint32_t fields[] = { 36 + data, 16, 1 | 1 << 16, rate, rate * 2, 2 | 16 << 16, data };
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 36, "data", 4);
    const int at[] = { 4, 16, 20, 24, 28, 32, 40 };
    for (int f = 0; f < 7; f++) {
        for (int b = 0; b < 4; b++) h[at[f] + b] = (uint8_t)(fields[f] >> (8 * b));
    }
}

// 16 kHz to 8 kHz in place, each pair averaged (a gentle low-pass). Returns the frames left.
static size_t halve_rate(int16_t *pcm, size_t frames) {
    for (size_t i = 0; i + 1 < frames; i += 2) pcm[i / 2] = (int16_t)((pcm[i] + pcm[i + 1]) / 2);
    return frames / 2;
}

// "yes:Yes,no:No:red" into up to MUSE_UI_BUTTONS_MAX buttons: an id, a label,
// and a colour (default `color`). NULL or an error message.
static const char *parse_buttons(const char *spec, uint32_t color, muse_ui_button_t *out, int *n) {
    *n = 0;
    for (const char *p = spec; *p;) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[64];
        if (len >= sizeof(item)) return "a button is id:label[:colour], under 64 characters";
        memcpy(item, p, len);
        item[len] = '\0';
        p += len;
        char *label = strchr(item, ':');
        if (!label || label == item || !label[1]) return "buttons are id:label[:colour], comma-separated";
        *label++ = '\0';
        char *hue = strchr(label, ':');
        uint32_t rgb = color;
        if (hue) {
            *hue++ = '\0';
            if (!parse_color(hue, &rgb)) return "a button's colour is #rrggbb or a name";
        }
        if (*n == MUSE_UI_BUTTONS_MAX) return "at most 6 buttons";
        if (strlen(item) >= sizeof(out->id) || strlen(label) >= sizeof(out->label)) {
            return "a button's id is under 16 characters and its label under 24";
        }
        strcpy(out[*n].id, item);
        strcpy(out[*n].label, label);
        out[*n].color = rgb;
        (*n)++;
    }
    return NULL;
}

// ---- Registration ----------------------------------------------------------

static cJSON *param(const char *type, const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static cJSON *params(const char *name, cJSON *p) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, name, p);
    return o;
}

static void add(cJSON *commands, const char *name, const char *description,
                cJSON *required, cJSON *optional, int timeout_ms) {
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "description", description);
    cJSON_AddItemToObject(command, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(command, "optional", optional ? optional : cJSON_CreateObject());
    if (timeout_ms) cJSON_AddNumberToObject(command, "timeout_ms", timeout_ms);
    cJSON_AddItemToObject(commands, name, command);
}

void muse_hw_commands_register(cJSON *commands) {
    if (!muse_board) return;  // Muse starts before Link connects; not yet, then
    add(commands, "device.status",
        "Battery percent and voltage, charging, USB power, whether the "
        "screen is on, its brightness, the speaker volume, what Muse is doing "
        "(idle, listening, thinking, speaking), and the screen's size and shape. "
        "boot_reason: how this boot began (power_on, restart, crash, watchdog, "
        "brownout...); for a few boots after a crash, last_crash has its task, pc, "
        "backtrace (ELF addresses) and boots_ago. memory: free bytes now and at the "
        "lowest (internal RAM is the scarce kind; under ~8 KB things fail). cpu: model, cores, mhz (busy) "
        "and idle_mhz, load_percent per core (approximate, last ~2 s; null at first); "
        "temperature: chip_temp_c. "
        "Instant: for questions about this device (CPU, memory, battery...), call it yourself; don't hand "
        "it to a sub-agent or background task.",
        NULL, NULL, 0);

    cJSON *text_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(text_opt, "color", param("string", "Text colour, #rrggbb or a name; default white."));
    cJSON_AddItemToObject(text_opt, "background", param("string", "Background colour; default black."));
    cJSON_AddItemToObject(text_opt, "size", param("string", "small, medium (default) or large."));
    add(commands, "display.show_text",
        "Show text, wrapped and centred, over Muse's face until "
        "display.show_animation, a tap or a talk. Plain ASCII; the round screen "
        "fits about 8 lines of medium text.",
        params("text", param("string", "The text; \\n breaks a line.")), text_opt, 0);

    add(commands, "display.set_brightness", "Set the screen brightness; kept across restarts.",
        params("brightness", param("integer", "10 to 100 percent.")), NULL, 0);

    cJSON *ui_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(ui_opt, "title", param("string", "Large text at the top."));
    cJSON_AddItemToObject(ui_opt, "text", param("string", "Body text; \\n breaks a line."));
    cJSON_AddItemToObject(ui_opt, "buttons", param("string", "Up to 6, id:label[:colour], comma-separated; e.g. yes:Yes:green,no:No:red."));
    cJSON_AddItemToObject(ui_opt, "progress", param("integer", "0 to 100 for a progress bar; omit for none."));
    cJSON_AddItemToObject(ui_opt, "color", param("string", "Text colour; default white."));
    cJSON_AddItemToObject(ui_opt, "background", param("string", "Default black."));
    add(commands, "display.show_ui",
        "Show a screen with a title, text, a progress bar and buttons over Muse's face. "
        "A pressed button is an input.read event: type button, its id. Stays until "
        "display.show_animation, a talk or another screen; call again to update it.",
        NULL, ui_opt, 0);

    add(commands, "display.power",
        "Turn the screen off (Muse sleeps; a tap or the wheel wakes it) or back on.",
        params("on", param("boolean", "true to wake the screen, false to sleep it.")), NULL, 0);

    if (muse_hw_has_led()) {
        cJSON *led_opt = cJSON_CreateObject();
        cJSON_AddItemToObject(led_opt, "color", param("string", "#rrggbb or a name; default white."));
        cJSON_AddItemToObject(led_opt, "brightness", param("integer", "0 to 100; default 40. 100 is glaring."));
        cJSON_AddItemToObject(led_opt, "effect", param("string", "solid (default), blink, breathe or off."));
        cJSON_AddItemToObject(led_opt, "period_ms", param("integer", "One blink or breath; default 1000."));
        add(commands, "led.set", "Set the RGB light on the device.", NULL, led_opt, 0);
    }

    char desc[512];
    snprintf(desc, sizeof(desc),
             "Read touch and button events: tap, long_press and swipe (x, y "
             "from the top left of the %dx%d screen, swipe direction), "
             "wheel_click and wheel_turn (steps, direction), button (display.show_ui) "
             "and detection (camera.watch: label, score). Returns events after "
             "`after`, waiting up to wait_ms for one. Pass the reply's last_seq "
             "as the next `after`. With capture_s, taps and the wheel reach only "
             "you for that long, not Muse's own UI.",
             muse_board->width, muse_board->height);
    cJSON *input_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(input_opt, "wait_ms", param("integer", "0 (default) to 60000."));
    cJSON_AddItemToObject(input_opt, "after", param("integer", "Event seq; default: only events from now on."));
    cJSON_AddItemToObject(input_opt, "capture_s", param("integer", "0 to 600 seconds; 0 ends a capture."));
    add(commands, "input.read", desc, NULL, input_opt, WAIT_MAX_MS + 5000);

    add(commands, "audio.play_url",
        "Download an MP3 or WAV (up to 1 MB, 30 s) and play it on the speaker at "
        "the current volume, with Muse's mouth moving. Replies when it ends; "
        "played is false if the user pressed talk.",
        params("url", param("string", "http:// or https:// URL, or a file on the SD card.")), NULL, 150000);

    add(commands, "audio.beep",
        "Play tones on the speaker: a beep, a chime or a short tune.",
        params("tones", param("string", "Comma-separated hz:ms, 0 hz a rest, up to 32 "
                                        "and 15 s; e.g. 523:150,659:150,784:300.")),
        NULL, 25000);

    add(commands, "audio.set_volume", "Set the speaker volume; kept across restarts.",
        params("volume", param("integer", "0 to 100.")), NULL, 0);

    cJSON *rec_opt = cJSON_CreateObject();
    cJSON_AddItemToObject(rec_opt, "seconds", param("integer", "1 to 5 at 16 kHz, to 10 at 8 kHz; default 3."));
    cJSON_AddItemToObject(rec_opt, "sample_rate", param("integer", "16000 (default) or 8000."));
    cJSON_AddItemToObject(rec_opt, "save", param("string", "A path on the SD card to save the WAV to; then only include_audio returns it."));
    cJSON_AddItemToObject(rec_opt, "include_audio", param("boolean", "With save, return the audio too."));
    add(commands, "audio.record",
        "Record from the mic and return it as a base64 WAV (16-bit mono). A talk press ends it early.",
        NULL, rec_opt, 25000);

    add(commands, "wifi.scan",
        "List the Wi-Fi networks in range, strongest first (ssid, rssi in dBm, secure), and the one joined.",
        NULL, NULL, SCAN_WAIT_MS + 5000);

    add(commands, "device.reboot", "Restart the device. Pairing and settings are kept.", NULL, NULL, 0);
    add(commands, "device.power_off",
        "Power the device off; pressing the wheel turns it back on. It can't be reached until then.",
        NULL, NULL, 0);

    muse_hw_camera_register(commands);
    muse_hw_io_register(commands);
    muse_hw_storage_register(commands);
    muse_hw_pet_register(commands);
#if CONFIG_MUSE_SCRIPTS
    muse_script_register(commands);
    muse_hw_apps_register(commands);
#endif

    add(commands, "audio.listen",
        "Measure how loud it is around the device, in dBFS: about -60 is a quiet "
        "room, -35 talking nearby, -15 very loud. Doesn't record or transcribe.",
        NULL, params("seconds", param("integer", "1 to 10; default 3.")), 20000);
}

// ---- Host-tested end

// ---- Shared with muse_hw_camera.c (muse_hw_commands_priv.h) ---------------

cJSON *hw_error(const char *code, const char *message) { return error_result(code, message); }
cJSON *hw_ok(cJSON *payload) { return ok_result(payload); }
cJSON *hw_async(void) { return async_result(); }
void hw_reply_to(hw_reply_to_t *to, const char *request_id,
                 noise_ctrl_session_generation_t session_generation) {
    set_reply_to(to, request_id, session_generation);
}
void hw_reply(const hw_reply_to_t *to, cJSON *result) { reply(to, result); }
size_t hw_base64(const uint8_t *in, size_t n, char *out) { return base64(in, n, out); }
bool hw_int(cJSON *p, const char *key, int *out) { return get_int(p, key, out); }
const char *hw_str(cJSON *p, const char *key) { return get_str(p, key); }
int hw_clamp(int v, int lo, int hi) { return clamp(v, lo, hi); }
cJSON *hw_param(const char *type, const char *description) { return param(type, description); }
cJSON *hw_params(const char *name, cJSON *p) { return params(name, p); }
void hw_add(cJSON *commands, const char *name, const char *description,
            cJSON *required, cJSON *optional, int timeout_ms) {
    add(commands, name, description, required, optional, timeout_ms);
}

// ---- device.status ---------------------------------------------------------

static const char *mode_name(muse_mode_t mode) {
    switch (mode) {
        case MUSE_MODE_BOOT: return "starting";
        case MUSE_MODE_IDLE: return "idle";
        case MUSE_MODE_LISTENING: return "listening";
        case MUSE_MODE_THINKING: return "thinking";
        case MUSE_MODE_SPEAKING: return "speaking";
        case MUSE_MODE_ERROR: return "error";
        case MUSE_MODE_OFF: return "powering off";
        default: return "unknown";
    }
}

static const char *boot_reason(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_SW: return "restart";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

#if CPU_LOAD
// Each core's load, sampled at every tick (1 kHz): a tick that finds the core
// running anything but its idle task counts as busy, and the ticks skipped
// while it sleeps (tickless idle) as idle. FreeRTOS's run-time stats would be
// exact, but their fields in every task and queue cost ~1.2 KB of internal
// RAM; this misses only work shorter than a tick that the tick itself starts.
// Read in the tick interrupt, which runs while flash is busy: IRAM code, and
// data in internal RAM.
extern TaskHandle_t volatile pxCurrentTCBs[];   // the kernel's: the task each core is running
static TaskHandle_t s_idle_task[portNUM_PROCESSORS];
static volatile uint32_t s_busy_ticks[portNUM_PROCESSORS];
static struct {
    int64_t at;
    uint32_t busy[portNUM_PROCESSORS];
} s_cpu_window;
static volatile int s_cpu_load[portNUM_PROCESSORS];   // over the last window; -1 before the first

static void IRAM_ATTR busy_tick(void) {
    int core = xPortGetCoreID();
    if (pxCurrentTCBs[core] != s_idle_task[core]) s_busy_ticks[core]++;
}

// On esp_timer's task: only reads the counts.
static void cpu_window(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        uint32_t busy = s_busy_ticks[core];
        if (s_cpu_window.at) {
            s_cpu_load[core] = busy_load_pct(busy - s_cpu_window.busy[core], now - s_cpu_window.at, configTICK_RATE_HZ);
        }
        s_cpu_window.busy[core] = busy;
    }
    s_cpu_window.at = now;
}

static void cpu_load_init(void) {
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        s_cpu_load[core] = -1;
        s_idle_task[core] = xTaskGetIdleTaskHandleForCore(core);
        esp_register_freertos_tick_hook_for_cpu(busy_tick, core);
    }
    static esp_timer_handle_t timer;
    // Doesn't wake the chip from light sleep: the window just runs longer, its sleep idle.
    const esp_timer_create_args_t a = { .callback = cpu_window, .name = "hw_cpu", .skip_unhandled_events = true };
    if (esp_timer_create(&a, &timer) == ESP_OK) esp_timer_start_periodic(timer, CPU_SAMPLE_US);
    cpu_window(NULL);   // the first window starts now
}
#endif

// The chip's temperature is muse_hw_io_status's chip_temp_c.
static void cpu_status(cJSON *pl) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *cpu = cJSON_AddObjectToObject(pl, "cpu");
    cJSON_AddStringToObject(cpu, "model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(cpu, "cores", chip.cores);
    // A core running this is at the top clock: power management drops it only
    // while both idle, so what it drops to comes from its configuration.
    uint32_t hz;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) == ESP_OK) {
        cJSON_AddNumberToObject(cpu, "mhz", hz / 1000000);
    }
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm;
    if (esp_pm_get_configuration(&pm) == ESP_OK) cJSON_AddNumberToObject(cpu, "idle_mhz", pm.min_freq_mhz);
#endif
    cJSON *load = cJSON_AddArrayToObject(cpu, "load_percent");
    for (int core = 0; core < chip.cores; core++) {
#if CPU_LOAD
        int pct = core < portNUM_PROCESSORS ? s_cpu_load[core] : -1;
#else
        int pct = -1;
#endif
        cJSON_AddItemToArray(load, pct >= 0 ? cJSON_CreateNumber(pct) : cJSON_CreateNull());
    }
}

static cJSON *status_command(void) {
    muse_power_t p = muse_state_power();
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "board", muse_board->name);
    if (p.battery_pct >= 0) {
        cJSON_AddNumberToObject(pl, "battery_percent", p.battery_pct);
    } else {
        cJSON_AddNullToObject(pl, "battery_percent");
    }
    if (p.battery_mv) cJSON_AddNumberToObject(pl, "battery_mv", p.battery_mv);
    cJSON_AddBoolToObject(pl, "charging", p.charging);
    cJSON_AddBoolToObject(pl, "usb_power", p.usb);
    cJSON_AddBoolToObject(pl, "screen_on", !muse_state_asleep());
    cJSON_AddNumberToObject(pl, "brightness", muse_settings_brightness());
    cJSON_AddNumberToObject(pl, "volume", muse_settings_volume());
    cJSON_AddBoolToObject(pl, "speaker_on", muse_settings_speaker_on());
    cJSON_AddStringToObject(pl, "mode", mode_name(muse_state_mode(NULL)));
    cJSON_AddBoolToObject(pl, "input_captured", muse_hw_captured());
    cJSON_AddBoolToObject(pl, "has_light", muse_hw_has_led());
    cJSON_AddNumberToObject(pl, "screen_width", muse_board->width);
    cJSON_AddNumberToObject(pl, "screen_height", muse_board->height);
    cJSON_AddBoolToObject(pl, "screen_round", muse_board->round);
    cJSON_AddBoolToObject(pl, "touch", muse_board->touch);
    cJSON_AddNumberToObject(pl, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(pl, "boot_reason", boot_reason());
    muse_crash_json(pl);
    cpu_status(pl);
    cJSON *mem = cJSON_AddObjectToObject(pl, "memory");
    cJSON_AddNumberToObject(mem, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    muse_hw_io_status(pl);
    muse_wifi_status_t wifi;
    muse_wifi_status(&wifi);
    cJSON_AddBoolToObject(pl, "wifi_connected", wifi.state == MUSE_WIFI_CONNECTED);
    if (wifi.ssid[0]) cJSON_AddStringToObject(pl, "wifi_ssid", wifi.ssid);
    if (wifi.state == MUSE_WIFI_CONNECTED) {
        cJSON_AddNumberToObject(pl, "wifi_rssi", wifi.rssi);
        cJSON_AddStringToObject(pl, "ip", wifi.ip);
    }
    return ok_result(pl);
}

// ---- display ---------------------------------------------------------------

static cJSON *show_text_command(cJSON *params) {
    const char *text = get_str(params, "text");
    if (!text) return error_result("missing_param", "text is required");
    if (strlen(text) > TEXT_MAX) return error_result("invalid_params", "text is over 600 characters");
    uint32_t fg = 0xffffff, bg = 0x000000;
    const char *color = get_str(params, "color");
    const char *background = get_str(params, "background");
    if ((color && !parse_color(color, &fg)) || (background && !parse_color(background, &bg))) {
        return error_result("invalid_params", "colours are #rrggbb or a name");
    }
    const char *size = get_str(params, "size");
    int s = 1;
    if (size) {
        if (strcasecmp(size, "small") == 0) s = 0;
        else if (strcasecmp(size, "large") == 0) s = 2;
        else if (strcasecmp(size, "medium") != 0) {
            return error_result("invalid_params", "size is small, medium or large");
        }
    }
    if (!muse_ui_text_show(text, fg, bg, s)) return error_result("unavailable", "the screen isn't up");
    return ok_result(NULL);
}

static cJSON *ui_command(cJSON *params) {
    muse_ui_panel_t panel = { .progress = -1, .fg = 0xffffff, .bg = 0x000000, .accent = 0xa77dff };
    panel.title = get_str(params, "title");
    panel.text = get_str(params, "text");
    if ((panel.title && strlen(panel.title) > 120) || (panel.text && strlen(panel.text) > TEXT_MAX)) {
        return error_result("invalid_params", "the title is at most 120 characters and the text 600");
    }
    const char *color = get_str(params, "color");
    const char *background = get_str(params, "background");
    if ((color && !parse_color(color, &panel.fg)) || (background && !parse_color(background, &panel.bg))) {
        return error_result("invalid_params", "colours are #rrggbb or a name");
    }
    int progress;
    if (get_int(params, "progress", &progress)) panel.progress = clamp(progress, 0, 100);
    const char *buttons = get_str(params, "buttons");
    if (buttons) {
        const char *err = parse_buttons(buttons, 0x5b3fd6, panel.buttons, &panel.n_buttons);
        if (err) return error_result("invalid_params", err);
    }
    if (!muse_ui_panel_show(&panel)) return error_result("unavailable", "the screen isn't up");
    return ok_result(NULL);
}

static cJSON *brightness_command(cJSON *params) {
    int pct;
    if (!get_int(params, "brightness", &pct)) return error_result("missing_param", "brightness is required");
    pct = clamp(pct, 10, 100);
    muse_settings_set_brightness(pct);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "brightness", pct);
    return ok_result(pl);
}

static cJSON *power_command(cJSON *params) {
    cJSON *on = params ? cJSON_GetObjectItem(params, "on") : NULL;
    if (!cJSON_IsBool(on)) return error_result("missing_param", "on (true or false) is required");
    muse_input_set_asleep(!cJSON_IsTrue(on));
    return ok_result(NULL);
}

// ---- led.set ---------------------------------------------------------------

static cJSON *led_command(cJSON *params) {
    if (!muse_hw_has_led()) return error_result("unsupported", "this board has no RGB light");
    uint32_t rgb = 0xffffff;
    const char *color = get_str(params, "color");
    if (color && !parse_color(color, &rgb)) return error_result("invalid_params", "color is #rrggbb or a name");
    int brightness = 40, period = 1000;
    get_int(params, "brightness", &brightness);
    get_int(params, "period_ms", &period);
    brightness = clamp(brightness, 0, 100);
    period = clamp(period, 100, 10000);
    muse_hw_led_mode_t mode = MUSE_HW_LED_SOLID;
    const char *effect = get_str(params, "effect");
    if (effect) {
        if (strcasecmp(effect, "blink") == 0) mode = MUSE_HW_LED_BLINK;
        else if (strcasecmp(effect, "breathe") == 0) mode = MUSE_HW_LED_BREATHE;
        else if (strcasecmp(effect, "off") == 0) mode = MUSE_HW_LED_OFF;
        else if (strcasecmp(effect, "solid") != 0) {
            return error_result("invalid_params", "effect is solid, blink, breathe or off");
        }
    }
    if (!muse_hw_led((rgb >> 16 & 0xff) * brightness / 100, (rgb >> 8 & 0xff) * brightness / 100,
                     (rgb & 0xff) * brightness / 100, mode, period)) {
        return error_result("internal", "couldn't set the light");
    }
    return ok_result(NULL);
}

// ---- input.read ------------------------------------------------------------

cJSON *hw_event_json(const muse_hw_event_t *ev, int64_t now) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddNumberToObject(e, "seq", ev->seq);
    cJSON_AddStringToObject(e, "type", muse_hw_event_name(ev->type));
    cJSON_AddNumberToObject(e, "ms_ago", (double)((now - ev->us) / 1000));
    switch (ev->type) {
        case MUSE_HW_TAP:
        case MUSE_HW_LONG_PRESS:
            cJSON_AddNumberToObject(e, "x", ev->x);
            cJSON_AddNumberToObject(e, "y", ev->y);
            cJSON_AddNumberToObject(e, "held_ms", ev->value);
            break;
        case MUSE_HW_SWIPE:
            cJSON_AddStringToObject(e, "direction", muse_hw_swipe_dir(ev));
            cJSON_AddNumberToObject(e, "x", ev->x);
            cJSON_AddNumberToObject(e, "y", ev->y);
            cJSON_AddNumberToObject(e, "x2", ev->x2);
            cJSON_AddNumberToObject(e, "y2", ev->y2);
            break;
        case MUSE_HW_WHEEL_CLICK:
            cJSON_AddNumberToObject(e, "held_ms", ev->value);
            break;
        case MUSE_HW_WHEEL_TURN:
            cJSON_AddNumberToObject(e, "steps", abs(ev->value));
            cJSON_AddStringToObject(e, "direction", ev->value > 0 ? "clockwise" : "counterclockwise");
            break;
        case MUSE_HW_BUTTON:
            cJSON_AddStringToObject(e, "id", ev->id);
            break;
        case MUSE_HW_UI:
            cJSON_AddStringToObject(e, "app", ev->app);
            cJSON_AddStringToObject(e, "id", ev->id);
            cJSON_AddStringToObject(e, "event", ev->ui_event);
            cJSON_AddNumberToObject(e, "value", ev->value);
            if (ev->text[0]) cJSON_AddStringToObject(e, "text", ev->text);
            break;
        case MUSE_HW_PET:
            cJSON_AddStringToObject(e, "what", ev->id);
            cJSON_AddNumberToObject(e, "value", ev->value);
            break;
        case MUSE_HW_DETECTION:
            cJSON_AddStringToObject(e, "label", ev->id);
            cJSON_AddNumberToObject(e, "score", ev->value);
            if (ev->x2 || ev->y2) {   // a box: x, y, w, h in the camera's 416x416 frame
                cJSON_AddNumberToObject(e, "x", ev->x);
                cJSON_AddNumberToObject(e, "y", ev->y);
                cJSON_AddNumberToObject(e, "w", ev->x2);
                cJSON_AddNumberToObject(e, "h", ev->y2);
            }
            break;
    }
    return e;
}

typedef struct {
    reply_to_t to;
    uint32_t after;
    int wait_ms;
} input_wait_t;

static cJSON *events_result(uint32_t after, int wait_ms) {
    // 4 KB, held for the whole wait: PSRAM, not the internal RAM malloc would give it.
    muse_hw_event_t *ev = heap_caps_malloc(EVENTS_MAX * sizeof(*ev), MALLOC_CAP_SPIRAM);
    if (!ev) return error_result("out_of_memory", "failed to allocate");
    int n = muse_hw_events(after, ev, EVENTS_MAX, wait_ms);
    int64_t now = esp_timer_get_time();
    cJSON *pl = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(pl, "events");
    for (int i = 0; i < n; i++) {
        cJSON *e = hw_event_json(&ev[i], now);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddNumberToObject(pl, "last_seq", n ? ev[n - 1].seq : (after > muse_hw_last_seq() ? muse_hw_last_seq() : after));
    cJSON_AddBoolToObject(pl, "captured", muse_hw_captured());
    free(ev);
    return ok_result(pl);
}

// Finished waiters, suspended for the next input.read to delete: a WithCaps task
// deleting itself needs a helper task in internal RAM, and aborts without one.
#define PARKED_MAX 4
static TaskHandle_t s_parked[PARKED_MAX];
static portMUX_TYPE s_parked_lock = portMUX_INITIALIZER_UNLOCKED;

static void reap_waiters(void) {
    for (int i = 0; i < PARKED_MAX; i++) {
        taskENTER_CRITICAL(&s_parked_lock);
        TaskHandle_t t = s_parked[i];
        s_parked[i] = NULL;
        taskEXIT_CRITICAL(&s_parked_lock);
        // Safe even if it hasn't suspended itself yet: this suspends it and waits.
        if (t) vTaskDeleteWithCaps(t);
    }
}

// Its stack in PSRAM: it only waits and replies, never touching flash.
static void input_wait_task(void *arg) {
    input_wait_t *w = arg;
    reply(&w->to, events_result(w->after, w->wait_ms));
    free(w);
    reap_waiters();   // those before it: at most one is left for the next read
    bool parked = false;
    taskENTER_CRITICAL(&s_parked_lock);
    for (int i = 0; i < PARKED_MAX && !parked; i++) {
        if (!s_parked[i]) {
            s_parked[i] = xTaskGetCurrentTaskHandle();
            parked = true;
        }
    }
    taskEXIT_CRITICAL(&s_parked_lock);
    if (parked) {
        for (;;) vTaskSuspend(NULL);
    }
    vTaskDeleteWithCaps(NULL);
}

static cJSON *input_command(cJSON *params, const char *request_id,
                            noise_ctrl_session_generation_t session_generation) {
    int wait_ms = 0, after, capture_s;
    get_int(params, "wait_ms", &wait_ms);
    wait_ms = clamp(wait_ms, 0, WAIT_MAX_MS);
    if (!get_int(params, "after", &after) || after < 0) after = (int)muse_hw_last_seq();
    if (get_int(params, "capture_s", &capture_s)) {
        capture_s = clamp(capture_s, 0, CAPTURE_MAX_S);
        muse_hw_capture(capture_s ? esp_timer_get_time() + (int64_t)capture_s * 1000000 : 0);
    }
    if (!wait_ms) return events_result((uint32_t)after, 0);
    reap_waiters();
    input_wait_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w) return error_result("out_of_memory", "failed to allocate");
    set_reply_to(&w->to, request_id, session_generation);
    w->after = (uint32_t)after;
    w->wait_ms = wait_ms;
    if (xTaskCreatePinnedToCoreWithCaps(input_wait_task, "input_wait", 3072, w, 4, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        free(w);
        return error_result("out_of_memory", "failed to start task");
    }
    return async_result();
}

// ---- Audio: clips and tones, on one worker with room for the MP3 decoder ----

typedef enum { JOB_URL, JOB_TONES, JOB_RECORD } job_kind_t;

typedef struct {
    job_kind_t kind;
    char *arg;  // the URL or the tones; JOB_RECORD: a path on the card to save to, or NULL
    int seconds, rate;  // JOB_RECORD
    bool include;       // JOB_RECORD: the audio in the result too, when saving
    reply_to_t to;
} audio_job_t;

typedef struct {
    reply_to_t to;
    double seconds;
    const char *format;
} play_ctx_t;

// A float to one decimal, as a double that prints short (0.8, not 0.800000011920929).
static double tenths(double v) {
    return round(v * 10) / 10;
}

static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_jobs_lock;   // one first start of the worker: link, console and scripts all queue jobs
static StaticSemaphore_t s_jobs_lock_buf;
static SemaphoreHandle_t s_played;

typedef struct {
    uint8_t *data;
    size_t len, cap;
    bool too_big;
} download_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    download_t *d = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || d->too_big) return ESP_OK;
    // Only the final response's body, not a redirect's.
    int status = esp_http_client_get_status_code(evt->client);
    if (status >= 300 && status < 400) return ESP_OK;
    if (d->len + evt->data_len > DOWNLOAD_MAX) {
        d->too_big = true;
        return ESP_FAIL;
    }
    if (d->len + evt->data_len > d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64 * 1024;
        while (cap < d->len + evt->data_len) cap *= 2;
        if (cap > DOWNLOAD_MAX) cap = DOWNLOAD_MAX;
        uint8_t *p = heap_caps_realloc(d->data, cap, MALLOC_CAP_SPIRAM);
        if (!p) {
            d->too_big = true;
            return ESP_FAIL;
        }
        d->data = p;
        d->cap = cap;
    }
    memcpy(d->data + d->len, evt->data, evt->data_len);
    d->len += evt->data_len;
    return ESP_OK;
}

static const char *download(const char *url, download_t *d) {
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = FETCH_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .event_handler = on_http_event,
        .user_data = d,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return "couldn't start the download";
    esp_err_t err = esp_http_client_perform(http);
    int status = esp_http_client_get_status_code(http);
    esp_http_client_cleanup(http);
    if (d->too_big) return "the file is over 1 MB, or there's no memory for it";
    if (err != ESP_OK) return "download failed";
    if (status != 200) return "the server didn't return the file";
    if (!d->len) return "the file is empty";
    return NULL;
}

const char *hw_download(const char *url, size_t max, uint8_t **data, size_t *len) {
    download_t d = { 0 };
    const char *err = download(url, &d);
    if (!err && d.len > max) err = "the file is too large";
    if (err) {
        free(d.data);
        return err;
    }
    *data = d.data;
    *len = d.len;
    return NULL;
}

static const char *decode_mp3(const uint8_t *d, size_t len, resampler_t *r) {
    mp3dec_t *dec = heap_caps_malloc(sizeof(*dec), MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!dec || !pcm) {
        free(dec);
        free(pcm);
        return "out of memory";
    }
    mp3dec_init(dec);
    int frames = 0;
    for (size_t off = 0; off < len && r->n < r->cap;) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, d + off, (int)(len - off), pcm, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        if (!samples) continue;
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) pcm[k] = (int16_t)((pcm[2 * k] + pcm[2 * k + 1]) / 2);
        }
        resample(r, info.hz, pcm, samples);
        frames++;
    }
    free(dec);
    free(pcm);
    return frames ? NULL : "no MP3 audio found";
}

static void play_done(bool played, void *user) {
    play_ctx_t *c = user;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddBoolToObject(pl, "played", played);
    cJSON_AddNumberToObject(pl, "seconds", tenths(c->seconds));
    cJSON_AddStringToObject(pl, "format", c->format);
    reply(&c->to, ok_result(pl));
    free(c);
    xSemaphoreGive(s_played);
}

// The PCM to play, sized for the longest clip or tune.
static bool alloc_pcm(resampler_t *r, int max_s) {
    r->cap = (size_t)max_s * RATE;
    r->out = heap_caps_malloc(r->cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    return r->out != NULL;
}

static void run_job(audio_job_t *job) {
    resampler_t r = { 0 };
    const char *err = NULL, *format = "tones";
    if (job->kind == JOB_TONES) {
        err = alloc_pcm(&r, TONES_MAX_MS / 1000 + 1) ? synth_tones(job->arg, &r) : "no memory for the audio";
    } else {
        // Downloaded first, so a failed fetch never holds the PCM buffer too.
        download_t d = { 0 };
        if (strncmp(job->arg, "http://", 7) && strncmp(job->arg, "https://", 8)) {
            d.data = hw_storage_load(job->arg, DOWNLOAD_MAX, &d.len, &err);   // a file on the card
        } else {
            err = download(job->arg, &d);
        }
        if (!err && !alloc_pcm(&r, CLIP_MAX_S)) err = "no memory for the audio";
        if (!err) {
            bool wav = d.len >= 12 && !memcmp(d.data, "RIFF", 4);
            format = wav ? "wav" : "mp3";
            err = wav ? decode_wav(d.data, d.len, &r) : decode_mp3(d.data, d.len, &r);
        }
        free(d.data);
    }
    if (!err && !r.n) err = "no audio";
    play_ctx_t *c = err ? NULL : calloc(1, sizeof(*c));
    if (!err && !c) err = "out of memory";
    if (err) {
        free(r.out);
        reply(&job->to, error_result(job->kind == JOB_URL ? "fetch_failed" : "invalid_params", err));
        return;
    }
    c->to = job->to;
    c->seconds = (double)r.n / RATE;
    c->format = format;
    ESP_LOGI(TAG, "playing %.1f s of %s", c->seconds, format);
    xSemaphoreTake(s_played, 0);
    if (!muse_voice_request_play(r.out, r.n, play_done, c)) {
        free(r.out);
        free(c);
        reply(&job->to, error_result("busy", "a clip is already playing"));
        return;
    }
    // One clip at a time: the next job waits for this one to end.
    xSemaphoreTake(s_played, portMAX_DELAY);
}

static size_t s_recorded;

static void record_done(size_t frames, void *user) {
    (void)user;
    s_recorded = frames;
    xSemaphoreGive(s_played);
}

// Recorded on the voice task, encoded here, so a long clip never holds it up.
static void run_record(audio_job_t *job) {
    size_t frames = (size_t)job->seconds * RATE;
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        reply(&job->to, error_result("out_of_memory", "no memory for the recording"));
        return;
    }
    xSemaphoreTake(s_played, 0);
    if (!muse_voice_request_record(pcm, frames, record_done, NULL)) {
        free(pcm);
        reply(&job->to, error_result("busy", "already recording"));
        return;
    }
    xSemaphoreTake(s_played, portMAX_DELAY);
    size_t got = s_recorded;
    if (job->rate == 8000) got = halve_rate(pcm, got);
    size_t bytes = 44 + got * 2;
    uint8_t *wav = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    char *b64 = heap_caps_malloc((bytes + 2) / 3 * 4 + 1, MALLOC_CAP_SPIRAM);
    if (!wav || !b64) {
        free(pcm);
        free(wav);
        free(b64);
        reply(&job->to, error_result("out_of_memory", "no memory to encode the recording"));
        return;
    }
    wav_header(wav, (uint32_t)got, (uint32_t)job->rate);
    memcpy(wav + 44, pcm, got * 2);
    free(pcm);
    const char *save_err = job->arg ? hw_storage_save(job->arg, wav, bytes, false) : NULL;
    base64(wav, bytes, b64);
    free(wav);
    if (save_err) {
        free(b64);
        reply(&job->to, error_result("save_failed", save_err));
        return;
    }
    cJSON *pl = cJSON_CreateObject();
    if (job->arg) cJSON_AddStringToObject(pl, "saved", job->arg);
    cJSON_AddStringToObject(pl, "format", "wav");
    cJSON_AddNumberToObject(pl, "sample_rate", job->rate);
    cJSON_AddNumberToObject(pl, "seconds", tenths((double)got / job->rate));
    cJSON_AddBoolToObject(pl, "cut_short", got < (size_t)job->seconds * job->rate);
    if (!job->arg || job->include) cJSON_AddStringToObject(pl, "audio_base64", b64);
    free(b64);
    reply(&job->to, ok_result(pl));
}

static void audio_task(void *arg) {
    QueueHandle_t jobs = arg;
    audio_job_t job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) == pdTRUE) {
            if (job.kind == JOB_RECORD) {
                run_record(&job);
            } else {
                run_job(&job);
            }
            free(job.arg);
        }
    }
}

static cJSON *queue_job(audio_job_t job, const char *request_id,
                        noise_ctrl_session_generation_t session_generation);

static cJSON *queue_audio(job_kind_t kind, const char *arg, const char *request_id,
                          noise_ctrl_session_generation_t session_generation) {
    if (!muse_settings_speaker_on()) {
        return error_result("speaker_off", "the speaker is turned off in Muse's settings");
    }
    audio_job_t job = { .kind = kind, .arg = strdup(arg) };
    if (!job.arg) return error_result("out_of_memory", "failed to allocate");
    return queue_job(job, request_id, session_generation);
}

static cJSON *queue_job(audio_job_t job, const char *request_id,
                        noise_ctrl_session_generation_t session_generation) {
    xSemaphoreTake(s_jobs_lock, portMAX_DELAY);
    if (!s_jobs) {
        QueueHandle_t jobs = xQueueCreate(2, sizeof(audio_job_t));
        if (!s_played) s_played = xSemaphoreCreateBinary();
        // Stack in PSRAM: the decoder's scratch is big, and this task never writes flash.
        if (!jobs || !s_played ||
            xTaskCreatePinnedToCoreWithCaps(audio_task, "muse_hw_audio", AUDIO_STACK, jobs, 4, NULL,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
            // All or nothing: a queue with no worker would take jobs nobody answers.
            xSemaphoreGive(s_jobs_lock);
            if (jobs) vQueueDelete(jobs);
            free(job.arg);
            return error_result("out_of_memory", "failed to start the audio worker");
        }
        s_jobs = jobs;
    }
    xSemaphoreGive(s_jobs_lock);
    set_reply_to(&job.to, request_id, session_generation);
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        free(job.arg);
        return error_result("busy", "two audio jobs are already waiting");
    }
    return async_result();
}

static cJSON *record_command(cJSON *params, const char *request_id,
                             noise_ctrl_session_generation_t session_generation) {
    int seconds = 3, rate = RATE;
    get_int(params, "seconds", &seconds);
    get_int(params, "sample_rate", &rate);
    if (rate != 16000 && rate != 8000) return error_result("invalid_params", "sample_rate is 16000 or 8000");
    seconds = clamp(seconds, 1, rate == 8000 ? RECORD_MAX_S_8K : RECORD_MAX_S);
    const char *save = get_str(params, "save");
    audio_job_t job = { .kind = JOB_RECORD, .seconds = seconds, .rate = rate,
                        .include = cJSON_IsTrue(cJSON_GetObjectItem(params, "include_audio")) };
    if (save && !(job.arg = strdup(save))) return error_result("out_of_memory", "failed to allocate");
    return queue_job(job, request_id, session_generation);
}

// ---- wifi.scan, device.reboot, device.power_off ----------------------------

static void scan_task(void *arg) {
    reply_to_t *to = arg;
    cJSON *result;
    if (muse_wifi_scan() != ESP_OK) {
        result = error_result("unavailable", "Wi-Fi is off or busy");
    } else {
        vTaskDelay(pdMS_TO_TICKS(200));
        for (int ms = 0; muse_wifi_scanning() && ms < SCAN_WAIT_MS; ms += 100) vTaskDelay(pdMS_TO_TICKS(100));
        muse_wifi_ap_t *aps = malloc(SCAN_MAX * sizeof(*aps));
        uint32_t gen = 0;
        int n = aps ? muse_wifi_scan_results(aps, SCAN_MAX, &gen) : 0;
        cJSON *pl = cJSON_CreateObject();
        cJSON *arr = cJSON_AddArrayToObject(pl, "networks");
        for (int i = 0; i < n; i++) {
            cJSON *ap = cJSON_CreateObject();
            cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
            cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(ap, "secure", aps[i].secure);
            cJSON_AddItemToArray(arr, ap);
        }
        free(aps);
        muse_wifi_status_t st;
        muse_wifi_status(&st);
        if (st.state == MUSE_WIFI_CONNECTED) cJSON_AddStringToObject(pl, "joined", st.ssid);
        result = ok_result(pl);
    }
    reply(to, result);
    free(to);
    vTaskDelete(NULL);
}

static cJSON *scan_command(const char *request_id, noise_ctrl_session_generation_t session_generation) {
    reply_to_t *to = calloc(1, sizeof(*to));
    if (!to) return error_result("out_of_memory", "failed to allocate");
    set_reply_to(to, request_id, session_generation);
    if (xTaskCreate(scan_task, "wifi_scan_cmd", 3072, to, 4, NULL) != pdPASS) {
        free(to);
        return error_result("out_of_memory", "failed to start task");
    }
    return async_result();
}

// After the reply has had time to go out.
static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(800));
    if (arg) {
        muse_input_request_power_off();
    } else {
        esp_restart();
    }
    vTaskDelete(NULL);
}

static cJSON *device_power_command(bool off) {
    if (xTaskCreate(reboot_task, "hw_reboot", 2048, off ? (void *)1 : NULL, 4, NULL) != pdPASS) {
        return error_result("out_of_memory", "failed to start task");
    }
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "status", off ? "powering off" : "rebooting");
    return ok_result(pl);
}

// ---- audio.set_volume, audio.listen ----------------------------------------

static cJSON *volume_command(cJSON *params) {
    int volume;
    if (!get_int(params, "volume", &volume)) return error_result("missing_param", "volume is required");
    volume = clamp(volume, 0, 100);
    muse_settings_set_volume(volume);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "volume", volume);
    return ok_result(pl);
}

static void listen_done(int ms, float avg_db, float peak_db, int loud_pct, void *user) {
    reply_to_t *to = user;
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddNumberToObject(pl, "seconds", tenths(ms / 1000.0));
    cJSON_AddNumberToObject(pl, "average_dbfs", tenths(avg_db));
    cJSON_AddNumberToObject(pl, "peak_dbfs", tenths(peak_db));
    cJSON_AddNumberToObject(pl, "loud_percent", loud_pct);
    reply(to, ok_result(pl));
    free(to);
}

static cJSON *listen_command(cJSON *params, const char *request_id,
                             noise_ctrl_session_generation_t session_generation) {
    int seconds = 3;
    get_int(params, "seconds", &seconds);
    seconds = clamp(seconds, 1, 10);
    reply_to_t *to = calloc(1, sizeof(*to));
    if (!to) return error_result("out_of_memory", "failed to allocate");
    set_reply_to(to, request_id, session_generation);
    if (!muse_voice_request_listen(seconds * 1000, listen_done, to)) {
        free(to);
        return error_result("busy", "already listening");
    }
    return async_result();
}

// ---- The serial console (tools/muse/hw.py) ---------------------------------

// Commands from the console run here, not on the serial task: some save
// settings to flash, which needs an internal stack, and the serial task's is
// small. Started on first use.
static QueueHandle_t s_console_q;
static noise_ctrl_command_cb s_dispatch;

void muse_hw_commands_set_dispatcher(noise_ctrl_command_cb dispatch) {
    s_dispatch = dispatch;
}

noise_ctrl_command_cb hw_dispatch(void) {
    return s_dispatch;
}

static void console_task(void *arg) {
    (void)arg;
    char *json;
    for (;;) {
        if (xQueueReceive(s_console_q, &json, portMAX_DELAY) != pdTRUE) continue;
        cJSON *root = cJSON_Parse(json);
        free(json);
        cJSON *id = root ? cJSON_GetObjectItem(root, "id") : NULL;
        char request_id[64] = "console";
        if (cJSON_IsString(id)) strlcpy(request_id, id->valuestring, sizeof(request_id));
        else if (cJSON_IsNumber(id)) snprintf(request_id, sizeof(request_id), "%d", id->valueint);
        const char *command = root ? get_str(root, "command") : NULL;
        cJSON *params = root ? cJSON_GetObjectItem(root, "params") : NULL;
        if (!cJSON_IsObject(params)) params = NULL;
        cJSON *result;
        if (!command) {
            result = error_result("invalid_params", "send {\"command\": ..., \"params\": {...}, \"id\": ...}");
        } else {
            result = s_dispatch ? s_dispatch(command, params, request_id, CONSOLE_SESSION)
                                : muse_hw_command(command, params, request_id, CONSOLE_SESSION);
            if (!result) result = error_result("unsupported", "not a command");
        }
        if (cJSON_IsTrue(cJSON_GetObjectItem(result, "_async"))) {
            cJSON_Delete(result);  // the reply follows
        } else {
            console_print(request_id, result);
        }
        cJSON_Delete(root);
    }
}

static void on_console(const char *json) {
    if (!s_console_q) {
        s_console_q = xQueueCreate(4, sizeof(char *));
        if (!s_console_q || xTaskCreate(console_task, "muse_hw_console", 6144, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "no memory for the hardware console");
            if (s_console_q) vQueueDelete(s_console_q);
            s_console_q = NULL;   // the next line tries again
            return;
        }
    }
    size_t n = strlen(json) + 1;
    char *copy = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);   // not strdup: under 4 KB, that's internal RAM
    if (copy) memcpy(copy, json, n);
    if (copy && xQueueSend(s_console_q, &copy, 0) != pdTRUE) free(copy);
}

void muse_hw_commands_init(void) {
    s_jobs_lock = xSemaphoreCreateMutexStatic(&s_jobs_lock_buf);
    muse_crash_init();
    noise_ctrl_set_result_hook(on_result);
    muse_hw_set_console(on_console);
    muse_hw_io_init();
    muse_hw_storage_init();
#if CPU_LOAD
    cpu_load_init();
#endif
#if CONFIG_MUSE_SCRIPTS
    muse_hw_apps_init();
    muse_script_init();
#endif
}

// ---- Dispatch --------------------------------------------------------------

cJSON *muse_hw_command(const char *command, cJSON *params, const char *request_id,
                       noise_ctrl_session_generation_t session_generation) {
    if (!muse_board) return NULL;
    if (strncmp(command, "camera.", 7) == 0) {
        return muse_hw_camera_command(command, params, request_id, session_generation);
    }
    if (!strncmp(command, "grove.", 6) || !strncmp(command, "i2c.", 4) || !strncmp(command, "uart.", 5)
        || !strcmp(command, "device.time")) {
        return muse_hw_io_command(command, params, session_generation != HW_SCRIPT_SESSION);
    }
    if (strncmp(command, "storage.", 8) == 0) return muse_hw_storage_command(command, params);
    if (strncmp(command, "pet.", 4) == 0) return muse_hw_pet_command(command, params);
#if CONFIG_MUSE_SCRIPTS
    if (strncmp(command, "app.", 4) == 0) {
        return muse_hw_apps_command(command, params, request_id, session_generation);
    }
#endif
#if CONFIG_MUSE_SCRIPTS
    if (strncmp(command, "script.", 7) == 0) {
        // Scripts don't manage scripts.
        if (session_generation == HW_SCRIPT_SESSION) return error_result("unsupported", "not from a script");
        return muse_script_command(command, params);
    }
#endif
    if (strcmp(command, "device.status") == 0) return status_command();
    if (strcmp(command, "display.show_text") == 0) return show_text_command(params);
    if (strcmp(command, "display.set_brightness") == 0) return brightness_command(params);
    if (strcmp(command, "display.show_ui") == 0) return ui_command(params);
    if (strcmp(command, "audio.record") == 0) return record_command(params, request_id, session_generation);
    if (strcmp(command, "wifi.scan") == 0) return scan_command(request_id, session_generation);
    if (strcmp(command, "device.reboot") == 0) return device_power_command(false);
    if (strcmp(command, "device.power_off") == 0) return device_power_command(true);
    if (strcmp(command, "display.power") == 0) return power_command(params);
    if (strcmp(command, "led.set") == 0) return led_command(params);
    if (strcmp(command, "input.read") == 0) return input_command(params, request_id, session_generation);
    if (strcmp(command, "audio.play_url") == 0) {
        const char *url = get_str(params, "url");
        if (!url || !url[0]) return error_result("missing_param", "an http(s):// url or a path on the SD card is required");
        return queue_audio(JOB_URL, url, request_id, session_generation);
    }
    if (strcmp(command, "audio.beep") == 0) {
        const char *tones = get_str(params, "tones");
        if (!tones) return error_result("missing_param", "tones is required");
        return queue_audio(JOB_TONES, tones, request_id, session_generation);
    }
    if (strcmp(command, "audio.set_volume") == 0) return volume_command(params);
    if (strcmp(command, "audio.listen") == 0) return listen_command(params, request_id, session_generation);
    return NULL;
}
