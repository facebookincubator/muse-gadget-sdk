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


#ifndef MG_OTA_H_
#define MG_OTA_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Firmware updates (CONFIG_MG_DFU): MCUmgr SMP over BLE with MCUboot
 * (swap using move). SMP is the BLE recovery tier: reads are always open,
 * and update writes need this connection's token proof on a set-up gadget;
 * in SAFE mode only SAFE asked with the button takes them (mg_ota_policy.h);
 * a test image keeps itself once the boot is healthy, and a client's earlier
 * confirm takes effect then (mg_ota_policy.h). MCUboot's secondary slot is
 * shared with the offline clip queue (mg_queue.h).
 */

/* After mg_queue_init(): decides who holds the slot, registers MCUmgr hooks. */
void mg_ota_init(void);

/* The boot is healthy (mg_health.c), in normal or SAFE mode: in normal mode
 * a test image is kept once a link has also proved ATT works
 * (mg_ota_link_proven()), then the clip store comes back. */
void mg_ota_healthy(void);
/* A link proved the gadget's own ATT path (mg_ble.c, the reachability
 * check's completed exchange): with the healthy mark, an updated image keeps
 * itself. */
void mg_ota_link_proven(void);

/* mg_smp_mem.c: memory blocks SMP holds right now (0 when idle: update
 * support takes no base memory). */
int mg_smp_mem_blocks(void);

/* For the bench status line. */
bool mg_ota_confirmed(void);
bool mg_ota_upload_active(void);
/* SMP update writes refused for want of the token proof. */
uint32_t mg_ota_smp_refused(void);

/* Admit this connection's next transfer (mg_ota_policy.h): a confirm press,
 * or a scripted one (the wired console, a bench-mode image's word). */
void mg_ota_window_open(uint32_t conn_gen);
/* A press while the update prompt is up on this connection: true if it
 * answered it (then it's no push-to-talk). */
bool mg_ota_prompt_take(bool real, bool injected_counts);
/* The link ended: its update prompt (and light) ends too. */
void mg_ota_link_ended(void);
/* An update chunk moved, or a press admitted an update, on this connection
 * (never the prompt alone): its link counts as reachable
 * (mg_reach_link_counts(), mg_reach_update_live()). */
bool mg_ota_update_live(void);
/* "idle", "press" (the prompt is up) or "admitted", for this connection. */
const char *mg_ota_prompt_state(void);

/* A setup committed (not a token refresh): an image staged before it is
 * dropped, and a transfer started before it carries nothing over (mg_ota.c). */
void mg_ota_setup_committed(void);

#endif
