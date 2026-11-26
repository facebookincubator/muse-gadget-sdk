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
#include <stdint.h>

#include "sdkconfig.h"
#include "network_proxy.h"

typedef void (*ble_provision_cb)(const char *ssid, const char *password,
                                 const char *access_token,
                                 const char *refresh_token,
                                 const char *username,
                                 const char *ota_url,
                                 bool ota_force,
                                 const char *api_url,
                                 const char *api_url_v2,
                                 const char *noise_host,
                                 const network_proxy_config_t *proxy,
                                 uint32_t session_generation);
typedef void (*ble_ota_cb)(const char *url, bool force);
typedef void (*ble_simple_cb)(void);

typedef struct {
    ble_provision_cb on_provision;
    ble_simple_cb    on_wifi_scan;
    ble_ota_cb       on_ota;
    ble_simple_cb    on_unpair;
    ble_simple_cb    on_get_device_info;
    ble_simple_cb    on_client_connected;
    ble_simple_cb    on_client_disconnected;
    void (*on_pairing_client_finished)(uint32_t session_generation);
} ble_callbacks_t;

struct ble_gatt_svc_def;
struct ble_gap_event;

// Another GATT service sharing this server (Muse's phone setup, musegadgets BLE
// audio). Add them before ble_server_start(), up to two: configure_host runs
// before the host starts, and on_gap_event sees every GAP event; the first
// non-zero return is the event's result. A companion with adv_uuid128 is
// advertised with that UUID whenever wants_advertising() says so, taking
// turns with Link's own advertising while both are wanted; call
// ble_server_refresh_advertising() when its answer changes.
typedef struct {
    const struct ble_gatt_svc_def *svcs;
    void (*configure_host)(void);
    int (*on_gap_event)(struct ble_gap_event *event);
    const uint8_t *adv_uuid128;        // little-endian, or NULL
    bool (*wants_advertising)(void);
    // Advertised with adv_uuid128 when they return non-zero (either may be
    // NULL): a 16-bit service UUID (the HID service, so phones list the device
    // as an input device) and the GAP appearance. Read at each advertising
    // start; call ble_server_refresh_advertising() when they change.
    uint16_t (*adv_uuid16)(void);
    uint16_t (*adv_appearance)(void);
    // True while the companion's protocol is stalled (its task missed
    // its heartbeat). Checked every second on the host task: the normal
    // identity stops advertising until it beats again (the SAFE identity
    // keeps advertising). May be NULL.
    bool (*stalled)(void);
} ble_companion_t;

// Adds a companion (NULL clears them).
void ble_server_set_companion(const ble_companion_t *companion);
void ble_server_refresh_advertising(void);
// True when SAFE runs on the public address (both SAFE random
// addresses refused): the Muse services refuse every access.
bool ble_server_muse_gatt_refused(void);
// Keep advertising for the companion service even after setup is complete.
void ble_server_set_companion_advertising(bool enabled);
bool ble_server_is_started(void);
// Host and controller synced (ready for GAP and GATT calls).
bool ble_server_is_synced(void);
bool ble_server_is_advertising(void);
// The public address, little-endian. False while BLE is down.
bool ble_server_own_address(uint8_t addr[6]);
// Stop the stack and free its heap but keep the controller's static memory,
// so ble_server_start() can bring it back (ble_server_full_shutdown()
// releases that too, for good).
bool ble_server_power_off(void);
#if CONFIG_HOMEHUB_BLE_PERSISTENT
// Advertising interval after setup (20-10240 ms); applies at once.
void ble_server_set_companion_interval_ms(int interval_ms);
int ble_server_companion_interval_ms(void);
#endif

void ble_server_start(const char *device_name, const ble_callbacks_t *cb);
void ble_server_begin_advertising(void);
void ble_server_stop_advertising(bool disconnect_client);
// Quiet: no advertising at all (a Wi-Fi video stream wants the shared radio)
// until called with false, which resumes whatever advertising was on.
// Connections are left alone.
void ble_server_set_quiet(bool quiet);

// Disconnect the current GATT client without changing advertising policy.
void ble_server_disconnect_client(void);
bool ble_server_has_connection(void);
/* A re-setup prompt showed on this connection (app.c): another connection's pairing_client_hello gets
 * error_operation_in_progress while it is up; this connection's takes it over (resetup_gap.h). */
void ble_server_resetup_prompt_shown(void);
/* The prompt ended (app.c): dismissed (the back gesture, a talk press on a re-setup prompt) refuses a new prompt on
 * this connection for 10 s on a release build; a confirm or the phone giving up doesn't. */
void ble_server_resetup_prompt_ended(bool dismissed);
/* Advertised (connectable) or had a connection at least once this boot: the
 * boot guard counts a boot healthy only then. */
bool ble_server_ever_reachable(void);
/* Connectable advertising or a link now, or BLE off or paused on purpose
 * (a running gadget must stay reachable). */
bool ble_server_reachable_now(void);
/* Bench (the BLE console's !SILENCE): stops advertising and
 * closes the link until a restart, as a stack that stopped advertising
 * would, to prove the stay-reachable check counts on hardware. */
void ble_server_bench_silence(void);
// Bench (the musegadgets console's !SETUP ADV): a set-up gadget
// advertises Link setup again for secs (0 ends it), taking turns with its
// companion and flagged as not set up, so an app that lists only Link setup
// adverts can set it up again. RAM only: a restart ends it.
void ble_server_bench_setup_adv(uint32_t secs);
// Bench (>ble.pause=SECS[,ADDR], the musegadgets console's !IN
// ble.pause=...): the current link closed, then for secs either no
// advertising at all (only_addr NULL) or advertising connectable only from
// only_addr (6 bytes, most significant first; public or random static, either
// is accepted: NimBLE's accept list with the "connect only from the list"
// filter policy), then the normal policy and advertising as before (a link
// the address holds when secs end stays; the normal policy is back at its
// disconnect). A gadget-side "off the phone" for A/B tests, and a way to hand
// a gadget to one central when a phone's app can't let it go: no unpair, no
// NVS write; it counts as reachable on purpose meanwhile. RAM only (a
// restart ends it). A new pause replaces one under way.
void ble_server_bench_pause(uint32_t secs, const uint8_t *only_addr);
// Advertise fast (30-50 ms) for 30 s from now, then Apple's 417.5 ms
// again (adv_pace.h); boot and a disconnect trigger it here, a talk press
// with no app connected through this (why: for the log, "adv: fast 30 s
// (why)"). Any task.
void ble_server_adv_fast(const char *why);
/* SAFE mode can't serve Muse: advertise under another identity (a
 * static random address, "SAFE-<product>-<tail>", no service UUIDs).
 * Before ble_server_start(). */
void ble_server_set_safe_identity(bool safe);
/* Bench (the console's !SAFEID): the next boot advertises the
 * SAFE identity in normal mode and restarts back after 120 s by itself. */
void ble_server_bench_safe_identity(void);
/* At boot, before ble_server_start(): true (once) after !SAFEID. */
bool ble_server_bench_safe_identity_take(void);

// Send a short status string as a single notification.
void ble_server_send_status(const char *status);
// Deferred pairing work must use these token-bound operations. Stale work is
// discarded, and disconnect executes on the BLE host queue.
bool ble_server_send_pairing_status(const char *status, uint32_t generation);
void ble_server_disconnect_pairing_session(uint32_t generation);

// Send a large payload using framed chunked notifications.
//   byte 0: 0xFE magic
//   byte 1: chunk index (0-based)
//   byte 2: total chunk count
//   bytes 3..: payload fragment
// Max 160 bytes per packet, 50ms stagger between chunks.
// Safe to call from worker tasks; serialized internally.
void ble_server_send_chunked(const char *data);
// Allocate the encrypted record counter and send it in the same TX order.
bool ble_server_send_encrypted_json(const char *json, uint32_t generation);

// Disconnect the BLE client after a delay (ms), giving time for any
// pending status notification to be sent before the link drops.
void ble_server_delayed_disconnect(uint32_t ms);

// Diagnostic: fully shut down the BLE controller so it stops contending
// for the shared radio. After this call, BLE cannot be brought back up
// without a reboot.
void ble_server_full_shutdown(void);

// Forgets every bonded phone: the stack's store when it's up, else where
// NimBLE keeps the bonds (CONFIG_BT_NIMBLE_NVS_PERSIST), so they don't come
// back when it starts. Every unpair and reset calls it.
void ble_server_forget_bonds(void);
// Forgets every bonded phone but the one connected now (by its identity
// address): an owner change's new phone keeps its bond. False: the stack
// isn't up, no phone is connected, or a bond couldn't be erased; the caller
// forgets them all (ble_server_forget_bonds).
bool ble_server_forget_bonds_but_connected(void);
