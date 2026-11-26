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
 * Stand-ins for the services the v1.0 home, Status page and Settings menu read
 * (muse_home.c, muse_settings_ui.c): settings in RAM, a clock the scenario
 * sets, a Live call client the scenario plays (muse_call.c itself is the
 * production code), no audio, battery or Wi-Fi.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sim_services.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "muse_board.h"
#include "muse_ui.h"

#include "muse_audio.h"
#include "muse_battery.h"
#include "muse_ble.h"
#include "mg_ble.h"
#include "mg_lct.h"
#include "muse_call.h"
#include "muse_chat.h"
#include "muse_clock.h"
#include "muse_input.h"
#include "muse_link.h"
#include "muse_avatar.h"
#include "network_proxy.h"
#include "muse_settings.h"
#include "muse_subs.h"
#include "muse_touch.h"
#include "muse_voice.h"
#include "muse_wifi.h"

extern unsigned sim_stubs_settings_writes;

static struct {
    int volume, mic_gain, sleep_s;
    bool sleep_never_charged;   /* Display > Sleep when charged: Never; Always by default */
    bool ble_on, wifi_on, haptics, developer;
    muse_talk_mode_t talk;
    bool clock_set, clock_24h;
    int hour, min;
    bool call_available;
} s = { .volume = 60, .mic_gain = 30, .sleep_s = 120, .ble_on = true, .haptics = true,
        .talk = MUSE_TALK_BOTH, .call_available = true };

void sim_stubs_set_clock(int hour, int min, bool h24)
{
    s.clock_set = hour >= 0;
    s.hour = hour;
    s.min = min;
    s.clock_24h = h24;
}

void sim_stubs_set_call_available(bool available)
{
    s.call_available = available;
}

void sim_stubs_set_volume(int pct)
{
    s.volume = pct;
}

/* ---- clock ---- */
bool muse_clock_local(struct tm *out)
{
    if (!s.clock_set) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_year = 2026 - 1900;
    out->tm_mon = 9;
    out->tm_mday = 30;
    out->tm_wday = 5;
    out->tm_hour = s.hour;
    out->tm_min = s.min;
    return true;
}
bool muse_clock_24h(void) { return s.clock_24h; }

/* ---- settings kept in RAM ---- */
int muse_settings_volume(void) { return s.volume; }
/* Who changed the speaker, as muse_settings.c counts it (none for the
 * scenario's volume= / speaker=, which stand for a boot's restore). */
static uint32_t s_vol_changes;
static muse_vol_by_t s_vol_by;
void sim_stubs_count_volume(muse_vol_by_t by) { s_vol_by = by; s_vol_changes++; }
uint32_t muse_settings_volume_changes(muse_vol_by_t *by)
{
    if (by) *by = s_vol_by;
    return s_vol_changes;
}
void muse_settings_set_volume_by(int pct, muse_vol_by_t by)
{
    s.volume = pct;
    sim_stubs_settings_writes++;
    sim_stubs_count_volume(by);
}
void muse_settings_set_volume(int pct) { muse_settings_set_volume_by(pct, MUSE_VOL_BY_DEVICE); }
int muse_settings_mic_gain(void) { return s.mic_gain; }
void muse_settings_set_mic_gain(int db) { s.mic_gain = db; sim_stubs_settings_writes++; }
int muse_settings_sleep_s(void) { return s.sleep_s; }
void muse_settings_set_sleep_s(int secs) { s.sleep_s = secs; sim_stubs_settings_writes++; }
bool muse_settings_sleep_when_charged(void) { return !s.sleep_never_charged; }
void muse_settings_set_sleep_when_charged(bool always) { s.sleep_never_charged = !always; sim_stubs_settings_writes++; }
bool muse_settings_ble_on(void) { return s.ble_on; }
void muse_settings_set_ble_on(bool on) { s.ble_on = on; }
bool muse_settings_wifi_on(void) { return s.wifi_on; }
void muse_settings_set_wifi_on(bool on) { s.wifi_on = on; }
bool muse_settings_haptics(void) { return s.haptics; }
void muse_settings_set_haptics(bool on) { s.haptics = on; sim_stubs_settings_writes++; }
/* Developer mode, in RAM here; each write counted (a vibration test
 * never changes a setting). */
unsigned sim_stubs_settings_writes;
bool muse_settings_developer(void) { return s.developer; }
void muse_settings_set_developer(bool on) { s.developer = on; sim_stubs_settings_writes++; printf("@developer %s\n", on ? "on" : "off"); }
void muse_settings_factory_reset(void) { s.developer = false; }
muse_talk_mode_t muse_settings_talk_mode(void) { return s.talk; }
void muse_settings_set_talk_mode(muse_talk_mode_t mode) { s.talk = mode; sim_stubs_settings_writes++; }
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]) { strcpy(out, "hatch.metaaivm.com"); }
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]) { out[0] = '\0'; }
size_t muse_settings_hatch_token_len(void) { return 0; }
bool muse_settings_set_hatch_host(const char *host) { (void)host; return true; }
void muse_settings_set_hatch_vm(const char *vm) { (void)vm; }
esp_err_t muse_settings_set_hatch_token(const char *token, bool append) { (void)token; (void)append; return ESP_OK; }
void muse_settings_set_wifi(const char *ssid, const char *pass) { (void)ssid; (void)pass; }

/* ---- audio, battery, voice ---- */
void muse_audio_set_mic_gain(int db) { (void)db; }
void muse_audio_set_volume(int volume) { (void)volume; }
void muse_battery_read(muse_battery_t *out) { memset(out, 0, sizeof(*out)); }
void muse_battery_reset(void) {}
muse_battery_level_t muse_battery_low_level(void) { return MUSE_BATTERY_LEVEL_OK; }
bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h) { (void)b; (void)rate10; (void)full_h; return false; }
float muse_voice_monitor_db(void) { return -60.0f; }
void muse_voice_request_chirp(void) {}
void muse_voice_set_monitor(bool on) { (void)on; }

/* ---- input ---- */
void muse_input_request_power_off(void) {}
bool muse_input_talk_choice(void) { return true; }
void muse_touch_attach(lv_indev_t *indev) { (void)indev; }
bool muse_touch_real(void) { return true; }   /* the simulator's pointer is a finger */
bool muse_touch_press_wireless(void) { return false; }
void muse_touch_set_shareable(bool (*shareable)(void)) { (void)shareable; }   /* no BLE sink here */
int muse_touch_downs_within(int ms) { (void)ms; return 0; }
/* The bench's named taps (the console's tap=): no console in the simulator. */
bool muse_touch_tap_obj(lv_obj_t *obj, int ms, muse_touch_gesture_t *g) { (void)obj; (void)ms; (void)g; return false; }
bool muse_touch_slide_obj(lv_obj_t *slider, int value, muse_touch_gesture_t *g) { (void)slider; (void)value; (void)g; return false; }

/* ---- Link, chat, Wi-Fi, BLE ---- */
const char *muse_hatch_state_name(muse_hatch_state_t state) { return state == MUSE_HATCH_REACHABLE ? "Connected" : "Not set up"; }
void muse_hatch_test(void) {}
bool muse_link_hatch_linked(void) { return true; }
bool muse_link_token_stored(void) { return sim_services_paired(); }
void muse_link_reset_setup(void) {}
bool g_sim_no_sdk_token;   /* scenario key sdk_token=0: a gadget without its SDK token */
const char *muse_link_sdk_token(void) { return g_sim_no_sdk_token ? NULL : "mgst_simulator"; }
bool muse_link_sdk_token_changed(void) { return false; }
bool muse_link_sdk_token_fp(unsigned *len, char hex[MUSE_LINK_FP_HEX]) { (void)len; (void)hex; return false; }
bool muse_link_sdk_token_set(const char *token) { (void)token; return false; }
const char *muse_link_state_name(muse_link_state_t state) { (void)state; return "Online"; }
/* No device session in the simulator: no Ask Muse for an image test. */
bool muse_link_image_test(void) { return false; }
/* A setup prompt takes the press; with the scenario's reconfirm=true it's a re-setup's prompt: one tap on its
 * Confirm control confirms (@confirm), a first setup or a re-setup alike; a tap anywhere else does nothing; the back
 * gesture (a swipe) ends it (@dismiss). */
bool g_sim_reconfirm;
static bool sim_prompt_up(void)
{
    return g_sim_reconfirm || muse_link_state() == MUSE_LINK_CONFIRM;
}
bool muse_link_confirm_key_press(void)
{
    if (!sim_prompt_up()) {
        return false;
    }
    if (!muse_link_prompt_armed()) {
        return true;   /* not on show yet: the press confirms nothing (muse_link.c) */
    }
    printf("@confirm\n");
    return true;
}
bool muse_link_confirm_dismiss(void)
{
    if (!sim_prompt_up()) {
        return false;
    }
    printf("@dismiss\n");
    return true;
}
/* The avatar face (main/mg_face.h): off unless a scenario loads a picture
 * (avatar_face=PATH, sim_stubs_face_load); then Settings > Watch face's
 * picker and Size work on the state here. The picture shows as
 * given, a panel-sized one as mg_face.c draws it at either Size (the
 * firmware's own decode, scaling and scrim: tests/test_mg_face.py renders
 * one); the Size row changes only the state here. */
static bool s_face_on, s_face_avatar = true, s_face_small;
static uint16_t *s_face_px;
static size_t s_face_n;

static void face_show(void)
{
    uint16_t *px = NULL;
    if (s_face_on && s_face_avatar && s_face_px) {
        px = heap_caps_malloc(s_face_n * sizeof(uint16_t), MALLOC_CAP_SPIRAM);   /* muse_ui takes it */
        if (px) memcpy(px, s_face_px, s_face_n * sizeof(uint16_t));
    }
    muse_ui_face_bg_set(px);
    /* The calling screen's plain picture: the same pixels here. */
    uint16_t *plain = NULL;
    if (px) {
        plain = heap_caps_malloc(s_face_n * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (plain) memcpy(plain, s_face_px, s_face_n * sizeof(uint16_t));
    }
    muse_ui_face_plain_set(plain);
}

bool sim_stubs_face_load(const char *path)
{
    free(s_face_px);
    s_face_px = NULL;
    s_face_on = false;
    if (strcmp(path, "none") != 0) {
        FILE *f = fopen(path, "rb");
        int w = 0, h = 0, max = 0;
        if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || fgetc(f) == EOF || w != muse_board->width ||
            h != muse_board->height || max != 255) {
            fprintf(stderr, "avatar_face: %s: not a %dx%d P6 picture\n", path, muse_board->width, muse_board->height);
            if (f) fclose(f);
            return false;
        }
        s_face_n = (size_t)w * h;
        s_face_px = malloc(s_face_n * sizeof(uint16_t));
        for (size_t i = 0; s_face_px && i < s_face_n; i++) {
            int r = fgetc(f), g = fgetc(f), b = fgetc(f);
            s_face_px[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
        }
        fclose(f);
        s_face_on = s_face_px != NULL;
    }
    face_show();
    return s_face_on || !strcmp(path, "none");
}

bool muse_link_face_available(void) { return s_face_on; }
void muse_link_face_label(char *out, size_t cap) { if (cap) snprintf(out, cap, "%s", s_face_on ? "My Muse" : ""); }
bool muse_link_face_avatar_picked(void) { return s_face_on && s_face_avatar; }
void muse_link_face_pick(bool avatar)
{
    s_face_avatar = avatar;
    printf("@face pick=%s\n", avatar ? "avatar" : "pixel");
    face_show();
}
bool muse_link_face_size_small(void) { return s_face_on && s_face_small; }
void muse_link_face_size_set(bool small)
{
    s_face_small = small;
    printf("@face size=%s\n", small ? "small" : "large");
}
bool muse_link_image_ask(char *why, size_t why_cap) { snprintf(why, why_cap, "not in the simulator"); return false; }
bool muse_link_image_status(char *out, size_t cap) { (void)out; (void)cap; return false; }
void muse_wifi_apply(void) {}
void muse_wifi_forget(const char *ssid) { (void)ssid; }
int muse_wifi_saved(muse_wifi_saved_t *out, int max) { (void)out; (void)max; return 0; }
esp_err_t muse_wifi_scan(void) { return ESP_OK; }
int muse_wifi_scan_results(muse_wifi_ap_t *out, int max, uint32_t *gen) { (void)out; (void)max; if (gen) *gen = 0; return 0; }
bool muse_wifi_scanning(void) { return false; }
void muse_ble_forget_all(void) {}

/* ---- Live call: the real muse_call.c over a client the scenario plays ---- */
void mg_ble_live_call(bool start) { (void)start; }
uint8_t mg_ble_live_call_failure_reason(void) { return 0; }
bool mg_ble_live_call_unproven(void) { return false; }
void mg_ble_live_call_held(bool held) { (void)held; }
bool mg_ble_live_call_client(void) { return s.call_available; }
void mg_lct_begin(void) {}
void mg_lct_begin_by(const char *by) { (void)by; }
void mg_lct_mark(mg_lct_point_t point) { (void)point; }
void mg_lct_end(void) {}
void mg_lct_end_why(const char *why) { (void)why; }
/* The subtitles' band is the production muse_subs.c (scenario subs=). */
/* Each wake and keep-lit (the idle timer's reset) prints, for the subtitles' rules. */
void muse_input_wake(const char *why) { printf("@wake %s\n", why); }
void muse_input_keep_lit(const char *why) { printf("@keep_lit %s\n", why); }
bool muse_input_charged_hold(void) { return false; }   /* no charger here: the passcode's usual idle rules */
void muse_input_press(muse_press_t src) { (void)src; }
bool muse_input_talk_ok(bool wireless) { (void)wireless; return true; }   /* the window's own finger */
void muse_ble_phone_setup(bool on) { muse_settings_set_ble_on(on); }
bool mg_ble_pair_auto_armed(void) { return false; }
bool mg_ble_pair_auto_take(void) { return false; }
void muse_f2_arm(int restart_s) { (void)restart_s; }
void muse_f2_print_result(void) {}
void muse_f2_boot(void) {}

/* Restart, the Wi-Fi proxy and the avatar sync: nothing behind them here. */
void muse_input_request_restart(void) {}
int muse_link_wifi_proxy_get(const char *ssid, network_proxy_config_t *out)
{
    (void)ssid;
    if (out) memset(out, 0, sizeof(*out));
    return 0;
}
bool muse_link_wifi_proxy_set(const char *ssid, const network_proxy_config_t *proxy) { (void)ssid; (void)proxy; return false; }
bool network_proxy_valid(const network_proxy_config_t *proxy) { return proxy && proxy->type == NETWORK_PROXY_OFF; }
void network_proxy_wipe(void *data, unsigned len) { memset(data, 0, len); }
void muse_avatar_refresh(void) {}
void muse_avatar_clear(void) {}
const char *muse_avatar_status(void) { return "not in the simulator"; }
