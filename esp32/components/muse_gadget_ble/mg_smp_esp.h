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

#include "mg_smp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The ESP-IDF half of firmware updates over BLE under the
 * MCUmgr SMP core (smp/): the backend (esp_ota_* into the
 * other OTA slot, test = boot it once PENDING_VERIFY, confirm = keep it) and
 * the platform hooks that aren't BLE (the record in NVS, the running image's
 * id). mg_ble.c adds the GATT transport, the gates and the reset.
 *
 * Revert is a property of the unit, not only of the chip: the bootloader
 * decides it, and the bootloader is never updated over the air. A test boot
 * that comes up PENDING_VERIFY proves this unit's bootloader has rollback;
 * one that comes up NEW (the app asked for a monitored first boot and the
 * bootloader ignored it) proves it hasn't. The verdict is kept in NVS beside
 * the bootloader's digest; until a test boot has proven it either way it's
 * "unproven" and the unit is taken to revert (every bootloader this tree
 * builds has CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). A unit without rollback
 * reports every commit as permanent.
 */

typedef enum {
    MG_SMP_RB_UNPROVEN = 0,   /* no test boot on this bootloader yet */
    MG_SMP_RB_PROVEN,         /* a test boot came up PENDING_VERIFY */
    MG_SMP_RB_ABSENT,         /* a test boot came up NEW: no rollback */
} mg_smp_rb_t;

/* Bytes of backend scratch the core allocates with its state (cfg.be_scratch):
 * the open OTA session and the image's first 352 bytes, held until its
 * header and board description say it's this board's, only while an update
 * runs (no idle memory). */
#define MG_SMP_ESP_SCRATCH 384

/* The two backends, const (flash): the same engine, with and without
 * MG_SMP_BE_CAN_REVERT. mg_smp_esp_probe() says which one this unit gets. */
extern const mg_smp_backend_t mg_smp_esp_backend_revert;
extern const mg_smp_backend_t mg_smp_esp_backend_permanent;

/* Probes the unit once at boot (bootloader digest, this boot's OTA state,
 * the kept verdict) and logs it; true = the unit can roll back (use
 * mg_smp_esp_backend_revert). Reads only: the verdict is written when a
 * test boot settles it (mg_smp_esp_settle). */
bool mg_smp_esp_probe(void);

/* Platform hooks for mg_smp_platform_t. */
int mg_smp_esp_record_get(void *ctx, uint8_t key, void *buf, uint16_t cap, uint16_t *len);
int mg_smp_esp_record_put(void *ctx, uint8_t key, const void *buf, uint16_t len);
void mg_smp_esp_running_id(void *ctx, mg_smp_image_id_t *out);

/* The running image was updated and waits for its confirm (PENDING_VERIFY). */
bool mg_smp_esp_pending_verify(void);
mg_smp_rb_t mg_smp_esp_rollback(void);
const char *mg_smp_esp_rollback_name(mg_smp_rb_t rb);

/* The boot is healthy: keep a rollback verdict this boot settled (the one
 * NVS write the probe defers; nothing writes flash before healthy). */
void mg_smp_esp_settle(void);

/* At the keep mark, after mg_smp_healthy (mg_ble.c): keeps what's ours to keep (a verdict whose write failed) and
 * answers true while something still needs keeping (a failed write, logged, or a pending image the confirm didn't
 * keep), so the caller tries again (mg_smp_esp_keep_due/tried/spent). */
bool mg_smp_esp_keep(void);

/* The keep's retries, paced in the caller's mark byte (pure, no state of their own): bits 0-1 are the caller's,
 * 2-4 count the tries, 5-7 hold the try's period (uptime / MG_SMP_ESP_KEEP_PERIOD, any unit the caller keeps).
 * due: a try now (the first, or a new period); tried: one more try made; spent: no more tries. */
#define MG_SMP_ESP_KEEP_TRIES 7u
#define MG_SMP_ESP_KEEP_PERIOD 5u   /* seconds, on the board */
bool mg_smp_esp_keep_due(uint8_t marks, uint32_t uptime);
uint8_t mg_smp_esp_keep_tried(uint8_t marks, uint32_t uptime);
bool mg_smp_esp_keep_spent(uint8_t marks);
/* The core gave its state back (mg_ble.c smp_mem_put): no update is open, so
 * the other slot's claim (mg_update_claim) is given back too. */
void mg_smp_esp_session_end(void);
bool mg_update_claim(bool take);

/* Two short lines about updates for the console and the boot log (the
 * Nordic UART mirror cuts long lines): the slots and their states; the
 * bootloader and its rollback verdict. */
int mg_smp_esp_status(char *out, size_t cap);
int mg_smp_esp_status_bootloader(char *out, size_t cap);

/* What a gadget's screen shows about an update. WRITING: *pct is
 * the share written (0-100); INSTALLING: checked and set to boot next;
 * STOPPED: aborted or refused before the commit (nothing installed: the
 * running image stays), or the link gone after it without the reset (the
 * image kept, pending its test boot: mg_smp_esp_ui_installed()), until the
 * screen has shown it and calls mg_smp_esp_ui_clear(). From any task; a few
 * bytes of state. */
typedef enum { MG_SMP_UI_NONE, MG_SMP_UI_WRITING, MG_SMP_UI_INSTALLING, MG_SMP_UI_STOPPED } mg_smp_ui_t;
mg_smp_ui_t mg_smp_esp_ui(uint8_t *pct);
void mg_smp_esp_ui_clear(void);
/* The screen drew st (the UI after its refresh); "Installing" not drawn yet
 * holds an update's restart up to 500 ms. */
void mg_smp_esp_ui_shown(mg_smp_ui_t st);
bool mg_smp_esp_installing_unshown(void);
/* This update's image is committed (set to boot next,
 * pending its test boot): its stop says so instead of "Nothing was
 * installed". Cleared by the next upload's start. */
bool mg_smp_esp_ui_installed(void);
/* The link went with no reset on its way: an update left at "Installing"
 * (its last byte in, maybe committed) stops there, its line by
 * mg_smp_esp_ui_installed(). The glue calls it at the link's end (where it
 * tells the core, mg_smp_disconnect), unless it's restarting for the update;
 * not where the core gives its state back, which it does right after a
 * commit too. */
void mg_smp_esp_ui_session_gone(void);

/* Pure helpers (host-tested). */
/* "1.2.3", "1.2.3+g0123abcd", "v1.2.3.dirty" -> major.minor.revision. */
void mg_smp_esp_parse_version(const char *s, mg_smp_version_t *out);
/* The 32-bit build id the packer stamps: the image's ELF SHA-256, its first
 * four bytes read little-endian. */
uint32_t mg_smp_esp_build_id(const uint8_t elf_sha256[32]);
/* The verdict from this boot's state and what NVS kept for this bootloader.
 * state: the running slot's esp_ota_img_states_t; kept_ok: NVS has a
 * verdict for this bootloader digest. */
mg_smp_rb_t mg_smp_esp_verdict(int state, bool kept_ok, mg_smp_rb_t kept);
/* "Update stopped"'s second line: "Nothing was installed"
 * for a stop before the commit (still on the old version), else the image's
 * state, "Installed; restart to finish" (committed, boots at the next
 * restart). */
const char *mg_smp_esp_stop_text(bool installed);

#ifdef __cplusplus
}
#endif
