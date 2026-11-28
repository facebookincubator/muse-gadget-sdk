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

#ifndef MG_APP_H_
#define MG_APP_H_

#include <stddef.h>
#include <stdint.h>

const char *mg_app_version(void);
uint8_t mg_app_battery_percent(void);
/* The last battery reading in mV, or <0 without one. */
int mg_app_battery_mv(void);
/* USB power plugged in: 1, 0, or -ENOTSUP where the board can't tell. */
int mg_app_usb_present(void);

/* mg_poweroff.c (CONFIG_MG_POWER_OFF): drops the link, darkens the LED and
 * enters System OFF; the button wakes the gadget (a reset). Returns at once;
 * the rest runs on the app work queue. */
void mg_app_power_off(const char *why);
/* Before Bluetooth: stays off after an unplug that followed a power
 * off on USB, or with a low cell off USB; charges dark on USB with a low
 * cell until it reaches CONFIG_MG_BATTERY_RESUME_MV (or a key press).
 * Returns when the gadget should start. */
void mg_power_boot_gate(void);

/* mg_bench.c (CONFIG_MG_BENCH) */
void mg_bench_init(void);

/* mg_ble.c */
int mg_ble_init(void);
int mg_ble_start_advertising(void);
/* Re-reads what to advertise (Link setup or not) and applies it. */
void mg_ble_refresh_advertising(void);
/* Before a reset the gadget asked for itself (an update): stops advertising
 * for good and drops the link cleanly, so the phone sees the disconnect at
 * once and doesn't reconnect to the image that is about to restart. */
void mg_ble_hold_for_reset(void);
/* The reachability check's bench test (mg_reach_policy.h): connectable
 * advertising off until the next boot and the link dropped (200 ms from now,
 * so a console's reply goes first), RAM only, so the check restarts the
 * gadget 30 s later as a failed boot. -EPERM in a release build
 * (CONFIG_MG_AUTOMATION off: a bench word), -ENOTSUP in a build without that
 * restart (CONFIG_MG_REACH_RESET), where it would leave the gadget
 * unreachable. */
int mg_ble_reach_test(void);
/* The TX path's bench test: the link's free controller credits are taken and
 * held, so nothing more can be sent on it; the gadget restarts as a peer
 * loss about 10 s later (RAM only). -EPERM in a release build
 * (CONFIG_MG_AUTOMATION off: a bench word), -ENOTSUP without the watchdog,
 * -ENOTCONN without a link. */
int mg_ble_tx_dead_test(void);
/* The bench test for ATT-dead links: every link until the next boot counts
 * as ATT-dead and drops after 6 s (RAM only); three in a row restart the
 * gadget as a peer loss. -EPERM in a release build (CONFIG_MG_AUTOMATION off: a
 * bench word), -ENOTSUP without CONFIG_MG_REACH_RESET. */
int mg_ble_att_dead_test(void);
void mg_ble_set_battery(uint8_t percent);

/* mg_nus.c */
void mg_nus_init(void);
/* A board's own Nordic UART console words (weak, none by default), for words
 * the console doesn't know: true when handled, its reply in out. Runs on the
 * app work queue; never waits. */
bool mg_board_nus_word(const char *cmd, char *out, size_t len);
/* A board's extra conditions for the setup reset gesture (weak,
 * true by default; e.g. only with the charger present). */
bool mg_board_reset_hold_allowed(void);
/* Whether the boot hold passes a SAFE point first (weak, false by default;
 * e.g. only with the charger present; recovery tier 3 by button). */
bool mg_board_safe_hold_allowed(void);
/* Board hooks (weak, nothing by default): the boot got healthy
 * (mg_health.c); the setup state at boot and at every change (complete:
 * set up; mg_setup_store.c). */
void mg_board_healthy(void);
void mg_board_setup_state(bool complete);
/* CONFIG_MG_SDK_TOKEN_BOARD: the owner's SDK token (NUL-terminated), or
 * NULL for none. */
const char *mg_board_sdk_token(void);
/* The reset cause (RESET_* flags) when the board knows better than hwinfo
 * (a loader that resets into the app): hw by default (weak, mg_health.c). */
uint32_t mg_board_reset_cause(uint32_t hw);
/* A press of the talk button before anything else sees it (weak, false by
 * default): true if the board's own prompt took it (a board whose update
 * goes through its own loader, with its own one-press confirm), and then
 * it's never push-to-talk and its release is swallowed. real: the button's
 * GPIO; injected_counts: this build takes an injected press. In SAFE too. */
bool mg_board_press_take(bool real, bool injected_counts);
/* The board's own update path is live on this connection (weak, false): its
 * transfer moving, or a press admitted it, never its prompt alone
 * (mg_reach_update_live()); the link then counts as reachable
 * (mg_reach_link_counts()), as mg_ota_update_live() does. */
bool mg_board_update_live(void);
/* Each time the app feeds the watchdog (weak, nothing by default): a board
 * whose loader left more watchdog channels or instances running feeds them
 * here, so they starve exactly when the app's own watchdog does. */
void mg_board_wdt_fed(void);
/* CONFIG_MG_SMP_GATE_BOARD: whether an SMP write (MCUmgr group and command
 * id) may run, asked after the token-proof gate (mg_smp_gate.c). No default:
 * a board that picks that option must define it. */
bool mg_board_smp_write_allowed(uint16_t group, uint8_t id);
/* CONFIG_MG_SMP_GATE_BOARD: whether the token-proof gate leaves this SMP
 * request (group, command id, op) to the board without the proof (weak,
 * false by default; mg_smp_gate.c): true for a request the board checks
 * itself. Writes still meet mg_board_smp_write_allowed(). */
bool mg_board_smp_gate_defers(uint16_t group, uint8_t id, uint8_t op);
/* Right before a restart a peer could cause (mg_health_peer_loss(); a soft
 * reset): e.g. a loader that counts boot attempts skips it (weak, nothing by
 * default). */
void mg_board_peer_restart(void);
/* Console output the NUS mirror dropped since boot, in bytes (its ring was
 * full; it never blocks the logging thread). */
unsigned int mg_nus_dropped(void);

#endif
