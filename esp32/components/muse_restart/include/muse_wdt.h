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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The gadget watchdog (CONFIG_MUSE_GADGET_WDT): the
 * task watchdog's users for the gadget's threads, each fed from its own
 * thread's loop. A thread that stops for longer than
 * CONFIG_ESP_TASK_WDT_TIMEOUT_S panics the chip (a crash record, then a
 * restart the boot guard counts). The NimBLE host is fed by a NimBLE callout
 * that runs in the host task (mg_ble.c); the musegadgets worker from its loop
 * (at least once a second); the Muse input task, which also takes the
 * display's lock and so covers a stuck UI, from its loop (at least every
 * 10 s, resting); Muse's voice task from its loop, its mic reads, speaker
 * writes and delays (resting, at least every 10 s), so a read that never
 * returns panics with its backtrace instead of leaving the mic dead for
 * hours (seen on the W175 and Watcher); on a Wi-Fi build the Link session task from its session
 * loop (its connect, handshake and reconnect waits, each with a timeout of
 * its own, aren't watched). The idle tasks are watched too (the BLE gadget
 * profiles: 15 s, a panic), so a task that hogs a core is caught as well;
 * every user here is fed at least every 10 s. Without the option (or the task
 * watchdog off, as on other boards) the calls do nothing.
 */
typedef enum {
    MUSE_WDT_BLE_HOST = 0,
    MUSE_WDT_MG_WORKER,
    MUSE_WDT_MUSE_INPUT,
    MUSE_WDT_LINK_SESSION,   /* Wi-Fi builds: the Link session's loop (noise_control.cpp) */
    MUSE_WDT_MUSE_VOICE,     /* Muse's voice task (muse_voice.c): fed at each mic read, speaker write,
                              * delay and place in its loop, from that task only; its rest waits <= 10 s */
    MUSE_WDT_COUNT,
} muse_wdt_user_t;

/* From the thread's loop: the first feed adds the user, later ones reset it. */
void muse_wdt_add(muse_wdt_user_t u);
void muse_wdt_feed(muse_wdt_user_t u);
/* The thread ends on purpose (the BLE stack shut down): stop watching it. */
void muse_wdt_remove(muse_wdt_user_t u);

#ifdef __cplusplus
}
#endif
