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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_UI_THEME
#include "muse_theme_policy.h"
#endif
#if CONFIG_MUSE_AVATAR_SYNC
#include "muse_avatar_render.h"
#endif

/*
 * User settings, persisted in NVS. Setters save immediately and then notify the
 * change listener (from the caller's task), which applies the setting to the
 * hardware. Brightness, auto-sleep and the speaker are polled where they're used,
 * as is the reply text size, which notifies nothing.
 */

#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define MUSE_HOST_MAX 63
#define MUSE_VM_MAX 63
#define MUSE_TOKEN_MAX 1023

#define MUSE_MIC_GAIN_MAX 36      /* dB; ES7210 PGA, applied in 3 dB steps */
#define MUSE_MIC_GAIN_TUNED 30    /* dB; the default Settings gain, the boards' tuned capture gain */

typedef enum {
    MUSE_SETTING_VOLUME,
    MUSE_SETTING_SPEAKER,
    MUSE_SETTING_MIC_GAIN,
    MUSE_SETTING_BRIGHTNESS,
    MUSE_SETTING_SLEEP,
    MUSE_SETTING_WIFI,          /* on/off or credentials */
    MUSE_SETTING_BLE,
    MUSE_SETTING_HATCH,
    MUSE_SETTING_VOICE_NOTES,
#if CONFIG_MUSE_AVATAR_SYNC
    MUSE_SETTING_AVATAR,
#endif
#if CONFIG_MUSE_WAKEWORD_ENABLE
    MUSE_SETTING_WAKEWORD,      /* the wake word opt-in changed */
#endif
    MUSE_SETTING_TALK,
    MUSE_SETTING_HAPTICS,
    MUSE_SETTING_DEVELOPER,
} muse_setting_t;

/* Who changed the speaker (the gadget shows every change it didn't
 * make itself). A boot's restore
 * from NVS is no change at all. */
typedef enum {
    MUSE_VOL_BY_DEVICE = 0,   /* the gadget's own keys, sliders, a call's controls */
    MUSE_VOL_BY_PHONE,        /* an app's set_settings speaker_volume, Muse's phone setup */
    MUSE_VOL_BY_BENCH,        /* the console's vol= (RAM only) */
} muse_vol_by_t;

/* Which inputs talk (Settings > Talk): the screen's hold
 * region on the home, the talk button, or both (the default where a board
 * has both). Persisted. */
typedef enum {
    MUSE_TALK_BOTH = 0,
    MUSE_TALK_SCREEN = 1,
    MUSE_TALK_BUTTON = 2,
} muse_talk_mode_t;

typedef void (*muse_setting_cb_t)(muse_setting_t what);

esp_err_t muse_settings_init(void);
void muse_settings_set_listener(muse_setting_cb_t cb);
#if CONFIG_MUSE_AVATAR_SYNC
muse_avatar_source_t muse_settings_avatar_source(void);
/* Call from the backend's flash-safe worker, not a UI event handler. */
esp_err_t muse_settings_set_avatar_source(muse_avatar_source_t source);
#endif

#if CONFIG_MUSE_UI_THEME
muse_theme_mode_t muse_settings_theme(void);
esp_err_t muse_settings_set_theme(muse_theme_mode_t mode);
#endif
/* A second listener, called after the first (the BLE client's setting reports). */
void muse_settings_set_observer(muse_setting_cb_t cb);

int muse_settings_volume(void);         /* 0..100 */
bool muse_settings_speaker_on(void);    /* off: replies are shown, not played */
int muse_settings_mic_gain(void);       /* dB, 0..MUSE_MIC_GAIN_MAX */
int muse_settings_brightness(void);     /* 10..100 */
int muse_settings_sleep_s(void);        /* 0 = never */
#if CONFIG_MUSE_MOTION_GESTURES
bool muse_settings_lift_wake(void);
bool muse_settings_face_down_quiet(void);
void muse_settings_set_lift_wake(bool on);
void muse_settings_set_face_down_quiet(bool on);
#endif
/* Display > Sleep when charged (muse_charged.h): true =
 * Always (the default), the normal timeout; false = Never, a lit screen held
 * on while on USB with the battery charged. Saved on the gadget only. */
bool muse_settings_sleep_when_charged(void);
/* The size of reply text on screen, from Extra small to Extra large. Until
 * it's chosen, the screen's own default (muse_settings_set_text_size_default). */
enum { MUSE_TEXT_XSMALL, MUSE_TEXT_SMALL, MUSE_TEXT_MEDIUM, MUSE_TEXT_LARGE, MUSE_TEXT_XLARGE, MUSE_TEXT_SIZES };
int muse_settings_text_size(void);
/* The UI's size for a person who hasn't chosen one: what the screen showed
 * before the setting existed. Not saved. */
void muse_settings_set_text_size_default(int size);
bool muse_settings_wifi_on(void);
bool muse_settings_ble_on(void);
/* StopWatch: false selects realtime PTT; true selects voice notes only. */
bool muse_settings_voice_notes(void);
void muse_settings_set_voice_notes(bool on);
#if CONFIG_MUSE_WAKEWORD_ENABLE
bool muse_settings_wakeword_on(void);   /* hands-free listening opt-in; default off */
#endif
/* With CONFIG_MUSE_APP_VIEW, Apps in Settings (off until the person turns
 * them on): whether the gadget runs the apps their Muse makes. Changing it
 * restarts the gadget, so Muse learns of the app commands as it reconnects. */
bool muse_settings_apps_on(void);
void muse_settings_set_apps_on(bool on);
#if CONFIG_MUSE_PHOTO_NOTES
/* Photo with voice notes (muse_photo_note.h); default off. */
bool muse_settings_photo_notes(void);
void muse_settings_set_photo_notes(bool on);
#endif

void muse_settings_wifi(char ssid[MUSE_SSID_MAX + 1], char pass[MUSE_PASS_MAX + 1]);
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]);
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]);
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]);
size_t muse_settings_hatch_token_len(void);

void muse_settings_set_volume(int pct);
void muse_settings_set_speaker_on(bool on);
/* The same, made from outside the gadget (shown with "Set by phone"). */
void muse_settings_set_volume_by(int pct, muse_vol_by_t by);
void muse_settings_set_speaker_on_by(bool on, muse_vol_by_t by);
/* A count of the speaker's changes since boot (the level or the switch; none
 * at boot) and, in *by, who made the latest: the UI shows the outside ones
 * (muse_vol_show.h). Any task. */
uint32_t muse_settings_volume_changes(muse_vol_by_t *by);
/* The bench volume (bench and test mutes are RAM-only; the console's
 * vol=, muse_vol_word.h): the speaker at pct in RAM only (0: off, the level
 * kept), never written to NVS; the saved values are kept aside until
 * muse_settings_volume_restore() or the next user or app change (the two
 * setters above), which brings them back first and then saves its own.
 * Notifies MUSE_SETTING_VOLUME (once) like any change. */
void muse_settings_set_volume_ram(int pct);
void muse_settings_volume_restore(void);   /* vol=saved: back to the saved volume */
bool muse_settings_volume_is_ram(void);
/* The saved volume as speaker_volume reads it (0 when the saved speaker is off). */
int muse_settings_saved_speaker_volume(void);
void muse_settings_set_mic_gain(int db);
void muse_settings_set_brightness(int pct);
void muse_settings_set_sleep_s(int secs);
void muse_settings_set_sleep_when_charged(bool always);
muse_talk_mode_t muse_settings_talk_mode(void);
void muse_settings_set_talk_mode(muse_talk_mode_t mode);
/* The vibration motor's buzz at the talk threshold; a client's
 * haptics_enabled setting sets it too. Boards without a motor ignore it. */
bool muse_settings_haptics(void);
void muse_settings_set_haptics(bool on);
/* Developer mode: off by default;
 * five taps on About's card turn it on, the switch at the top of the
 * Developer page turns it off. Persisted; a factory reset clears it. Only
 * then does Settings show the Developer row. */
bool muse_settings_developer(void);
void muse_settings_set_developer(bool on);
/* A factory reset (Link's setup reset, before its restart): what a reset
 * clears of these settings (Developer mode). Works before muse_settings_init
 * too (SAFE mode never starts Muse). */
void muse_settings_factory_reset(void);
/* An unpair or reset: the Muse server, VM and device token set by hand back
 * to their defaults, in storage and here. False: storage didn't take the erase. */
bool muse_settings_forget_server(void);
void muse_settings_set_text_size(int size);
void muse_settings_set_wifi_on(bool on);
void muse_settings_set_ble_on(bool on);
#if CONFIG_MUSE_WAKEWORD_ENABLE
void muse_settings_set_wakeword_on(bool on);
#endif
/* A network name is remembered first among the saved ones and joined now;
 * an empty ssid forgets every saved network. */
void muse_settings_set_wifi(const char *ssid, const char *pass);
/* False, changing nothing, for a server this build doesn't allow
 * (muse_server_hosts.h); empty means the default. */
bool muse_settings_set_hatch_host(const char *host);
void muse_settings_set_hatch_vm(const char *vm);
/* append=true adds to the stored token (for chunked BLE writes). */
esp_err_t muse_settings_set_hatch_token(const char *token, bool append);
