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
 * The ESP-IDF backend of the shared SMP core (mg_smp_esp.h). The core checks
 * the envelope's SHA-256 and the upload's whole-file sha before it calls
 * finish(); finish() is esp_ota_end(), which checks the app image and, with
 * CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, its RSA signature; commit()
 * sets the boot partition (the image boots once PENDING_VERIFY); confirm() is
 * esp_ota_mark_app_valid_cancel_rollback(). Before any of that, write() holds
 * the app's first MUSE_IMAGE_HEAD_BYTES and writes nothing until they say the
 * image is this board's (muse_image_check: the image header's chip id and the
 * board description's product code and board profile); another chip's or
 * board's image is refused (EINVAL) with the slot untouched. The core keeps the active-hash
 * record through record_get/put (one NVS blob) and matches it on the version
 * and the build id (the image's ELF SHA-256, first four bytes).
 */

#include "mg_smp_esp.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_bootloader_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "muse_image_id.h"
#include "nvs.h"
#include "sdkconfig.h"

_Static_assert(ESP_OTA_IMG_NEW == 0x0 && ESP_OTA_IMG_PENDING_VERIFY == 0x1,
               "mg_smp_esp_util.c's copy of esp_ota_img_states_t");

static const char *TAG = "mg.smp";

#define NVS_NS "mg_smp"
#define RB_KEY "rb"
#define RB_MAGIC 0x4D53524Cu   /* "MSRL" */

typedef struct {
    uint32_t magic;
    uint8_t digest[32];   /* the bootloader image's appended SHA-256 */
    uint8_t verdict;      /* mg_smp_rb_t */
} rb_rec_t;

/* The open OTA session lives in the core's backend scratch (cfg.be_scratch,
 * no idle memory): it exists from begin() until abort() or commit() returns, and
 * nothing of it is held between updates. */
typedef struct {
    const esp_partition_t *part;   /* the slot being written */
    esp_ota_handle_t ota;
    uint32_t written;              /* payload bytes taken, the held head included */
    uint32_t total;                /* the payload's size (begin), for the screen's percent */
    bool open;
    bool flashed;                  /* esp_ota_write ran: the slot no longer holds what it did */
    uint16_t head_n;
    uint8_t head[MUSE_IMAGE_HEAD_BYTES];   /* the app's first bytes, until they're checked */
} esp_sess_t;

/* The screen's update state and percent (mg_smp_esp_ui), a few bytes:
 * the UI task can't read the core's scratch (valid only inside its calls).
 * s_ui_shown: the screen drew s_ui's state; s_ui_committed: this update's
 * image is set to boot next (its stop says so). */
static volatile uint8_t s_ui, s_ui_pct, s_ui_shown, s_ui_committed;
_Static_assert(sizeof(esp_sess_t) <= MG_SMP_ESP_SCRATCH, "MG_SMP_ESP_SCRATCH too small");

static esp_sess_t *sess(void)
{
    return mg_smp_scratch();
}

/* This unit's rollback verdict (needed all boot) and whether it still has to
 * be kept, in one byte. */
#define RB_DIRTY 0x80u   /* settled this boot, not yet kept */
static uint8_t s_rb_state;
#define s_rb ((mg_smp_rb_t)(s_rb_state & 0x7Fu))

/* ---- the unit: bootloader and rollback --------------------------------------- */

/* The bootloader's appended digest, read when needed (no static copy: no idle memory). */
static bool bootloader_digest(uint8_t out[32])
{
    const esp_partition_pos_t pos = {
        .offset = CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
        .size = CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
    };
    esp_image_metadata_t md;
    memset(&md, 0, sizeof(md));
    if (esp_image_get_metadata(&pos, &md) != ESP_OK || !md.image.hash_appended) {
        return false;
    }
    memcpy(out, md.image_digest, 32);
    return true;
}

static bool rb_load(rb_rec_t *r)
{
    nvs_handle_t h = 0;
    size_t n = sizeof(*r);
    bool ok = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    ok = ok && nvs_get_blob(h, RB_KEY, r, &n) == ESP_OK && n == sizeof(*r) && r->magic == RB_MAGIC;
    if (h) {
        nvs_close(h);
    }
    return ok;
}

static int running_state(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK ? (int)st : -1;
}

/* Keeps a verdict this boot settled (called once the boot is healthy: no
 * flash writes before that). False: it's still to be kept (the write failed,
 * logged; the mark's next round tries again). */
static bool rb_keep(void)
{
    rb_rec_t r = { .magic = RB_MAGIC, .verdict = (uint8_t)s_rb };
    if (!(s_rb_state & RB_DIRTY) || !bootloader_digest(r.digest)) {
        return true;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, RB_KEY, &r, sizeof(r));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rollback verdict not kept (%s): again at the next round", esp_err_to_name(err));
        return false;
    }
    s_rb_state &= (uint8_t)~RB_DIRTY;   /* only once it's written */
    return true;
}

static const esp_partition_t *other_slot(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    return next && next != run ? next : NULL;
}

/* ---- backend ---------------------------------------------------------------- */

/* mg_ble.c: an update starting cuts a client's pattern. Weak, so
 * a backend without the BLE glue (the host tests) builds without it. */
__attribute__((weak)) void mg_haptic_interrupt(uint8_t reason)
{
    (void)reason;
}

/* main/mg_glue.c (ota.h ota_claim): one update at a time with the Wi-Fi
 * OTA, which writes the same slot. take: claim it for this update (false:
 * a Wi-Fi update has it, refuse as busy); !take: give it back. Weak: always
 * free where nothing else updates (the host tests). */
__attribute__((weak)) bool mg_update_claim(bool take)
{
    (void)take;
    return true;
}

void mg_smp_esp_session_end(void)
{
    mg_update_claim(false);
}

static int be_begin(void *ctx, uint32_t payload_len, uint32_t *resume_off)
{
    (void)ctx;
    (void)resume_off;   /* RAM resume only: the core keeps the offset */
    mg_haptic_interrupt(2 /* mg_haptic_reason_updating */);   /* no patterns while an update runs */
    esp_sess_t *ss = sess();
    if (!ss) {
        return MG_SMP_ENOMEM;
    }
    if (ss->open) {
        esp_ota_abort(ss->ota);
        ss->open = false;
    }
    ss->part = other_slot();
    if (!ss->part) {
        ESP_LOGW(TAG, "no second OTA slot: updates over BLE aren't possible on this layout");
        return MG_SMP_ENOTSUP;
    }
    if (payload_len > ss->part->size) {
        return MG_SMP_ENOMEM;
    }
    if (payload_len < MUSE_IMAGE_HEAD_BYTES) {
        ESP_LOGW(TAG, "update refused: %u bytes is too short for an app image", (unsigned)payload_len);
        return MG_SMP_EINVAL;
    }
    if (mg_smp_esp_pending_verify()) {
        /* Writing the other slot would erase the only image to go back to. */
        ESP_LOGW(TAG, "update refused: the running image waits for its confirm");
        return MG_SMP_EBADSTATE;
    }
    if (!mg_update_claim(true)) {
        ESP_LOGW(TAG, "update refused: a Wi-Fi update is running");
        return MG_SMP_EBUSY;
    }
    /* Erase as it writes: no seconds-long erase up front (the first
     * chunk is answered at once). ESP-IDF v6.0.1's esp_ota_begin erases
     * nothing with OTA_WITH_SEQUENTIAL_WRITES: the slot is untouched until
     * the first esp_ota_write, which waits for the head check (write()). */
    esp_err_t err = esp_ota_begin(ss->part, OTA_WITH_SEQUENTIAL_WRITES, &ss->ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        mg_update_claim(false);
        return err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? MG_SMP_EBADSTATE : MG_SMP_EUNKNOWN;
    }
    ss->open = true;
    ss->flashed = false;
    ss->head_n = 0;
    ss->written = 0;
    ss->total = payload_len;
    s_ui_pct = 0;
    s_ui_committed = 0;
    s_ui = MG_SMP_UI_WRITING;
    ESP_LOGI(TAG, "update: %u bytes into %s", (unsigned)payload_len, ss->part->label);
    return MG_SMP_EOK;
}

/* The image's first bytes are in: is it this board's? (Before any write: a
 * wrong image leaves the other slot as it was.) */
static int check_head(const esp_sess_t *ss)
{
    muse_image_info_t info;
    muse_image_verdict_t v = muse_image_check(ss->head, ss->head_n, true, &info);
    if (v == MUSE_IMAGE_OK) {
        ESP_LOGI(TAG, "update is this board's (%s %s, version %s)", info.product_code, info.board_id, info.version);
        return MG_SMP_EOK;
    }
    char who[112];
    muse_image_describe(&info, who, sizeof(who));
    ESP_LOGW(TAG, "update refused before writing: %s (offered %s; this is chip %u, %s %s)",
             muse_image_verdict_text(v), who, (unsigned)muse_board_desc.chip_id, muse_board_desc.product_code,
             muse_board_desc.board_id);
    return MG_SMP_EINVAL;   /* the core's "wrong board in the payload" */
}

static int ota_write(esp_sess_t *ss, const uint8_t *buf, uint32_t n)
{
    esp_err_t err = esp_ota_write(ss->ota, buf, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? MG_SMP_ECORRUPT : MG_SMP_EUNKNOWN;
    }
    ss->flashed = true;
    return MG_SMP_EOK;
}

static int be_write(void *ctx, uint32_t off, const uint8_t *buf, uint32_t n)
{
    (void)ctx;
    esp_sess_t *ss = sess();
    if (!ss || !ss->open || off != ss->written) {
        return MG_SMP_EBADSTATE;   /* the core writes in order; anything else is a bug */
    }
    int rc = MG_SMP_EOK;
    if (!ss->flashed) {
        /* Held, not written, until the header and the board description are in. */
        uint32_t k = MUSE_IMAGE_HEAD_BYTES - ss->head_n;
        if (k > n) {
            k = n;
        }
        memcpy(ss->head + ss->head_n, buf, k);
        ss->head_n = (uint16_t)(ss->head_n + k);
        ss->written += k;
        buf += k;
        n -= k;
        if (ss->head_n == MUSE_IMAGE_HEAD_BYTES) {
            rc = check_head(ss);
            if (rc == MG_SMP_EOK) {
                rc = ota_write(ss, ss->head, ss->head_n);
            }
        }
    }
    if (rc == MG_SMP_EOK && n) {
        rc = ota_write(ss, buf, n);
        ss->written += n;
    }
    if (rc != MG_SMP_EOK) {
        s_ui = MG_SMP_UI_STOPPED;
        return rc;
    }
    if (ss->total) {
        s_ui_pct = (uint8_t)(ss->written >= ss->total ? 100 : (uint64_t)ss->written * 100 / ss->total);
        if (ss->written >= ss->total) {
            /* "Installing" from the last byte on, so the check, the
             * commit and the restart don't sit at "Updating 100%". */
            s_ui = MG_SMP_UI_INSTALLING;
            s_ui_shown = false;
        }
    }
    return MG_SMP_EOK;
}

/* A refused or given-up upload leaves nothing that reads as an image: the
 * slot's first sector (the app header and its description) is erased, so
 * image state and ota? show the slot empty, never a half-written or corrupt
 * image as present and bootable ("image state is truthful"). Only once
 * something was written (flashed): until then the slot still holds what it
 * held before, which image state still tells truthfully. */
#define FORGET_BYTES 4096
static void forget_upload(const esp_sess_t *ss)
{
    const esp_partition_t *p = ss->part;
    if (!ss->flashed) {
        return;
    }
    if (p && esp_partition_erase_range(p, 0, FORGET_BYTES) != ESP_OK) {
        ESP_LOGW(TAG, "couldn't clear %s's header", p->label);
    }
}

static int be_finish(void *ctx)
{
    (void)ctx;
    esp_sess_t *ss = sess();
    if (!ss || !ss->open) {
        return MG_SMP_EBADSTATE;
    }
    ss->open = false;
    esp_err_t err = esp_ota_end(ss->ota);   /* the image check, and its signature */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "update refused: %s", esp_err_to_name(err));
        s_ui = MG_SMP_UI_STOPPED;
        forget_upload(ss);
        mg_update_claim(false);
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? MG_SMP_ECORRUPT : MG_SMP_EUNKNOWN;
    }
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(ss->part, &d) != ESP_OK) {
        s_ui = MG_SMP_UI_STOPPED;
        forget_upload(ss);
        mg_update_claim(false);
        return MG_SMP_ECORRUPT;
    }
    ESP_LOGI(TAG, "update %s checked in %s", d.version, ss->part->label);
    return MG_SMP_EOK;
}

static bool can_revert(void)
{
    return s_rb != MG_SMP_RB_ABSENT;
}

static int be_commit(void *ctx, bool permanent)
{
    (void)ctx;
    if (permanent && can_revert()) {
        /* "Permanent without a test": ESP-IDF can only boot it once pending
         * and keep it after it runs. The apps never ask for this. */
        return MG_SMP_EBADSTATE;
    }
    if (!mg_update_claim(true)) {
        ESP_LOGW(TAG, "commit refused: a Wi-Fi update is running");
        return MG_SMP_EBUSY;
    }
    const esp_partition_t *p = other_slot();   /* the slot begin() wrote */
    esp_err_t err = p ? esp_ota_set_boot_partition(p) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit: %s", esp_err_to_name(err));
        return MG_SMP_EBADSTATE;
    }
    s_ui_committed = 1;   /* a stop from here on: installed, pending its test boot */
    if (s_ui != MG_SMP_UI_INSTALLING) {
        s_ui = MG_SMP_UI_INSTALLING;   /* the reset that follows boots it */
        s_ui_shown = false;
    }
    if (can_revert()) {
        ESP_LOGI(TAG, "test: %s boots next, once, until confirmed", p->label);
    } else {
        ESP_LOGW(TAG, "commit: %s boots next, for good (this unit's bootloader can't roll back)", p->label);
    }
    return MG_SMP_EOK;
}

static int be_confirm(void *ctx)
{
    (void)ctx;
    int st = running_state();
    if (st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "confirm: %s", esp_err_to_name(err));
            return MG_SMP_EUNKNOWN;
        }
        ESP_LOGI(TAG, "confirmed: this image stays");
    }
    return MG_SMP_EOK;
}

static void be_abort(void *ctx)
{
    (void)ctx;
    esp_sess_t *ss = sess();
    if (ss && ss->open) {
        esp_ota_abort(ss->ota);
        ss->open = false;
        s_ui = MG_SMP_UI_STOPPED;   /* an upload given up: the running image stays */
        forget_upload(ss);
        mg_update_claim(false);
    } else if (ss && ss->flashed && !s_ui_committed) {
        /* Given up after finish closed the handle, before the commit (the core couldn't save its record): the
         * screen leaves "Installing" and lets the display sleep, the half-installed slot is cleared, and the
         * claim goes back (a Wi-Fi OTA may start). */
        s_ui = MG_SMP_UI_STOPPED;
        forget_upload(ss);
        ss->flashed = false;
        mg_update_claim(false);
    }
}

mg_smp_ui_t mg_smp_esp_ui(uint8_t *pct)
{
    mg_smp_ui_t st = (mg_smp_ui_t)s_ui;
    *pct = st == MG_SMP_UI_WRITING ? s_ui_pct : 0;
    return st;
}

void mg_smp_esp_ui_shown(mg_smp_ui_t st)
{
    if (st == (mg_smp_ui_t)s_ui) {
        s_ui_shown = true;
    }
}

bool mg_smp_esp_installing_unshown(void)
{
    return s_ui == MG_SMP_UI_INSTALLING && !s_ui_shown;
}

bool mg_smp_esp_ui_installed(void)
{
    return s_ui_committed != 0;
}

void mg_smp_esp_ui_session_gone(void)
{
    if (s_ui == MG_SMP_UI_INSTALLING) {
        /* The link went after the last byte with no reset on its way: the
         * screen leaves "Installing" (don't remove power) for its stop. */
        s_ui = MG_SMP_UI_STOPPED;
        ESP_LOGW(TAG, "update stopped after %s: the link went without the reset",
                 s_ui_committed ? "its commit (installed: it boots at the next restart)" : "its last byte (nothing installed)");
    }
}

void mg_smp_esp_ui_clear(void)
{
    if (s_ui == MG_SMP_UI_STOPPED) {
        s_ui = MG_SMP_UI_NONE;
    }
}

static void fill_id(mg_smp_image_id_t *id, const esp_app_desc_t *d)
{
    mg_smp_esp_parse_version(d->version, &id->version);
    id->build_id = mg_smp_esp_build_id(d->app_elf_sha256);
}

static int be_slot_info(void *ctx, uint8_t slot, mg_smp_slot_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    if (slot == 0) {
        fill_id(&out->id, esp_app_get_description());
        out->flags = MG_SMP_SLOT_PRESENT | MG_SMP_SLOT_ACTIVE | MG_SMP_SLOT_BOOTABLE;
        if (running_state() != ESP_OTA_IMG_PENDING_VERIFY) {
            out->flags |= MG_SMP_SLOT_CONFIRMED;   /* NEW (no rollback) is as good as kept */
        }
        return MG_SMP_EOK;
    }
    if (slot != 1) {
        return MG_SMP_ENOENT;
    }
    const esp_partition_t *other = other_slot();
    esp_app_desc_t d;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) {
        return MG_SMP_ENOENT;
    }
    fill_id(&out->id, &d);
    out->flags = MG_SMP_SLOT_PRESENT;
    mg_smp_image_id_t sid;
    if (mg_smp_staged(&sid, out->hash)) {
        out->id = sid;   /* the upload in this slot: its envelope's id and hash */
        out->flags |= MG_SMP_SLOT_HAS_HASH;
    }
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool known = esp_ota_get_state_partition(other, &st) == ESP_OK;
    if (!known || (st != ESP_OTA_IMG_INVALID && st != ESP_OTA_IMG_ABORTED)) {
        out->flags |= MG_SMP_SLOT_BOOTABLE;
    }
    if (known && st == ESP_OTA_IMG_VALID) {
        out->flags |= MG_SMP_SLOT_CONFIRMED;
    }
    if (esp_ota_get_boot_partition() == other) {
        out->flags |= MG_SMP_SLOT_PENDING;
        if (!can_revert()) {
            out->flags |= MG_SMP_SLOT_PERMANENT;
        }
    }
    return MG_SMP_EOK;
}

#define ESP_BACKEND(name, fl)                                                   \
    const mg_smp_backend_t name = {                                             \
        .flags = (fl),                                                          \
        .max_write = 0,                                                         \
        .max_payload = 0,   /* begin() checks the other slot's size */          \
        .begin = be_begin,                                                      \
        .write = be_write,                                                      \
        .finish = be_finish,                                                    \
        .commit = be_commit,                                                    \
        .confirm = be_confirm,                                                  \
        .abort = be_abort,                                                      \
        .slot_info = be_slot_info,                                              \
    }
ESP_BACKEND(mg_smp_esp_backend_revert, MG_SMP_BE_CAN_REVERT);
ESP_BACKEND(mg_smp_esp_backend_permanent, 0);

bool mg_smp_esp_probe(void)
{
    uint8_t digest[32];
    rb_rec_t kept;
    bool kept_ok = rb_load(&kept) && bootloader_digest(digest) && !memcmp(kept.digest, digest, 32);
    int st = running_state();
    mg_smp_rb_t v = mg_smp_esp_verdict(st, kept_ok, kept_ok ? (mg_smp_rb_t)kept.verdict : MG_SMP_RB_UNPROVEN);
    bool dirty = (st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW) && !(kept_ok && kept.verdict == v);
    s_rb_state = (uint8_t)((uint8_t)v | (dirty ? RB_DIRTY : 0u));
    char line[160];
    mg_smp_esp_status(line, sizeof(line));
    ESP_LOGI(TAG, "%s", line);
    mg_smp_esp_status_bootloader(line, sizeof(line));
    ESP_LOGI(TAG, "%s", line);
    if (s_rb == MG_SMP_RB_ABSENT) {
        ESP_LOGW(TAG, "this unit's bootloader was built without rollback: an update can't go back by itself; "
                      "reflash the bootloader over USB to get rollback");
    }
    return can_revert();
}

/* ---- platform hooks that aren't BLE --------------------------------------------- */

int mg_smp_esp_record_get(void *ctx, uint8_t key, void *buf, uint16_t cap, uint16_t *len)
{
    (void)ctx;
    char name[8];
    snprintf(name, sizeof(name), "rec%u", (unsigned)key);
    nvs_handle_t h = 0;
    size_t n = cap;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return MG_SMP_ENOENT;
    }
    esp_err_t err = nvs_get_blob(h, name, buf, &n);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return MG_SMP_ENOENT;
    }
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_INVALID_LENGTH ? MG_SMP_EMSGSIZE : MG_SMP_EUNKNOWN;
    }
    *len = (uint16_t)n;
    return MG_SMP_EOK;
}

int mg_smp_esp_record_put(void *ctx, uint8_t key, const void *buf, uint16_t len)
{
    (void)ctx;
    char name[8];
    snprintf(name, sizeof(name), "rec%u", (unsigned)key);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no NVS for the update record");
        return MG_SMP_EUNKNOWN;
    }
    esp_err_t err = nvs_set_blob(h, name, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        rb_keep();   /* written from an SMP request: the boot is healthy */
    }
    return err == ESP_OK ? MG_SMP_EOK : MG_SMP_EUNKNOWN;
}

void mg_smp_esp_running_id(void *ctx, mg_smp_image_id_t *out)
{
    (void)ctx;
    fill_id(out, esp_app_get_description());
}

bool mg_smp_esp_pending_verify(void)
{
    return running_state() == ESP_OTA_IMG_PENDING_VERIFY;
}

mg_smp_rb_t mg_smp_esp_rollback(void)
{
    return s_rb;
}

void mg_smp_esp_settle(void)
{
    (void)rb_keep();   /* a failed write: mg_smp_esp_keep tries again */
}

bool mg_smp_esp_keep(void)
{
    bool again = !rb_keep();
    /* Two slots: the core's confirm ran just before (mg_smp_healthy); a pending image it couldn't keep (the
     * write failed, logged) is tried again, not left to roll back at the next reset. */
    return again || (can_revert() && running_state() == ESP_OTA_IMG_PENDING_VERIFY);
}

static const char *state_name(int st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "flashed";
    }
}

int mg_smp_esp_status(char *out, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = other_slot();
    esp_app_desc_t od;
    bool have_other = other && esp_ota_get_partition_description(other, &od) == ESP_OK;
    esp_ota_img_states_t ost = ESP_OTA_IMG_UNDEFINED;
    if (other) {
        esp_ota_get_state_partition(other, &ost);
    }
    return snprintf(out, cap, "ota: running %s %s (%s); other %s %s (%s)", run ? run->label : "?",
                    esp_app_get_description()->version, state_name(running_state()), other ? other->label : "none",
                    have_other ? od.version : "-", have_other ? state_name((int)ost) : "empty");
}

int mg_smp_esp_status_bootloader(char *out, size_t cap)
{
    char bl[12] = "?";
    uint8_t digest[32];
    if (bootloader_digest(digest)) {
        snprintf(bl, sizeof(bl), "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
    }
    esp_bootloader_desc_t d;   /* read when asked: no static copy (no idle memory) */
    bool ok = esp_ota_get_bootloader_description(NULL, &d) == ESP_OK;
    return snprintf(out, cap, "ota: bootloader %s %s digest %s; %s", ok ? d.idf_ver : "?", ok ? d.date_time : "", bl,
                    mg_smp_esp_rollback_name(s_rb));
}
