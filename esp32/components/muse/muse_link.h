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
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"   /* the ops table's layout depends on it: the same in every file */
#endif

#include "muse_wifi.h"

typedef struct network_proxy_config network_proxy_config_t;

/*
 * Muse runs inside Home Link, which owns the radios, the setup credentials and
 * the Muse account. Link registers these operations at boot
 * (main/muse_glue.c); Muse's Wi-Fi status, scans, saved networks, BLE and Muse
 * VM lookups all go through them. Every call is safe from any task.
 */

typedef enum {
    MUSE_LINK_BOOT,
    MUSE_LINK_UNPAIRED,       /* advertising for the Muse app */
    MUSE_LINK_PAIRING,        /* the app is connected */
    MUSE_LINK_CONFIRM,        /* waiting for a button press to confirm pairing */
    MUSE_LINK_CONNECTING,     /* joining Wi-Fi / reaching the VM */
    MUSE_LINK_ONLINE,
    MUSE_LINK_OFFLINE,
    MUSE_LINK_ERROR,
} muse_link_state_t;

/* A reply frame of a request on Link's session: see noise_ctrl_req_cb in main/noise_control.h.
 * MUSE_LINK_REQ_TOO_LARGE in place of -1: a reply frame too big for this board's
 * buffers ended the session (NOISE_CTRL_REQ_TOO_LARGE). */
#define MUSE_LINK_REQ_TOO_LARGE (-2)
typedef void (*muse_link_req_cb)(void *ctx, int status, const uint8_t *data, size_t len, bool end);

typedef struct {
    void (*wifi_status)(muse_wifi_status_t *out);
    void (*wifi_apply)(void);                     /* on/off or credentials changed */
    void (*wifi_get)(char *ssid, char *pass);     /* MUSE_SSID_MAX+1 / MUSE_PASS_MAX+1; pass may be NULL */
    void (*wifi_set)(const char *ssid, const char *pass);
    esp_err_t (*wifi_scan)(void);
    bool (*wifi_scanning)(void);
    int (*wifi_scan_results)(muse_wifi_ap_t *out, int max, uint32_t *gen);
    void (*ble_apply)(void);                      /* BLE phone setup setting changed */
    bool (*ble_started)(void);
    bool (*hatch_linked)(void);                   /* paired to a Muse account */
    bool (*token_stored)(void);                   /* a device token is stored (set up), read fresh */
    const char *(*sdk_token)(void);               /* the maker's SDK token, or NULL */
    bool (*sdk_token_set)(const char *token);     /* stores it and restarts; false if invalid */
    bool (*sdk_token_changed)(void);              /* paired with another token: pair again to use this one */
    /* What may show of it: its length and the first 4 bytes of its SHA-256 as
     * hex (main/token_fp.h); false without a token. */
    bool (*sdk_token_fp)(unsigned *len, char hex[9]);
    /* Fills the VM whose id is want_vm (empty: Link's preferred VM). *vm_token is heap. */
    bool (*hatch_vm)(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                     char **vm_token);
    /* A press that began after pressed_after_us (esp_timer's clock). true: Link
     * used it (a pairing waits for a press, which confirms it if it began after
     * the prompt). talk_key: the talk key's press, which never confirms a
     * re-setup outside setup mode (it dismisses the prompt); else a confirm key
     * or the prompt's on-screen Confirm. */
    bool (*talk_press)(int64_t pressed_after_us, bool talk_key);
    /* Not now: the back gesture (a swipe, Escape) ends the pairing prompt, a
     * first setup's or a re-setup's (the phone can start it again). A tap
     * elsewhere on the screen does nothing. */
    void (*confirm_dismiss)(void);
    void (*reset_setup)(void);                    /* forget Link setup and reboot */
    /* Requests to the VM on Link's own Noise session, for boards without PSRAM for their own. */
    bool (*req_ready)(void);                      /* the session is up */
    int64_t (*req_open)(const char *verb, const char *path, const char *const *headers, bool end_body,
                        muse_link_req_cb cb, void *ctx);
    bool (*req_send)(int64_t id, const void *data, size_t len, bool end_body, int wait_ms);
    void (*req_cancel)(int64_t id);
#if CONFIG_MUSE_WASM_APPS
    size_t (*req_max_body)(void);                 /* the largest reply one frame can carry */
#endif
    void (*power_save)(bool on);                  /* screen off: the session polls less */
    void (*wifi_nap)(bool nap);                   /* screen off a while: leave Wi-Fi until false */
    void (*ble_quiet)(bool quiet);                /* a Wi-Fi video stream: no BLE advertising until false */
    int (*wifi_saved)(muse_wifi_saved_t *out, int max);   /* most recently joined first */
    int (*wifi_proxy_get)(const char *ssid, network_proxy_config_t *out);
    bool (*wifi_proxy_set)(const char *ssid, const network_proxy_config_t *proxy);
    void (*wifi_forget)(const char *ssid);        /* one saved network; empty forgets them all */
    /* Developer > Tests > Ask Muse for an image (main/session_image.h); NULL
     * where the gadget has no such test. From the LVGL task. */
    bool (*image_ask)(char *why, size_t why_cap); /* false with the reason */
    bool (*image_status)(char *out, size_t cap);  /* the test's line; true while a run is under way */
    /* The avatar face (main/mg_face.h): Settings > Watch face's face
     * picker and the avatar face's Size; NULL where the gadget has
     * none. From the LVGL task. */
    void (*face_label)(char *out, size_t cap);    /* the Muse's own name, "My Muse" until known */
    bool (*face_avatar_picked)(void);
    void (*face_pick)(bool avatar);               /* the avatar face, or Pixel Muse */
    bool (*face_size_small)(void);                /* the avatar face's Size: Small, or Large */
    void (*face_size_set)(bool small);
} muse_link_ops_t;

/* Pauses BLE advertising (setup's, or the companion's) while a Wi-Fi video
 * stream plays: the radio's scheduler gives an advertising BLE half of every
 * beacon period, and the stream's packets wait out that half. Connections
 * stay up. Calls nest; false releases one. */
void muse_link_ble_quiet(bool quiet);

void muse_link_register(const muse_link_ops_t *ops);

/* Link's status, mirrored from its LED state. */
void muse_link_set_state(muse_link_state_t state);
muse_link_state_t muse_link_state(void);
const char *muse_link_state_name(muse_link_state_t state);

bool muse_link_hatch_linked(void);
/* A device token is stored: the gadget has been set up with the Muse app
 * (cached, refreshed at most every 2 s; else the home says "Not set up"). */
bool muse_link_token_stored(void);
const char *muse_link_sdk_token(void);
bool muse_link_sdk_token_set(const char *token);
bool muse_link_sdk_token_changed(void);
/* Whether there's an SDK token, and what a screen may show of it: its length
 * and the start of its SHA-256 as 8 hex digits ("" if unknown). Never any of
 * its characters: screenshots end up in transcripts. */
#define MUSE_LINK_FP_HEX 9
bool muse_link_sdk_token_fp(unsigned *len, char hex[MUSE_LINK_FP_HEX]);
bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name, size_t name_cap,
                        char **vm_token);
/* A talk-key press, now; true: Link used it. On a Muse Gadget one press
 * confirms a first setup, whenever it began; on a set-up gadget the talk key
 * never confirms a re-setup outside setup mode: its press dismisses the
 * prompt (no talk turn). Home Link counts only one that began after its
 * prompt. released_us is when the key was last let go (esp_timer's clock), or
 * 0 if the caller doesn't track it; muse_link_talk_press() passes 0. */
bool muse_link_confirm_press(int64_t released_us);
bool muse_link_talk_press(void);
/* A confirm key's press (Enter, a second key) or a tap on the prompt's
 * Confirm disc, now: confirms a first setup or a re-setup. */
bool muse_link_confirm_key_press(void);
/* Not now (muse_link_ops_t.confirm_dismiss): the back gesture; true if a
 * pairing prompt was up and is ended. */
bool muse_link_confirm_dismiss(void);
/* The bench's confirmation (>pair.confirm, the auto-confirm): a press, as
 * the talk key's. */
bool muse_link_bench_confirm(void);
void muse_link_reset_setup(void);
/* The first saved network, owned by Link. Return false when Link hasn't registered. */
bool muse_link_wifi_get(char *ssid, char *pass);
bool muse_link_wifi_set(const char *ssid, const char *pass);
bool muse_link_ble_started(void);
/* Requests on Link's session (noise_ctrl_req_* in main/noise_control.h); open returns 0 if down. */
bool muse_link_req_ready(void);
/* The avatar face's picker and Size (muse_link_ops_t's face_*): false where there's none. */
bool muse_link_face_available(void);
void muse_link_face_label(char *out, size_t cap);
bool muse_link_face_avatar_picked(void);
void muse_link_face_pick(bool avatar);
bool muse_link_face_size_small(void);
void muse_link_face_size_set(bool small);
/* The Ask Muse for an image test (muse_link_ops_t's image_ask, image_status). */
bool muse_link_image_test(void);
bool muse_link_image_ask(char *why, size_t why_cap);
bool muse_link_image_status(char *out, size_t cap);
int64_t muse_link_req_open(const char *verb, const char *path, const char *const *headers, bool end_body,
                           muse_link_req_cb cb, void *ctx);
bool muse_link_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms);
void muse_link_req_cancel(int64_t id);
#if CONFIG_MUSE_WASM_APPS
/* The largest reply body one frame of Link's session can carry; bound a reply that
 * could be larger, e.g. with Range (noise_ctrl_req_max_body). */
size_t muse_link_req_max_body(void);
#endif
void muse_link_ble_apply(void);

/* Proxy state: 0 saved, 1 saving, -1 unknown network, -2 last save failed.
 * Set queues a write on Link's internal-stack keeper; get polls its result. */
int muse_link_wifi_proxy_get(const char *ssid, network_proxy_config_t *out);
bool muse_link_wifi_proxy_set(const char *ssid, const network_proxy_config_t *proxy);
