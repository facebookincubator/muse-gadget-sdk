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
 * The ESP32 SMP server on the host, for tests/test_mg_smp.py: the vendored
 * SMP core (components/muse_gadget_ble/smp/) under the REAL ESP-IDF
 * backend (mg_smp_esp.c), in the configuration mg_ble.c gives it (inline, one
 * 1 KB slot, buf_count 3, revert), over the core's fake-GATT link
 * (smp/conformance/smplink.py's Link), so the suite's Android and iOS flows drive it.
 *
 * Underneath, a simulated ESP-IDF OTA layer: two app slots with otadata
 * states, a bootloader with or without rollback (--no-rollback), NVS that
 * survives resets, and the boot guard's part: the healthy mark --healthy-ms
 * after boot (--healthy-ms -1: never), and an image whose version names
 * "crash" resets before healthy (so its test boot comes back the old image).
 *
 *   mg_smp_esp_server --port 0 [--boot-ms N] [--healthy-ms N] [--no-rollback] [--resume-hold-s N]
 *                     [--fail-keeps N] [--fail-blob KEY]
 * prints "PORT <udp port> PLAIN 0", then serves until SIGTERM.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "mg_smp.h"
#include "mg_smp_esp.h"
#include "mg_smp_front.h"
#include "mg_smp_gate.h"
#include "muse_local_press.h"
#include "nvs.h"

/* ---- the simulated device ------------------------------------------------------ */

#define SLOT_SIZE (4u << 20)

typedef struct {
    esp_partition_t part;
    uint8_t *data;
    uint32_t len;
    bool entry;                   /* otadata has an entry for it */
    esp_ota_img_states_t state;
} slot_t;

static slot_t s_slot[2];
static unsigned s_erases;   /* esp_partition_erase_range calls (a refused upload's header) */
static unsigned s_writes;   /* esp_ota_write calls: none before the image's head is checked */
static uint32_t s_wpos;     /* the open slot's write position */
static int s_run, s_boot;         /* running and next-boot slot */
static bool s_rollback_bl = true; /* the bootloader has rollback */
static esp_app_desc_t s_desc;     /* the running image's */
static int s_ota_open = -1;       /* slot being written */
static uint32_t s_boots;

static uint32_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%u] ", now_ms() % 100000000u);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof(b), "0x%x", (unsigned)e);
    return b;
}

/* A fake ESP-IDF app image: 0xE9, header, the esp_app_desc_t at 32. */
static bool parse_desc(const uint8_t *d, uint32_t n, esp_app_desc_t *out)
{
    if (n < 32 + sizeof(esp_app_desc_t) || d[0] != 0xE9) {
        return false;
    }
    memcpy(out, d + 32, sizeof(*out));
    return out->magic_word == 0xABCD5432u;
}

static void factory(void)
{
    static const char *labels[2] = { "ota_0", "ota_1" };
    for (int i = 0; i < 2; i++) {
        s_slot[i].part = (esp_partition_t){ .address = 0x20000 + (uint32_t)i * SLOT_SIZE, .size = SLOT_SIZE,
                                            .type = 0, .subtype = 0x10 + i };
        strcpy(s_slot[i].part.label, labels[i]);
        s_slot[i].data = calloc(1, SLOT_SIZE);
    }
    /* What a USB flash leaves: the factory image in ota_0, otadata blank. */
    uint8_t *d = s_slot[0].data;
    d[0] = 0xE9;
    esp_app_desc_t a = { .magic_word = 0xABCD5432u };
    strcpy(a.version, "1.0.0+gfactory");
    for (int i = 0; i < 32; i++) {
        a.app_elf_sha256[i] = (uint8_t)(0xF0 + i);
    }
    memcpy(d + 32, &a, sizeof(a));
    s_slot[0].len = 4096;
    s_run = s_boot = 0;
}

/* What the bootloader does at a reset. */
static void bootloader(void)
{
    if (s_rollback_bl) {
        slot_t *r = &s_slot[s_run];
        if (r->entry && r->state == ESP_OTA_IMG_PENDING_VERIFY && s_boot == s_run) {
            r->state = ESP_OTA_IMG_ABORTED;   /* a test boot that never confirmed */
            s_boot = 1 - s_run;
            logf_("bootloader: %s was never confirmed: back to %s", r->part.label, s_slot[s_boot].part.label);
        }
    }
    s_run = s_boot;
    slot_t *b = &s_slot[s_run];
    if (s_rollback_bl && b->entry && b->state == ESP_OTA_IMG_NEW) {
        b->state = ESP_OTA_IMG_PENDING_VERIFY;
    }
    parse_desc(b->data, b->len, &s_desc);
    s_boots++;
    logf_("boot %u: %s %s, state %d", s_boots, b->part.label, s_desc.version, b->entry ? (int)b->state : -1);
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_desc;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &s_slot[s_run].part;
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    (void)start_from;
    return &s_slot[1 - s_run].part;
}

const esp_partition_t *esp_ota_get_boot_partition(void)
{
    return &s_slot[s_boot].part;
}

static int idx(const esp_partition_t *p)
{
    return p == &s_slot[0].part ? 0 : p == &s_slot[1].part ? 1 : -1;
}

esp_err_t esp_ota_begin(const esp_partition_t *p, size_t image_size, esp_ota_handle_t *out)
{
    (void)image_size;
    int i = idx(p);
    if (i < 0 || i == s_run) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_slot[s_run].entry && s_slot[s_run].state == ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_ERR_OTA_ROLLBACK_INVALID_STATE;   /* as ESP-IDF does */
    }
    /* As ESP-IDF v6.0.1 with OTA_WITH_SEQUENTIAL_WRITES: nothing is erased
     * here; the first write erases as it goes (and with it the old image). */
    s_wpos = 0;
    s_ota_open = i;
    *out = 1;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t size)
{
    (void)h;
    if (s_ota_open < 0 || s_wpos + size > SLOT_SIZE) {
        return ESP_FAIL;
    }
    slot_t *s = &s_slot[s_ota_open];
    if (s_wpos == 0) {
        if (((const uint8_t *)data)[0] != 0xE9) {
            return ESP_ERR_OTA_VALIDATE_FAILED;   /* ESP-IDF checks the magic byte, nothing else */
        }
        memset(s->data, 0xFF, SLOT_SIZE);   /* the erase that goes with the first write */
    }
    s_writes++;
    memcpy(s->data + s_wpos, data, size);
    s_wpos += (uint32_t)size;
    s->len = s_wpos;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t h)
{
    (void)h;
    int i = s_ota_open;
    s_ota_open = -1;
    esp_app_desc_t d;
    if (i < 0 || !parse_desc(s_slot[i].data, s_slot[i].len, &d)) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (strstr(d.version, "badsig")) {
        return ESP_ERR_OTA_VALIDATE_FAILED;   /* the RSA signature, simulated */
    }
    return ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t h)
{
    (void)h;
    s_ota_open = -1;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    int i = idx(p);
    if (i < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_boot = i;
    if (i != s_run) {
        s_slot[i].entry = true;
        s_slot[i].state = ESP_OTA_IMG_NEW;   /* the app's CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE */
    }
    return ESP_OK;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state)
{
    int i = idx(p);
    if (i < 0 || !s_slot[i].entry) {
        return ESP_ERR_NOT_FOUND;
    }
    *state = s_slot[i].state;
    return ESP_OK;
}

static uint32_t s_confirms;

static int s_fail_keeps;       /* --fail-keeps N: the next N keeps fail (a transient otadata write error) */
static char s_fail_blob[16];   /* --fail-blob KEY: writes of that NVS key fail */

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    slot_t *r = &s_slot[s_run];
    if (r->entry && r->state != ESP_OTA_IMG_VALID && s_fail_keeps > 0) {
        s_fail_keeps--;
        logf_("keep: otadata write failed");
        return ESP_FAIL;
    }
    if (r->entry && r->state != ESP_OTA_IMG_VALID) {
        r->state = ESP_OTA_IMG_VALID;
        s_confirms++;
        logf_("confirm: %s %s is valid", r->part.label, s_desc.version);
    }
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_invalid_rollback(void)
{
    return ESP_ERR_OTA_ROLLBACK_FAILED;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t offset, size_t size)
{
    int i = idx(p);
    if (i < 0 || offset + size > SLOT_SIZE || i == s_run) {
        return ESP_ERR_INVALID_ARG;   /* never the running slot */
    }
    memset(s_slot[i].data + offset, 0xFF, size);
    s_erases++;
    return ESP_OK;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *desc)
{
    int i = idx(p);
    return i >= 0 && parse_desc(s_slot[i].data, s_slot[i].len, desc) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t esp_ota_get_bootloader_description(const esp_partition_t *bootloader, esp_bootloader_desc_t *desc)
{
    (void)bootloader;
    memset(desc, 0, sizeof(*desc));
    desc->magic_byte = 80;
    strcpy(desc->idf_ver, s_rollback_bl ? "v6.0.1-rollback" : "v6.0.1-norollback");
    return ESP_OK;
}

esp_err_t esp_image_get_metadata(const esp_partition_pos_t *part, esp_image_metadata_t *md)
{
    (void)part;
    md->image.hash_appended = 1;
    md->image_len = 20000;
    memset(md->image_digest, s_rollback_bl ? 0xB1 : 0xB0, 32);   /* one bootloader each */
    return ESP_OK;
}

/* NVS: a few blobs that survive resets. */
typedef struct {
    char key[32];
    uint8_t v[256];
    size_t n;
    bool used;
} blob_t;
static blob_t s_nvs[16];
static char s_ns[16][16];

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    for (uint32_t i = 0; i < 16; i++) {
        if (!s_ns[i][0] || !strcmp(s_ns[i], ns)) {
            snprintf(s_ns[i], sizeof(s_ns[i]), "%s", ns);
            *out = i + 1;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static blob_t *find(nvs_handle_t h, const char *key, bool make)
{
    char k[32];
    snprintf(k, sizeof(k), "%u/%s", (unsigned)h, key);
    for (int i = 0; i < 16; i++) {
        if (s_nvs[i].used && !strcmp(s_nvs[i].key, k)) {
            return &s_nvs[i];
        }
    }
    for (int i = 0; make && i < 16; i++) {
        if (!s_nvs[i].used) {
            s_nvs[i].used = true;
            strcpy(s_nvs[i].key, k);
            return &s_nvs[i];
        }
    }
    return NULL;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    blob_t *b = find(h, key, false);
    if (!b) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*len < b->n) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, b->v, b->n);
    *len = b->n;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t len)
{
    if (s_fail_blob[0] && !strcmp(key, s_fail_blob)) {
        logf_("nvs: %s write failed", key);
        return ESP_FAIL;
    }
    blob_t *b = find(h, key, true);
    if (!b || len > sizeof(b->v)) {
        return ESP_FAIL;
    }
    memcpy(b->v, v, len);
    b->n = len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    (void)h;
}

/* ---- the link and the platform (mg_ble.c's configuration) ----------------------------- */

static int s_sock = -1;
static struct sockaddr_in s_peer;
static bool s_have_peer, s_connected, s_down;
static uint16_t s_mtu = 23;
static int s_drop_next;
static uint32_t s_reset_at, s_up_at, s_boot_t, s_crash_at;
static int s_boot_ms = 300, s_healthy_ms = 300;
static uint32_t s_sends;
static volatile sig_atomic_t s_stop;

/* mg_ble.c's configuration (the resume hold settable for the test). */
static mg_smp_config_t s_cfg = { .slot_size = 1024, .buf_size = 512, .rx_slots = 1, .buf_count = 3, .inline_rx = 1,
                                 .resume_hold_s = 120, .be_scratch = MG_SMP_ESP_SCRATCH };

/* mem_get/mem_put, tracked: what the core holds now, at most one of each kind. */
static void *s_mem[2];
static uint32_t s_mem_size[2], s_mem_gets;

static void *pf_mem_get(void *ctx, uint8_t kind, uint32_t size)
{
    (void)ctx;
    if (kind > 1 || s_mem[kind]) {
        logf_("BUG: mem_get kind %u while one is out", kind);
        return NULL;
    }
    void *p = aligned_alloc(8, (size + 7u) & ~7u);
    if (!p) {
        return NULL;
    }
    memset(p, 0xA5, size);   /* the core must not rely on zeroed memory */
    s_mem[kind] = p;
    s_mem_size[kind] = size;
    s_mem_gets++;
    return p;
}

/* The claim on the other slot it shares with the Wi-Fi OTA (main/ota.c,
 * mg_glue.c in the firmware). --wifi-ota: a Wi-Fi download holds it. */
static bool s_wifi_ota, s_claimed;
bool mg_update_claim(bool take)
{
    if (!take) {
        s_claimed = false;
        return true;
    }
    if (s_wifi_ota) {
        return false;
    }
    s_claimed = true;
    return true;
}

static void pf_mem_put(void *ctx, uint8_t kind, void *block)
{
    (void)ctx;
    if (kind > 1 || block != s_mem[kind]) {
        logf_("BUG: mem_put kind %u of a block that isn't out", kind);
        return;
    }
    free(block);
    s_mem[kind] = NULL;
    s_mem_size[kind] = 0;
    if (kind == MG_SMP_MEM_STATE) {
        mg_smp_esp_session_end();   /* as mg_ble.c does */
    }
}

static void gatt_send(uint8_t type, const uint8_t *v, size_t n)
{
    if (!s_have_peer) {
        return;
    }
    uint8_t d[600];
    if (n > sizeof(d) - 1) {
        n = sizeof(d) - 1;
    }
    d[0] = type;
    if (n) {
        memcpy(d + 1, v, n);
    }
    sendto(s_sock, d, n + 1, 0, (struct sockaddr *)&s_peer, sizeof(s_peer));
}

static int pf_send(void *ctx, const uint8_t *buf, uint16_t n)
{
    (void)ctx;
    if (!s_connected || s_down) {
        return MG_SMP_EUNKNOWN;
    }
    if (n > s_mtu - 3) {
        logf_("BUG: notification of %u over MTU %u", n, s_mtu);
    }
    gatt_send(0x00, buf, n);
    s_sends++;
    return 0;   /* never MG_SMP_EAGAIN: mg_ble.c waits out a full TX queue */
}

static uint16_t pf_mtu(void *ctx)
{
    (void)ctx;
    return s_mtu;
}

static void pf_reset(void *ctx)
{
    (void)ctx;
    s_reset_at = now_ms() + 100;   /* SMP_RESET_AFTER_TX_MS */
}

static bool pf_claim(void *ctx, bool claim)
{
    (void)ctx;
    (void)claim;
    return true;
}

static void pf_media_stop(void *ctx)
{
    (void)ctx;
}

static bool pf_healthy(void *ctx)
{
    (void)ctx;
    return !s_down && s_healthy_ms >= 0 && (int32_t)(now_ms() - s_boot_t) >= s_healthy_ms;
}

static uint32_t pf_now(void *ctx)
{
    (void)ctx;
    return now_ms();
}

static void pf_log(void *ctx, const char *line)
{
    (void)ctx;
    logf_("smp: %s", line);
}

/* mg_ble.c's smp_may_change answers from the connection (its token
 * proof, not set up, a SAFE the user entered). --auth proven|unproven wires the hook with every
 * connection starting proven or not; LINK.md 0x09 proves or un-proves this
 * one; a disconnect or reset ends it. Without --auth the hook isn't wired. */
static bool s_auth_on, s_auth_unproven, s_auth_proven;
static unsigned s_auth_asks, s_auth_refused;
static uint8_t s_auth_last_op;
/* --gate: mg_ble.c's update gate (protocols/test-vectors/mg-update-v2.json): mg_smp_gate.h from the device's state
 * (0x0C: holds a token, a release build, a recovery the user's button entered, then optionally the wired console's
 * arm, a passcode lock, a recovery the gadget entered itself), and on a link that needs the press the request front
 * (mg_smp_front.c, as mg_ble.c's smp_front_whole runs it) asking the real muse_local_press core about each change
 * whole (0x0D: 1 a press on the gadget, counted only while the prompt shows; 2 a new link for the core; 3 the token
 * proof (as 0x09 1); 4 a cancel on the gadget; 5 the passcode entered; 6 + u32 the clock moves on that many ms; 7 an
 * injected press; 8 Forget; 9 set up again). The front answers rc 10 while the prompt asks and 11 for a refusal itself; the core's
 * hooks say yes to what it let through. A new link ends the prompt. The update vectors replay through it (test_mg_smp
 * UpdateV2Vectors). Every write goes through the front, as on the gadget. */
static bool s_gate, s_g_set_up = true, s_g_release = true, s_g_button, s_g_locked, s_g_self_safe;
static muse_local_press_t s_g_press = { .prompt_ms = -1, .bound_ms = -1, .covered_ms = -1, .armed_ms = -1 };
static mg_smp_front_rx_t s_front;
static mg_smp_front_slot_t s_fslot;
static mg_smp_front_up_t s_up;   /* the length the core's upload named (mg_ble.c's s_smp_up) */
static bool s_front_ok;   /* the front let the request the core is reading through */
static int64_t s_g_skew;   /* the 0x0D 6 moves */
static int s_g_rc = 11;

static int64_t g_now(void)
{
    return (int64_t)pf_now(NULL) + s_g_skew;
}

static int pf_refusal_rc(void *ctx)
{
    (void)ctx;
    return s_g_rc;
}

static mg_smp_gate_t g_gate(void)
{
    (void)s_g_set_up;   /* set up or not, the same */
    bool safe = s_g_button || s_g_self_safe;
    return mg_smp_gate(safe, s_g_button, s_auth_proven, s_g_release);
}

static bool gate_ask(uint8_t op)
{
    s_auth_asks++;
    s_auth_last_op = op;
    bool ok = s_auth_proven;
    s_g_rc = 11;
    if (s_gate) {
        mg_smp_gate_t g = g_gate();
        ok = g == MG_SMP_GATE_OPEN || (g == MG_SMP_GATE_NEEDS_PRESS && s_front_ok);
    }
    if (!ok) {
        s_auth_refused++;
        logf_("auth: op %u refused rc %d (connection not proven)", op, s_g_rc);
    }
    return ok;
}

static bool pf_may_change(void *ctx, uint8_t op)
{
    (void)ctx;
    return gate_ask(op);
}

static bool pf_may_upload(void *ctx, uint32_t len, const uint8_t *sha32)
{
    (void)ctx;
    (void)len;
    (void)sha32;   /* the front read the upload itself */
    return gate_ask(MG_SMP_OP_UPLOAD);
}

static muse_update_answer_t front_ask(void *ctx, const muse_update_req_t *req)
{
    (void)ctx;
    return muse_local_press_core_ask(&s_g_press, g_now(), s_g_locked, req);
}

static bool front_bound(void *ctx, muse_update_ident_t *id)
{
    (void)ctx;
    return muse_local_press_core_bound(&s_g_press, id);
}

static mg_smp_platform_t s_pf = {
    .send = pf_send,
    .mtu = pf_mtu,
    .record_get = mg_smp_esp_record_get,
    .record_put = mg_smp_esp_record_put,
    .running_id = mg_smp_esp_running_id,
    .reset = pf_reset,
    .ota_claim = pf_claim,
    .media_stop = pf_media_stop,
    .boot_healthy = pf_healthy,
    .mem_get = pf_mem_get,
    .mem_put = pf_mem_put,
    .now_ms = pf_now,
    .log = pf_log,
};

static const mg_smp_env_t s_env_revert = { &s_cfg, &s_pf, &mg_smp_esp_backend_revert };
static const mg_smp_env_t s_env_permanent = { &s_cfg, &s_pf, &mg_smp_esp_backend_permanent };
static const mg_smp_env_t *s_env;

/* One whole request, as mg_ble.c's smp_front_whole takes it. */
static void front_whole(void *ctx, const uint8_t *req, size_t n)
{
    (void)ctx;
    if (!mg_smp_front_depth_ok(req, n)) {
        uint8_t r[13];
        logf_("front: a request nested deeper than %d levels, or malformed, refused", MG_SMP_FRONT_MAX_DEPTH);
        (void)pf_send(NULL, r, (uint16_t)mg_smp_front_refusal(req, MG_SMP_EINVAL, r));
        return;
    }
    /* An upload write past its upload's length: rc 11 and the upload ends, however the link was opened, wherever the
     * hooks are wired (--auth, --gate), as mg_ble.c's front does (smp-v12). */
    bool past = s_auth_on && mg_smp_front_past_len(req, n, &s_up, mg_smp_upload_active());
    bool ended = false;
    int rc = 0;
    bool gated = s_gate && g_gate() == MG_SMP_GATE_NEEDS_PRESS;
    if (gated) {
        rc = mg_smp_front_gate(req, n, &s_fslot, mg_smp_upload_active(), past, front_ask, front_bound, NULL, &ended);
    } else if (past) {
        rc = MG_SMP_EACCESSDENIED;
    }
    if (ended || (past && rc == MG_SMP_EACCESSDENIED)) {
        mg_smp_abort(s_env);   /* the binding ended, or a write past the length: the upload goes */
    }
    if (rc > 0) {
        uint8_t r[13];
        s_auth_asks++;
        s_auth_refused++;
        logf_("front: an update change refused rc %d%s", rc, past ? " (past its upload's length)" : "");
        (void)pf_send(NULL, r, (uint16_t)mg_smp_front_refusal(req, rc, r));
        return;
    }
    (void)mg_smp_front_up_note(req, n, &s_up);
    s_front_ok = gated && rc == 0;
    mg_smp_rx(s_env, req, (uint16_t)n);
    s_front_ok = false;
}

static void front_raw(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    mg_smp_rx(s_env, p, (uint16_t)n);
}

static const mg_smp_front_out_t s_front_out = { front_whole, front_raw, NULL };
static bool s_marked;
static uint8_t s_marks;   /* mg_ble.c's s_smp_marks: the keep's retries */

static void boot(void)
{
    bootloader();
    s_boot_t = now_ms();
    s_crash_at = strstr(s_desc.version, "crash") ? s_boot_t + 50 : 0;
    /* A reset loses RAM: whatever the core held is gone, and so is its pointer. */
    for (int k = 0; k < 2; k++) {
        free(s_mem[k]);
        s_mem[k] = NULL;
        s_mem_size[k] = 0;
    }
    mg_smp_test_reboot();
    s_marked = false;
    s_marks = 0;
    s_claimed = false;   /* RAM */
    s_auth_proven = !s_auth_unproven;   /* a reboot ends every connection's proof */
    s_env = mg_smp_esp_probe() ? &s_env_revert : &s_env_permanent;
    if (mg_smp_check(s_env) != MG_SMP_EOK) {
        logf_("BUG: mg_smp_check refused the ESP32 configuration");
        exit(3);
    }
}

static void status(void)
{
    char j[1024];
    mg_smp_version_t v;
    mg_smp_esp_parse_version(s_desc.version, &v);
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(&s_slot[s_run].part, &st);
    int n = snprintf(j, sizeof(j),
                     "{\"running\":\"%u.%u.%u+%u\",\"version_string\":\"%.*s\",\"build_id\":%u,\"boots\":%u,"
                     "\"slot\":%d,\"state\":%d,\"healthy\":%s,\"rollback\":%d,\"confirms\":%u,"
                     "\"can_revert\":%s,\"drops\":%u,\"held\":%u,\"mem_state\":%u,\"mem_buffers\":%u,"
                     "\"mem_gets\":%u,\"ota_open\":%s,\"other_present\":%s,\"erases\":%u,\"claimed\":%s,"
                     "\"writes\":%u,\"ui\":%d,\"ui_installed\":%s,\"ui_stop_text\":\"%s\","
                     "\"auth\":%d,\"auth_asks\":%u,\"auth_refused\":%u,\"auth_last_op\":%u}",
                     v.major, v.minor, v.revision, (unsigned)v.build, (int)sizeof(s_desc.version), s_desc.version,
                     (unsigned)mg_smp_esp_build_id(s_desc.app_elf_sha256), (unsigned)s_boots, s_run, (int)st,
                     pf_healthy(NULL) ? "true" : "false", (int)mg_smp_esp_rollback(), (unsigned)s_confirms,
                     mg_smp_esp_rollback() == MG_SMP_RB_ABSENT ? "false" : "true", 0u, (unsigned)mg_smp_held(s_env),
                     (unsigned)s_mem_size[0], (unsigned)s_mem_size[1], (unsigned)s_mem_gets,
                     s_ota_open >= 0 ? "true" : "false",
                     esp_ota_get_partition_description(&s_slot[1 - s_run].part, &(esp_app_desc_t){0}) == ESP_OK
                         ? "true" : "false",
                     s_erases, s_claimed ? "true" : "false", s_writes, (int)mg_smp_esp_ui(&(uint8_t){0}),
                     mg_smp_esp_ui_installed() ? "true" : "false",
                     mg_smp_esp_ui(&(uint8_t){0}) == MG_SMP_UI_STOPPED ? mg_smp_esp_stop_text(mg_smp_esp_ui_installed()) : "",
                     (int)s_auth_proven, s_auth_asks, s_auth_refused, (unsigned)s_auth_last_op);
    if (n < 0) {
        return;
    }
    /* Never past j (gatt_send keeps to its own buffer too). */
    gatt_send(0x07, (const uint8_t *)j, (size_t)n < sizeof(j) ? (size_t)n : sizeof(j) - 1);
}

static void on_sig(int s)
{
    (void)s;
    s_stop = 1;
}

int main(int argc, char **argv)
{
    int port = 0;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : "0";
        if (!strcmp(argv[i], "--port")) {
            port = atoi(v), i++;
        } else if (!strcmp(argv[i], "--boot-ms")) {
            s_boot_ms = atoi(v), i++;
        } else if (!strcmp(argv[i], "--healthy-ms")) {
            s_healthy_ms = atoi(v), i++;
        } else if (!strcmp(argv[i], "--no-rollback")) {
            s_rollback_bl = false;
        } else if (!strcmp(argv[i], "--wifi-ota")) {
            s_wifi_ota = true;
        } else if (!strcmp(argv[i], "--auth")) {
            s_auth_on = strcmp(v, "off") != 0;
            s_auth_unproven = !strcmp(v, "unproven");
            i++;
        } else if (!strcmp(argv[i], "--gate")) {
            s_gate = s_auth_on = s_auth_unproven = true;
        } else if (!strcmp(argv[i], "--resume-hold-s")) {
            s_cfg.resume_hold_s = (uint16_t)atoi(v), i++;
        } else if (!strcmp(argv[i], "--fail-keeps")) {
            s_fail_keeps = atoi(v), i++;
        } else if (!strcmp(argv[i], "--fail-blob")) {
            snprintf(s_fail_blob, sizeof(s_fail_blob), "%s", v), i++;
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int big = 1 << 20;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("bind");
        return 2;
    }
    socklen_t al = sizeof(a);
    getsockname(s_sock, (struct sockaddr *)&a, &al);
    if (s_cfg.slot_size != MG_SMP_FRONT_MAX) {
        logf_("BUG: the request front holds %d bytes, the core's slot %u", MG_SMP_FRONT_MAX, (unsigned)s_cfg.slot_size);
        return 3;
    }
    if (s_auth_on) {
        s_pf.may_change = pf_may_change;
        s_pf.refusal_rc = pf_refusal_rc;
        s_pf.may_upload = pf_may_upload;
    }
    factory();
    boot();
    printf("PORT %d PLAIN 0\n", ntohs(a.sin_port));
    fflush(stdout);
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    uint8_t d[70000];
    while (!s_stop) {
        struct pollfd fd = { s_sock, POLLIN, 0 };
        poll(&fd, 1, 2);
        uint32_t now = now_ms();
        if (s_crash_at && !s_down && (int32_t)(now - s_crash_at) >= 0) {
            s_crash_at = 0;
            logf_("crash before the healthy mark (boot guard reset)");
            s_reset_at = now;
        }
        if (s_reset_at && (int32_t)(now - s_reset_at) >= 0) {
            s_reset_at = 0;
            if (s_connected) {
                gatt_send(0x02, NULL, 0);
                s_connected = false;
            }
            s_down = true;
            s_up_at = now + (uint32_t)s_boot_ms;
        }
        if (s_down && (int32_t)(now - s_up_at) >= 0) {
            s_down = false;
            boot();
        }
        if (fd.revents & POLLIN) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(s_sock, d, sizeof(d), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                s_peer = from;
                s_have_peer = true;
            }
            if (n > 0 && d[0] == 0x07) {
                status();
            } else if (n > 0 && !s_down) {
                switch (d[0]) {
                case 0x00:
                    if (!s_connected) {
                        break;
                    }
                    if (s_drop_next > 0) {
                        s_drop_next--;
                        break;
                    }
                    if (n - 1 > s_mtu - 3) {
                        logf_("link: write of %zd over MTU %u refused", n - 1, s_mtu);
                        break;
                    }
                    if (n - 1 > 512) {
                        /* Android refuses such a write (GATT's attribute maximum): the
                         * server made the client send one (its buf_size). */
                        logf_("BUG: a write of %zd bytes: over 512, Android can't send it", n - 1);
                        break;
                    }
                    mg_smp_front_rx(&s_front, d + 1, (size_t)(n - 1), now_ms(), &s_front_out);
                    break;
                case 0x01:
                    s_mtu = n >= 3 ? (uint16_t)(d[1] | d[2] << 8) : 23;
                    if (s_mtu < 23) {
                        s_mtu = 23;
                    }
                    s_connected = true;
                    s_auth_proven = !s_auth_unproven;   /* a new connection */
                    muse_local_press_core_link_new(&s_g_press);   /* ...ends the update's prompt and press */
                    mg_smp_front_rx_reset(&s_front);
                    gatt_send(0x04, NULL, 0);
                    break;
                case 0x02:
                    if (s_connected) {
                        s_connected = false;
                        s_auth_proven = !s_auth_unproven;   /* the proof was this connection's */
                        mg_smp_disconnect(s_env);
                        mg_smp_front_rx_reset(&s_front);
                        if (!s_reset_at) {
                            mg_smp_esp_ui_session_gone();   /* as mg_ble.c does: the link went, no reset coming */
                        }
                    }
                    break;
                case 0x05:
                    s_drop_next = n >= 2 ? d[1] : 1;
                    break;
                case 0x06:
                    mg_smp_abort(s_env);
                    break;
                case 0x09:
                    s_auth_proven = n >= 2 && d[1] != 0;
                    break;
                case 0x0C:
                    if (n >= 4) {
                        s_g_set_up = d[1] != 0;
                        s_g_release = d[2] != 0;
                        s_g_button = d[3] != 0;
                    }
                    if (n >= 7) {
                        if (d[4]) {
                            muse_local_press_core_arm(&s_g_press, g_now());   /* the wired console's update.arm */
                        }
                        s_g_locked = d[5] != 0;
                        s_g_self_safe = d[6] != 0;
                    }
                    break;
                case 0x0D:
                    if (n >= 2) {
                        if (d[1] == 1) {
                            (void)muse_local_press_core_note(&s_g_press, g_now(), s_g_locked);
                        } else if (d[1] == 2) {
                            muse_local_press_core_link_new(&s_g_press);
                        } else if (d[1] == 3) {
                            s_auth_proven = true;
                        } else if (d[1] == 4) {
                            muse_local_press_core_cancel(&s_g_press, g_now());
                        } else if (d[1] == 5) {
                            s_g_locked = false;
                        } else if (d[1] == 7) {
                            /* an injected press (a console or Nordic UART word, a scripted touch): never a
                             * confirm, so the core never hears it */
                        } else if (d[1] == 8) {
                            /* Forget: the device token erased, with the link's proof and the press's binding (the
                             * press is still asked; the wired arm stays), as mg_ble_proof_clear_done does */
                            s_g_set_up = false;
                            s_auth_proven = false;
                            muse_local_press_core_forget(&s_g_press, g_now());
                        } else if (d[1] == 9) {
                            s_g_set_up = true;   /* set up again (a new device token): an unproven change still asks */
                        } else if (d[1] == 6 && n >= 6) {
                            s_g_skew += (int64_t)(d[2] | d[3] << 8 | d[4] << 16 | (uint32_t)d[5] << 24);
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (!s_down) {
            if (!s_marked && pf_healthy(NULL)) {
                /* mg_ble.c's mark, its retries 10x faster (uptime in 100 ms units) */
                uint32_t up = (uint32_t)(now_ms() - s_boot_t) / 100u;
                if (mg_smp_esp_keep_due(s_marks, up)) {
                    int rc = mg_smp_healthy(s_env);   /* the healthy mark: an event, once (mg_ble.c) */
                    if (rc != MG_SMP_ENOMEM && rc != MG_SMP_EBUSY) {
                        if (!mg_smp_esp_keep()) {
                            s_marked = true;
                        } else if (mg_smp_esp_keep_spent(s_marks)) {
                            s_marked = true;
                            logf_("mark: the keep failed %u times", (unsigned)MG_SMP_ESP_KEEP_TRIES + 1);
                        } else {
                            s_marks = mg_smp_esp_keep_tried(s_marks, up);
                        }
                    }
                }
            }
            mg_smp_poll(s_env);
        }
    }
    logf_("exit: boots %u confirms %u held %u", (unsigned)s_boots, (unsigned)s_confirms, (unsigned)mg_smp_held(s_env));
    return 0;
}
