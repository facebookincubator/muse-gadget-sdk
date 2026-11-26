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

#include "mg_glue.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app.h"
#include "identity.h"
#include "ble_server.h"
#include "led_status.h"
#include "config_store.h"
#include "link_pairing.h"
#include "mg_ble.h"
#include "mg_mic_gain.h"
#include "mg_spk_report.h"
#include "muse_bootguard.h"
#include "muse_local_press.h"
#include "ota.h"
#if CONFIG_MUSE_GADGET_BLE_SESSION || (CONFIG_MUSE_GADGET_BLE_SMP && !CONFIG_MUSE_GADGET_BLE_STANDALONE)
#include "noise_control.h"
#endif

#if CONFIG_MUSE_ENABLED
#include "mgcommands.h"
#include "muse_audio.h"
#include "muse_audio_dma.h"
#include "muse_battery.h"
#include "muse_board.h"
#include "muse_home.h"
#include "muse_input.h"
#include "muse_settings.h"
#include "muse_spk_dsp.h"
#include "muse_state.h"
#include "muse_touch.h"
#include "muse_voice.h"
#if CONFIG_MUSE_GADGET_BLE_VIDEO
#include "muse_video.h"
#endif
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
#include "muse_call.h"
#include "muse_subs.h"
#endif
#include "muse_clock.h"
#endif
#if CONFIG_HOMEHUB_VOICE && !CONFIG_MUSE_ENABLED
#include "voice.h"
#endif

#if CONFIG_MUSE_ENABLED
static const char *model(void) {
    return muse_board ? muse_board->name : NULL;
}

static int battery_pct(void) {
    return muse_state_power().battery_pct;   // -1 without a battery
}

// set_mic_gain (mgcommands.h): a 1-100 level for this connection's
// captures, 50 = the board's tuned gain (MUSE_MIC_GAIN_TUNED); 1-50 from the
// codec's lowest step (0 dB) up to tuned, 50-100 from tuned up to the board's
// no-clip ceiling for speech at 30 cm (mic_gain_speech_max_db; tuned when it
// has no headroom), in 3 dB steps (mg_mic_gain.h). Applied at once, a running
// capture included (muse_audio applies it at its next read, whatever the
// capture's format). RAM only: never saved, the user's Settings gain
// untouched; 0 (the connection ended) goes back to it. On the BLE host:
// doesn't block.
static void mic_gain(uint8_t level) {
    if (!level) {
        muse_audio_mic_gain_override(-1);
        ESP_LOGI("link.mg", "mic gain back to %d dB (connection ended)", muse_settings_mic_gain());
        return;
    }
    const muse_board_t *b = muse_board_get();
    int ceil_db = b && b->mic_gain_speech_max_db ? b->mic_gain_speech_max_db : MUSE_MIC_GAIN_TUNED;
    int db = mg_mic_gain_db(level, 0, MUSE_MIC_GAIN_TUNED, ceil_db, MG_MIC_GAIN_STEP_DB);
    muse_audio_mic_gain_override(db);
    ESP_LOGI("link.mg", "set_mic_gain %d: %d dB for this connection (1 = 0, 50 = %d, 100 = %d dB)", level, db,
             MUSE_MIC_GAIN_TUNED, ceil_db);
}

// The actuator the haptics core drives (mgcommands.h, Haptics). The StopWatch's ERM is on
// or off (no strength), takes timing lists, and a pulse under ~30 ms isn't felt.
static const uint8_t s_erm_actuator[4] = { mg_haptic_actuator_erm, mg_haptic_actuator_flag_timing, 30, 0 };

static void haptic_motor(bool on, uint16_t max_ms) {
    const muse_board_t *b = muse_board_get();
    if (b && b->buzz) {
        b->buzz(on ? max_ms : 0, 100);   // the core gates it (haptics_enabled, the rules)
    }
}

static bool haptics_on(void) {
    return muse_settings_haptics();
}

static bool speaker_write(const int16_t *pcm, size_t frames) {
    // The mouth follows the audio, as it does for the Wi-Fi path's replies.
    muse_state_set_level(muse_settings_speaker_on() ? muse_audio_level(pcm, frames) : 0);
    // The volume and speaker settings apply. Written as they are: the player ran
    // the board's speech chain already (speaker_chain below), and its barge-in
    // rewind is what the DMA held, post-chain.
    return muse_audio_write_chained(pcm, frames) == ESP_OK;
}

// The speaker's output depth, for buffer_update's queued ms and the live-depth
// floor (mg_play_live_floor_us): its I2S DMA (the board's i2s_dma_desc x
// i2s_dma_frames at the output rate; ESP-IDF's defaults when unset), and on a
// board with a speech chain (the ESP-Mosaico's) the chain's look-ahead, which
// holds MUSE_SPK_DSP_LOOKAHEAD samples (2 ms) before they reach the DMA:
// 72 ms there, 70 on the other boards. Counted with the
// chain on, its default (>spk.dsp off only shortens the path).
static uint32_t speaker_depth_us(void) {
    const muse_board_t *b = muse_board_get();
    if (!b) return UINT32_MAX;
    uint32_t us = muse_audio_dma_depth_us(b->i2s_dma_desc, b->i2s_dma_frames, MUSE_AUDIO_RATE);
    if (b->speaker_speech_chain) {
        us += (uint32_t)((uint64_t)MUSE_SPK_DSP_LOOKAHEAD * 1000000u / MUSE_AUDIO_RATE);
    }
    return us;
}

// musegadgets speaker_volume is the Muse speaker setting: 0 is off, any other
// value is on at that volume (the level is kept while it's off).
static int speaker_volume(void) {
    return muse_settings_speaker_on() ? muse_settings_volume() : 0;
}

/* Which levels the client hears (mg_spk_report.h): a change made on the
 * device, and only that, is reported. The settings notify from whichever
 * task changed them (LVGL, the input task, the BLE worker for the client's
 * own set), so the state is kept under a spinlock; the report goes out
 * after it. */
static mg_spk_report_t s_spk_report = { .reported = -1, .client_value = -1 };
static portMUX_TYPE s_spk_mux = portMUX_INITIALIZER_UNLOCKED;

static void report_speaker_volume(void) {
    mg_ble_report_setting(mg_setting_parameter_speaker_volume);   // logged on mg.ble (the Nordic UART mirror)
}

static void set_speaker_volume(uint8_t volume) {
    portENTER_CRITICAL(&s_spk_mux);
    mg_spk_client_begin(&s_spk_report, volume);   /* the client's own change: no report back */
    portEXIT_CRITICAL(&s_spk_mux);
    /* A write of the level the speaker already plays at changes nothing: nothing
     * is saved, and a bench volume (vol=, RAM only) stays in RAM. A real
     * change is the user's phone setting: saved (ending any bench volume). */
    if (volume != speaker_volume()) {
        /* By the phone: the gadget shows it ("Set by phone"). */
        if (volume == 0) {
            muse_settings_set_speaker_on_by(false, MUSE_VOL_BY_PHONE);
        } else {
            muse_settings_set_volume_by(volume, MUSE_VOL_BY_PHONE);
            muse_settings_set_speaker_on_by(true, MUSE_VOL_BY_PHONE);
        }
    }
    int now = speaker_volume();
    portENTER_CRITICAL(&s_spk_mux);
    bool report = mg_spk_client_end(&s_spk_report, now);
    portEXIT_CRITICAL(&s_spk_mux);
    if (report) report_speaker_volume();   // a device change landed during the client's
}

// The device's user changed the volume or muted (buttons, a call's controls,
// the Sound page): the client hears speaker_volume.
static void speaker_observer(muse_setting_t what) {
    if (what != MUSE_SETTING_VOLUME && what != MUSE_SETTING_SPEAKER) return;
    int v = speaker_volume();
    portENTER_CRITICAL(&s_spk_mux);
    bool report = mg_spk_changed(&s_spk_report, v);
    portEXIT_CRITICAL(&s_spk_mux);
    if (report) report_speaker_volume();
}

// The board's speaker makeup gain for stream_audio (muse_board_t.speaker_boost_db10).
static int speaker_boost_db10(void) {
    return muse_board ? muse_board->speaker_boost_db10 : 0;
}

// The board's speech chain for the player's replies (muse_board_t.speaker_speech_chain,
// the ESP-Mosaico): its own instance, apart from muse_audio_write's for the
// device's sounds, in PSRAM where there is some. mg_ble.c's player decides when
// it runs (mg_chain_t, mg_chain.h): every reply chunk, silence while it still
// holds audio, emptied when the speaker already went quiet on a reply.
static muse_spk_chain_t *s_play_chain;

static void speaker_chain(int16_t *pcm, size_t n) {
    muse_spk_chain_run(s_play_chain, pcm, n, MUSE_AUDIO_RATE);
}

static void speaker_chain_reset(void) {
    muse_spk_chain_reset(s_play_chain);
}

static void attention(void) {
    muse_state_poke();    // keeps the screen (and the codecs) on for playback
    muse_state_nudge();   // wakes the voice task for a capture request
}

// report_battery_status (device_action 9) from the latest reading the
// input task took (muse_battery), never a bus access from the BLE worker.
static bool battery_status(mg_battery_status_t *out) {
    int64_t at = muse_battery_reading_us();
    if (!at) return false;   // no reading yet: the client hears busy
    muse_power_t p = muse_state_power();
    bool present = p.battery_pct >= 0 || p.battery_mv > 0;
    *out = (mg_battery_status_t){
        .voltage_mv = (uint16_t)(p.battery_mv > 0 ? p.battery_mv : 0),
        .soc_percent = (uint8_t)(p.battery_pct >= 0 ? p.battery_pct : 0),
        .timestamp_ms = (uint32_t)(at / 1000),
        .flags = (uint8_t)((p.charging ? mg_battery_status_flag_charging : 0) |
                           (present ? mg_battery_status_flag_present : 0)),
    };
    if (p.has_current) {
        out->current_100ua = (int16_t)p.current_100ua;
        out->flags |= mg_battery_status_flag_current_measured;
    }
    if (p.has_temp) out->temperature_c = (int16_t)p.temp_c;
    out->capacity_mah = (uint16_t)p.remaining_mah;
    out->full_capacity_mah = (uint16_t)p.full_mah;
    return true;
}

// Charging started or stopped (or USB came or went): told unsolicited.
static void battery_listener(const muse_power_t *p, bool changed) {
    (void)p;
    if (changed) mg_ble_report_battery();
}

// power_off (device_action 10): the same goodbye and power-off as the
// device's own Power off, on the input task.
static void power_off(void) {
    muse_input_request_power_off();
}

static bool power_off_ready(void) {
    const muse_board_t *b = muse_board ? muse_board : muse_board_get();
    return !b->power_off_battery_only || muse_state_on_battery_real();   // never a bench nap's as-if battery
}

#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
// Live call's view (the call's controls, its video) and subtitles draw into
// PSRAM buffers: a board without PSRAM can't show them, so it doesn't offer
// them (features list only what works; unlisted commands are answered
// unsupported).
static bool board_draws_calls(void) {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}
#endif
#elif CONFIG_HOMEHUB_VOICE
static const char *model(void) {
    return "Home Assistant Voice PE";
}
#endif

// A stored key that can't be read: NVS "not found" is no key; any other read error, or a value that isn't 64
// hex digits, is a key that can't be read. Still no proof matches, and set_up() then says set up, so the owner
// check fails closed instead of reading the gadget as unset and open.
static atomic_bool s_key_unreadable;

// K from its 64 hex digits; wipes the digits.
static bool proof_key_from_hex(char *hex, size_t hex_size, uint8_t k[MG_TOKEN_PROOF_KEY_LEN]) {
    bool ok = strlen(hex) == 2 * MG_TOKEN_PROOF_KEY_LEN;
    for (int i = 0; ok && i < MG_TOKEN_PROOF_KEY_LEN; i++) {
        uint8_t b = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[2 * i + j];
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            ok = ok && v >= 0;
            b = (uint8_t)(b << 4 | (v & 0x0f));
        }
        k[i] = b;
    }
    mg_token_proof_wipe(hex, hex_size);
    if (!ok) mg_token_proof_wipe(k, MG_TOKEN_PROOF_KEY_LEN);
    return ok;
}

// The token proof key Link setup stored in its config, as 64 hex digits (the
// committed setup's while a re-provisioning journal is open, as the pair).
static bool proof_key(uint8_t k[MG_TOKEN_PROOF_KEY_LEN]) {
    char hex[2 * MG_TOKEN_PROOF_KEY_LEN + 1] = {0};
    if (!config_get_proof_key_hex(hex, sizeof(hex))) hex[0] = '\0';
    bool ok = proof_key_from_hex(hex, sizeof(hex), k);
    // config_key_lookup tells absence from a read that failed (or a value that's there but malformed).
    if (!ok && config_key_lookup(config_reprovision_open() ? "rp_k" : "mg_proof_k") != CONFIG_KEY_NOT_FOUND
        && !atomic_exchange(&s_key_unreadable, true)) {
        ESP_LOGE("link.mg", "token proof key unreadable or malformed: failing closed");
    }
    return ok;
}

// mg_ble's set_up: Link setup's marker, or a marker or key that can't be read.
static bool set_up_or_unreadable(void) {
    if (config_setup_complete_or_unreadable()) {
        return true;
    }
    uint8_t k[MG_TOKEN_PROOF_KEY_LEN];
    bool have = proof_key(k);
    mg_token_proof_wipe(k, sizeof(k));
    return have || atomic_load(&s_key_unreadable);
}

#if CONFIG_MUSE_GADGET_BLE_AUDIO
// The device session's credential (mgcommands.h, Device session): the node
// id, the token pair Link setup stored, and the device key with the VM it is
// bound to when setup left one. Never logged.

static bool session_credentials(mg_session_creds_t *c) {
    // The node id setup stored with K and the current pair, the committed
    // setup's while a setup again is under way
    // (never device_info's new offer before its commit), and K itself, in one
    // snapshot; a gadget set up before the node id was stored uses its
    // identity's id in use (homelink-).
    char hex[2 * MG_TOKEN_PROOF_KEY_LEN + 1] = {0};
    if (!app_gadget_read_session_setup(c->node_id, sizeof(c->node_id), c->access_token,
                                       sizeof(c->access_token), c->refresh_token,
                                       sizeof(c->refresh_token), &c->access_expires_at, hex,
                                       sizeof(hex))) {
        mg_token_proof_wipe(hex, sizeof(hex));
        return false;
    }
    _Static_assert(sizeof(c->proof_k) == MG_TOKEN_PROOF_KEY_LEN, "K");
    c->proof_k_set = proof_key_from_hex(hex, sizeof(hex), c->proof_k);
    if (!c->node_id[0]) {
        snprintf(c->node_id, sizeof(c->node_id), "%s", identity_node_id());
    }
    if (!config_get_str("device_key", c->device_key, sizeof(c->device_key))
        || !config_get_str("device_key_id", c->key_id, sizeof(c->key_id))) {
        c->device_key[0] = c->key_id[0] = '\0';
    }
    if (!c->device_key[0] || !config_get_str("vm_id", c->vm_id, sizeof(c->vm_id))) {
        c->vm_id[0] = '\0';
    }
    if (!c->device_key[0] || !config_get_str("vm_kind", c->vm_kind, sizeof(c->vm_kind))) {
        c->vm_kind[0] = '\0';
    }
    return true;
}
#endif

#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_GADGET_BLE_STANDALONE
// A Wi-Fi gadget holding no Wi-Fi credentials (set up token-only, or Wi-Fi
// forgotten) can't refresh its own token pair: the client does and writes it
// back. Asked at each request_status.
static bool holds_no_wifi(void) {
    char ssid[33] = {0};
    return !config_get_str("ssid", ssid, sizeof(ssid)) || !ssid[0];
}
#endif

#if CONFIG_MUSE_ENABLED
// Bench lines over the Nordic UART: only tuning that can't change setup or
// what the phone sees (the BLE console is unauthenticated).
static bool ble_console(char *line) {
    if (!strcmp(line, "guard?")) {   // read-only: the boot guard's counts
        muse_bootguard_log_state();
        return true;
    }
    if (!strcmp(line, "ota?")) {   // read-only: slots, states, bootloader rollback
        mg_ble_ota_log();
        return true;
    }
    static const char *const allowed[] = { "ui.md5=", "ui.lat=", "video.itvl=" };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (!strncmp(line, allowed[i], strlen(allowed[i]))) return muse_input_console_line(line);
    }
    // The vibration tests as plain lines too, the same
    // words as !IN vib= / vib?: open, queued to the input task's console (the
    // same handler and safety rules as the USB console's >vib=).
    if (!strncmp(line, "vib=", 4) || !strcmp(line, "vib?")) {
        if (!muse_input_console_post(line)) ESP_LOGW("link.mg", "%.32s: busy", line);
        return true;
    }
    return false;
}
#endif

#if CONFIG_MUSE_ENABLED
// The Nordic UART console's !IN <word> (mg_ble.c): presses of the device's
// own screen and buttons for a Mac client's checks and session.ping
// (one link.heartbeat on the device session, which changes nothing on either
// side), the read-only voice?, wake?, call? and mic.gain?, the vibration tests
// (vib=, vib?), mic.gain=N (the user's Settings mic gain), spk.dsp /
// spk.dsp= (the speaker's speech chain) and ptt.minrise= / ptt.minrise?
// (the bench guard), vol=N / vol=saved / vol? (the bench volume, RAM only),
// open in automation builds; a release build runs the state-changing ones
// only on a proven connection (nus_command);
// nothing else of the USB console (no setup, restart, power or tokens). Queued to the
// input task: the caller is the NimBLE host, which must not wait on the UI.
// 1 queued, 0 not a bench word, -1 the queue is full.
static int ble_bench_input(char *line) {
    // vib= / vib?: the vibration tests, the same
    // engine and rules as Developer > Tests > Vibration (no settings change).
    static const char *const exact[] = { "call", "call=0", "call?", "wake", "wake?", "voice?", "ptt=1", "ptt=0", "vib?",
                                         "mic.gain?", "dev?", "dev=1", "dev=0", "spk.dsp", "spk.dsp?", "ptt.minrise?",
                                         "vol?",
#if CONFIG_MUSE_GADGET_BLE_SESSION
                                         "session.ping",
#endif
#if CONFIG_MUSE_SESSION_IMAGE
                                         "img.ask", "img?",
#endif
    };
    static const char *const prefix[] = { "tap=", "press=", "key=", "page=", "vib=", "mic.gain=", "spk.dsp=", "ptt.minrise=", "vol=", "video.fps=",
#if CONFIG_MUSE_SESSION_IMAGE
                                          "img.show=",
#endif
    };
    bool ok = false;
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]) && !ok; i++) ok = !strcmp(line, exact[i]);
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]) && !ok; i++) ok = !strncmp(line, prefix[i], strlen(prefix[i]));
    if (!ok) return 0;
    return muse_input_console_post(line) ? 1 : -1;
}
#endif

#if CONFIG_MUSE_ENABLED && CONFIG_MUSE_GADGET_BLE_VIDEO
// musegadgets BLE as a muse_video transport: mg_ble's ring and its
// request_streaming, request_keyframe, buffer_update and stop_streaming.
static void ble_video_freed(const muse_video_status_t *st) {
    mg_ble_video_freed(st->shown, st->dropped, st->decode_ms);
    if (st->last_age_ms != 0xffff) mg_ble_video_shown(st->last_number, st->last_age_ms, st->shown);
}

static muse_video_transport_t s_ble_video = {
    .name = "ble",
    .freed = ble_video_freed,
    .keyframe = mg_ble_video_keyframe,
    .stop = mg_ble_video_stop,
    .request = mg_ble_video_request,
    .has_client = mg_ble_video_client,
};

static bool ble_video_begin(const mg_video_params_t *vp) {
    const muse_video_params_t p = {
        .codec = vp->codec == mg_data_type_video_h264 ? MUSE_VIDEO_CODEC_H264
                 : vp->codec == mg_data_type_video_vp8 ? MUSE_VIDEO_CODEC_VP8 : 0,
        .width = vp->width,
        .height = vp->height,
        .fps = vp->fps,
        .kbps = vp->kbps,
        .playout_ms = vp->playout_ms,
        .tiles = vp->tiles,
        .content = vp->content == mg_video_content_ui ? MUSE_VIDEO_CONTENT_UI : MUSE_VIDEO_CONTENT_VIDEO,
    };
#if CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    /* A UI stream and a live call don't run together yet (the S3's internal
     * RAM for both is unmeasured): the client hears busy. */
    if (p.content == MUSE_VIDEO_CONTENT_UI && muse_call_active()) {
        ESP_LOGW("link.mg", "UI stream refused: a live call is on (busy)");
        return false;
    }
#endif
    return muse_video_begin(&s_ble_video, &p);
}

static void ble_video_end(void) {
    muse_video_end(&s_ble_video);
}

// The board's offer in the protocol's terms (mg_ble adds its L2CAP PSM).
static const mg_video_caps_t *ble_video_caps(void) {
    static mg_video_caps_t caps;
    const muse_video_caps_t *c = muse_video_caps();
    if (!c) return NULL;   // a panel without draw_frame
    caps = (mg_video_caps_t){
        .display_w = c->display_w, .display_h = c->display_h, .round = c->round,
        .max_w = c->max_w, .max_h = c->max_h, .max_fps = c->max_fps,
        .sug_w = c->sug_w, .sug_h = c->sug_h, .sug_fps = c->sug_fps, .sug_kbps = c->sug_kbps,
        .max_playout_ms = c->max_playout_ms, .max_tiles = c->max_tiles, .tile_split_pct = c->tile_split_pct,
        .vp8 = c->vp8,
        .no_h264 = c->no_h264,
        .ui_w = c->ui_w, .ui_h = c->ui_h, .ui_fps = c->ui_fps, .ui_max_frame = c->ui_max_frame,
        .ui_tiles = c->ui_tiles, .ui_still = c->ui_still,
    };
    return &caps;
}
#endif

void mg_glue_setup_changed(void) {
    mg_ble_setup_changed();
}

void mg_glue_settings_reset(void) {
    mg_ble_settings_reset();
}

bool mg_glue_queue_forget(const char *why) {
    return mg_ble_queue_forget(why);
}

static void register_companion(void) {
    ble_companion_t companion = {
        .svcs = mg_ble_services(),
        .configure_host = mg_ble_configure_host,
        .on_gap_event = mg_ble_gap_event,
        .adv_uuid128 = mg_ble_adv_uuid128(),
        .wants_advertising = mg_ble_wants_advertising,
        .stalled = mg_ble_stalled,   // a stalled worker isn't advertised
    };
    companion.adv_uuid16 = mg_ble_adv_uuid16;
    companion.adv_appearance = mg_ble_adv_appearance;
    ble_server_set_companion(&companion);
}

#if CONFIG_MUSE_GADGET_BLE_SMP
// One update at a time. The Bluetooth update (mg_smp_esp.c) claims the
// other slot the way a Wi-Fi OTA does (ota.h), so neither starts while the
// other writes it; each refuses as busy.
bool mg_update_claim(bool take) {
    if (take) return ota_claim(OTA_OWNER_BLE);
    ota_release(OTA_OWNER_BLE);
    return true;
}
#if !CONFIG_MUSE_GADGET_BLE_STANDALONE
// A Wi-Fi + BLE gadget keeps an image a Bluetooth update installed at the
// first of: the control session up (the mark ota_verify_task keeps a
// Wi-Fi OTA's image at, and it does that for this one too), or the boot
// guard's healthy mark (BLE advertised or connected and 30 s up), saved
// network or not: BLE is the recovery tier, so an image that breaks Wi-Fi can
// still be replaced over BLE, and one updated away from its network isn't
// rolled back at its next reset. ota_verify_task never rolls back an image
// a Wi-Fi OTA didn't install (app.c).
//
// One verifier per installation: an image device.ota installed is kept at its
// own mark, the control session up (ota_verify_task), never at Bluetooth's
// before it.
static bool smp_keep_mark(void) {
    if (noise_ctrl_is_connected()) return true;
    if (ota_verify_recorded_for_running()) return false;
    return muse_bootguard_is_healthy() && ble_server_ever_reachable();
}
#endif
#endif

// Link setup under way (a pairing session): the link stays active for its
// whole duration (link modes).
static bool setup_active(void) {
    return link_pairing_session_generation() != 0;
}

// An unproven link's update asks for the confirm press (mg_ble.h local_press_ask).
static uint8_t glue_update_ask(const struct muse_update_req *req) {
    return (uint8_t)muse_local_press_ask(req);
}

#if CONFIG_MUSE_ENABLED
// SAFE mode (muse_bootguard.h): musegadgets for recovery only. The Muse
// UI, audio and input never started, so nothing here touches them: the token
// proof, the board's name, and the Nordic UART console's "safe.off", which
// turns Muse on again for the next boot.
static const char *safe_model(void) {
    const muse_board_t *b = muse_board_get();
    return b ? b->name : NULL;
}

// The Nordic UART's words in SAFE, after mg_ble's own (nus_command: the open
// bench words !PAIR AUTO, !RESTART, !SAFEID, status, version, link.wake/sleep
// and the rest, which SAFE keeps; !IN answers that this build has no bench
// input, since no Muse runs to take it). Only these differ from the normal
// console, and only while in SAFE, because SAFE runs no Muse feature for the
// other words to act on; leaving SAFE restarts into
// the normal console with every word.
static bool safe_console(char *line) {
    if (!strcmp(line, "ota?")) {
        mg_ble_ota_log();
        return true;
    }
    char out[64];   // safe.off / safe=0 (in SAFE: Muse on and a planned restart) and guard?, as on USB
    // The Nordic UART in SAFE: no token proof runs there, so its safe.off leaves SAFE but clears no loss count:
    // only the key hold, a proven word or the USB console does.
    if (!muse_bootguard_console_word_from(line, out, sizeof(out), false)) return false;
    if (out[0]) ESP_LOGW("link.mg", "%s", out);
    return true;
}

#if CONFIG_MUSE_ENABLED
static void vibrate(uint16_t freq_hz, uint16_t ms, uint8_t volume_pct) {
    (void)freq_hz;   // one motor speed
    const muse_board_t *b = muse_board_get();
    if (b && b->buzz && muse_settings_haptics()) {
        b->buzz(ms, volume_pct);
    }
}
#endif

// The recovery reason SAFE declares (mgcommands.h, Recovery): its cause.
static uint8_t safe_reason(muse_boot_why_t why) {
    switch (why) {
    case MUSE_BOOT_WHY_BUTTON: return mg_recovery_reason_user;
    case MUSE_BOOT_WHY_FAILED_BOOTS: return mg_recovery_reason_boot_loop;   // boots that never got healthy
    case MUSE_BOOT_WHY_CRASHES:
    case MUSE_BOOT_WHY_LOST: return mg_recovery_reason_crash_guard;   // runs that ended abnormally
    case MUSE_BOOT_WHY_KEPT_OFF: return mg_recovery_reason_muse_guard;   // Muse still off from those crashes
    default: return mg_recovery_reason_unspecified;
    }
}

static void start_safe(void) {
    static mg_ble_platform_t platform = {
        .proof_key = proof_key,
        .set_up = set_up_or_unreadable,
        .proof_clear = app_gadget_clear_setup,
        .model = safe_model,
        .console = safe_console,
        .refresh_advertising = ble_server_refresh_advertising,
        .safe_mode = true,
        .bench_silence = ble_server_bench_silence,
        .bench_setup_adv = ble_server_bench_setup_adv,
        .bench_pause = ble_server_bench_pause,
        .adv_fast = ble_server_adv_fast,   // a talk press with no app
        .setup_active = setup_active,
        .muse_gatt_refused = ble_server_muse_gatt_refused,   // SAFE on the public address
        /* SAFE declares recovery mode behind its own BLE identity. The
         * ESP32 boot guard's SAFE keeps Muse off until safe.off
         * (muse_guard); a plain restart stays SAFE, so no restart flag. */
        .recovery_mode = mg_device_mode_recovery,
        .recovery_reason = mg_recovery_reason_muse_guard,
        .recovery_flags = 0,
    };
    // A SAFE the user entered with the key takes a Bluetooth update with
    // no press (mg_smp_gate.h); one the guard entered itself can't ask for
    // the confirm press (its screen is off), so it answers rc 11 and USB or
    // the key's SAFE is the way. Such a recovery declares reason user, so a
    // client tells the two apart, and one the guard entered itself reports
    // its own cause.
    platform.safe_updates_open = muse_bootguard_safe_why() == MUSE_BOOT_WHY_BUTTON;
    platform.recovery_reason = safe_reason(muse_bootguard_safe_why());
    register_companion();
    mg_ble_init(&platform);
}
#endif

void mg_glue_start(void) {
#if CONFIG_MUSE_ENABLED
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        start_safe();
        return;
    }
#endif
    static mg_ble_platform_t platform = {
        // The token proof: K from Link setup; clear resets setup.
        .proof_key = proof_key,
        .set_up = set_up_or_unreadable,
        .proof_clear = app_gadget_clear_setup,
        .local_press_ask = glue_update_ask,   // an unproven link's update: the confirm press (mg_smp_gate.h)
#if CONFIG_MUSE_ENABLED
        .model = model,
        .battery_pct = battery_pct,
        .mic_gain = mic_gain,
        .attention = attention,
        .speaker_write = speaker_write,
        .speaker_depth_us = speaker_depth_us,
        .speaker_clear = muse_audio_speaker_clear,   // the barge-in's touch-down (muse_audio_tx.h)
        .assistant_state = muse_voice_assistant_state,   // the face follows the turn
        .speaker_volume = speaker_volume,
        .set_speaker_volume = set_speaker_volume,
        .speaker_boost_db10 = speaker_boost_db10,
        .console = ble_console,
        .bench_input = ble_bench_input,
        .bench_silence = ble_server_bench_silence,
        .bench_safe_identity = ble_server_bench_safe_identity,
        .bench_setup_adv = ble_server_bench_setup_adv,
        .bench_pause = ble_server_bench_pause,
        .adv_fast = ble_server_adv_fast,   // a talk press with no app
#if CONFIG_MUSE_GADGET_BLE_VIDEO
        .video_begin = ble_video_begin,
        .video_end = ble_video_end,
        .video_shown32 = muse_video_shown32,   // replies start with their anchored picture
#endif
        .clock_set = muse_clock_set,             // the phone's time (device_action set/get_unix_time)
        .clock_get = muse_clock_get,
#elif CONFIG_HOMEHUB_VOICE
        .model = model,
        .assistant_state = voice_assistant_state,   // the LED ring follows the turn
#endif
        // The bench switch on for the connected central: "Bench" in the
        // home's top row, or the status light's cue on a board with no screen.
#if CONFIG_MUSE_ENABLED
        .bench_mark = muse_home_set_bench,
#else
        .bench_mark = led_status_set_bench,
#endif
        .refresh_advertising = ble_server_refresh_advertising,
        .setup_active = setup_active,   // link modes: Link setup keeps the link active
        .muse_gatt_refused = ble_server_muse_gatt_refused,   // SAFE on the public address
        // The device session: its credential from Link's config, its
        // refreshed pair stored in one commit;
        // credentials only on a Wi-Fi gadget.
        .session_credentials = session_credentials,
        .session_store_tokens = app_gadget_store_session_tokens,
        .session_busy = app_gadget_session_busy,   // a re-setup committing
#if CONFIG_MUSE_GADGET_BLE_AUDIO && !CONFIG_MUSE_GADGET_BLE_STANDALONE
        .client_refreshes = holds_no_wifi,   // per connection
#endif
#if CONFIG_MUSE_GADGET_BLE_SESSION
        // BLE-only: the gadget's VM session over BLE, its messages to and
        // from noise_control.cpp.
        .session_state = noise_ctrl_ble_state,
        .session_message = noise_ctrl_ble_message,
        .user_action = noise_ctrl_user_action,   // a user action opens the session for ~2 min
#endif
    };
#if CONFIG_MUSE_ENABLED
    // The board, whether or not the Muse boot task has published muse_board yet.
    const muse_board_t *b = muse_board ? muse_board : muse_board_get();
    if (b && b->speaker_speech_chain &&
        (s_play_chain = heap_caps_calloc_prefer(1, sizeof(*s_play_chain), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_8BIT)) != NULL) {
        platform.speaker_chain = speaker_chain;
        platform.speaker_chain_reset = speaker_chain_reset;
    } else if (b && b->speaker_speech_chain) {
        ESP_LOGW("link.mg", "no memory for the speaker's speech chain: replies play unchained");
    }
#endif
#if CONFIG_MUSE_ENABLED && CONFIG_MUSE_GADGET_BLE_VIDEO
    platform.video_caps = ble_video_caps();   // NULL on a panel without draw_frame: no stream_video
#endif
#if CONFIG_MUSE_ENABLED && CONFIG_MUSE_GADGET_BLE_LIVE_CALL
    if (board_draws_calls()) {
        platform.live_call = muse_call_client_state;   // the face shows the call
        platform.subtitle_caps = muse_subs_caps;       // its subtitles, drawn over the video or face
        platform.subtitle = muse_subs_event;
        platform.subtitle_ready = muse_subs_ready;     // unlisted if the band's buffers didn't allocate
    } else {
        ESP_LOGI("link.mg", "no PSRAM: Live call and subtitles not offered");
    }
#endif
#if CONFIG_MUSE_ENABLED
    // A touchscreen: its touches for the client (ui_input) and the HID.
    // Battery and power actions where the board has them.
    if (b && b->read_power) {
        platform.battery_status = battery_status;
        muse_battery_set_listener(battery_listener);
    }
    if (b && b->power_off) {
        platform.power_off = power_off;
        if (b->power_off_battery_only) platform.power_off_ready = power_off_ready;
    }
    if (b && b->touch) {
        platform.touch_w = (uint16_t)b->width;
        platform.touch_h = (uint16_t)b->height;
        platform.touch_round = b->round;
        platform.touch_private = muse_touch_private;   // a passcode entry on screen: no touch goes out
        static const muse_touch_sink_t sink = {
            .wanted = mg_ble_touch_wanted,
            .sample = mg_ble_touch,
        };
        muse_touch_set_sink(&sink);
    }
    /* A vibration motor (the StopWatch's): the client's haptics_enabled and
     * ptt_buzz_* are offered, and the buzz comes at the hold threshold, when
     * the push-to-talk turn opens, unless Settings > Talk turned it off. */
    if (b && b->buzz) {
        platform.vibrate = vibrate;
        platform.haptic_actuator = s_erm_actuator;
        platform.haptic_motor = haptic_motor;
        platform.haptics_on = haptics_on;
    }
#endif
#if CONFIG_MUSE_GADGET_BLE_SMP && !CONFIG_MUSE_GADGET_BLE_STANDALONE
    platform.boot_healthy = smp_keep_mark;
#endif
    register_companion();
    mg_ble_init(&platform);
#if CONFIG_MUSE_ENABLED
    muse_settings_set_observer(speaker_observer);   // a level changed on the device reaches the client
#endif
#if CONFIG_MUSE_ENABLED && CONFIG_MUSE_GADGET_BLE_VIDEO
    s_ble_video.ring = mg_ble_vstream();   // allocated by mg_ble_init when video is offered
    if (s_ble_video.ring) muse_video_add_transport(&s_ble_video);
    muse_video_set_audio_clock(mg_ble_av_clock);   // the picture follows the reply's voice
#endif
}
