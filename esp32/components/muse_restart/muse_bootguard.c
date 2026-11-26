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

/* The boot guard on the device (muse_bootguard.h) and the gadget watchdog
 * (muse_wdt.h). */

#include "muse_bootguard.h"
#include "muse_crashrec.h"
#include "muse_wdt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_restart.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "muse_retained.h"
#include "soc/soc_caps.h"
#if CONFIG_ESP_TASK_WDT_EN
#include "esp_task_wdt.h"
#endif
#if CONFIG_MUSE_BOOTGUARD && SOC_USB_SERIAL_JTAG_SUPPORTED
#include "hal/usb_serial_jtag_ll.h"
#endif
#if CONFIG_MUSE_BOOTGUARD && (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3)
#include "soc/rtc_cntl_reg.h"
#define DOWNLOAD_BOOT() REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)
#elif CONFIG_MUSE_BOOTGUARD && CONFIG_IDF_TARGET_ESP32C6
#include "soc/lp_aon_reg.h"
#define DOWNLOAD_BOOT() REG_SET_BIT(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT)
#endif

__attribute__((unused)) static const char *TAG = "muse_boot";   /* unused with CONFIG_MUSE_BOOTGUARD off */

#if CONFIG_MUSE_BOOTGUARD
static MUSE_RETAINED_ATTR muse_bootguard_rec_t s_rec;
#endif
static muse_boot_mode_t s_mode = MUSE_BOOT_NORMAL;
static muse_boot_why_t s_why = MUSE_BOOT_WHY_NONE;   /* why this boot is SAFE */


#define NVS_NS "muse_boot"
#define NVS_OFF_KEY "safe_elf"   /* the image whose crashes turned Muse off (8 bytes of its ELF SHA-256) */

#if CONFIG_MUSE_BOOTGUARD
/* A USB host sends a start-of-frame every millisecond: one in 3 ms means a
 * computer is on the cable (a charger alone sends none). */
static bool usb_host_present(void)
{
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);
    esp_rom_delay_us(3000);
    return (usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SOF) != 0;
#else
    return false;
#endif
}

static bool nvs_ready(void)
{
    /* Before Link's config store: initializing twice is harmless, and a
     * partition that needs erasing is left for the config store to handle. */
    return nvs_flash_init() == ESP_OK;
}

static bool kept_off(void)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t elf[8];
    size_t n = sizeof(elf);
    bool off = nvs_get_blob(h, NVS_OFF_KEY, elf, &n) == ESP_OK && n == sizeof(elf);
    if (off && memcmp(elf, esp_app_get_description()->app_elf_sha256, sizeof(elf)) != 0) {
        /* Another image (an update or a flash): it gets its own chance. */
        ESP_LOGW(TAG, "new firmware since Muse turned itself off: on again");
        if (nvs_erase_key(h, NVS_OFF_KEY) != ESP_OK || nvs_commit(h) != ESP_OK) {
            /* On this boot anyway; the next boot compares again. */
            ESP_LOGE(TAG, "the keep-off record couldn't be erased");
        }
        off = false;
    }
    nvs_close(h);
    return off;
}

static void keep_off(void)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "can't keep Muse off: no NVS (SAFE this boot only)");
        return;
    }
    if (nvs_set_blob(h, NVS_OFF_KEY, esp_app_get_description()->app_elf_sha256, 8) != ESP_OK
        || nvs_commit(h) != ESP_OK) {
        ESP_LOGE(TAG, "can't keep Muse off: the record wasn't saved (SAFE this boot only)");
    }
    nvs_close(h);
}
#endif

muse_boot_mode_t muse_bootguard_begin(void)
{
#if CONFIG_MUSE_BOOTGUARD
    muse_bootguard_in_t in = {
        .reason = muse_restart_boot_reason(),
        .intentional = muse_restart_boot_intentional(NULL, NULL),
#ifdef DOWNLOAD_BOOT
        .can_download = true,
#endif
    };
    in.kept_off = kept_off();
    /* Only worth the 3 ms when it could matter. */
    if (muse_bootguard_rec_valid(&s_rec) && s_rec.pending && s_rec.failed + 1 >= MUSE_BOOTGUARD_FAILED_BOOTS) {
        in.usb_host = usb_host_present();
    }
    muse_bootguard_out_t out = muse_bootguard_decide(&s_rec, &in);
    s_mode = out.mode;
    s_why = out.mode == MUSE_BOOT_NORMAL ? MUSE_BOOT_WHY_NONE : out.why;
    if (out.mode != MUSE_BOOT_NORMAL) {
        ESP_LOGW(TAG, "boot guard: %s -> %s", muse_bootguard_why_name(out.why),
                 out.mode == MUSE_BOOT_DOWNLOAD ? "ROM download mode (USB host present)" : "SAFE mode");
    }
    if (out.keep_off) {
        keep_off();
    }
#ifdef DOWNLOAD_BOOT
    if (out.mode == MUSE_BOOT_DOWNLOAD) {
        /* The ROM reads the flag at the next reset and waits for esptool on
         * USB instead of booting this image. Not a planned restart: no marker. */
        DOWNLOAD_BOOT();
        esp_restart();
    }
#endif
    if (s_mode == MUSE_BOOT_DOWNLOAD) {
        s_mode = MUSE_BOOT_SAFE;
    }
#endif
    return s_mode;
}

muse_boot_mode_t muse_bootguard_mode(void)
{
    return s_mode;
}

muse_boot_why_t muse_bootguard_safe_why(void)
{
    return s_mode == MUSE_BOOT_SAFE ? s_why : MUSE_BOOT_WHY_NONE;
}

/* NULL where the build leaves the checks out (muse_bootguard_set_*reachable() below). */
static bool (*s_reachable)(void) __attribute__((unused));
static bool (*s_still_reachable)(void) __attribute__((unused));
static volatile bool s_healthy;

/* Both reachability checks only where the build asks for them (CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE): off, healthy
 * is uptime alone and nothing restarts the gadget for being out of reach, whoever calls these. */
void muse_bootguard_set_still_reachable(bool (*reachable_now)(void))
{
#if CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE
    s_still_reachable = reachable_now;
#else
    (void)reachable_now;
#endif
}

void muse_bootguard_set_reachable(bool (*reachable)(void))
{
#if CONFIG_MUSE_BOOTGUARD_BLE_REACHABLE
    s_reachable = reachable;
#else
    (void)reachable;
#endif
}

#if CONFIG_MUSE_BOOTGUARD
static void on_healthy(void *arg)
{
    (void)arg;
    if (s_reachable && !s_reachable()) {
        /* Up for 30 s but never advertised or connected (NimBLE didn't sync,
         * or advertising was held): not reachable, so not healthy. */
        if (s_mode == MUSE_BOOT_NORMAL) {
            ESP_LOGE(TAG, "boot NOT healthy: BLE never advertised in %d s; restarting (counts as a failed boot)",
                     MUSE_BOOTGUARD_HEALTHY_MS / 1000);
            esp_restart();   /* unmarked on purpose: the guard counts it */
        }
        ESP_LOGE(TAG, "SAFE mode and BLE never advertised: nothing further to escalate to");
        return;
    }
    muse_bootguard_rec_healthy(&s_rec);
    s_healthy = true;
    ESP_LOGI(TAG, "boot healthy (%d s up)", MUSE_BOOTGUARD_HEALTHY_MS / 1000);
}

static void on_stable(void *arg)
{
    (void)arg;
    muse_bootguard_rec_stable(&s_rec);
}
#endif

bool muse_bootguard_is_healthy(void)
{
#if CONFIG_MUSE_BOOTGUARD
    return s_healthy;
#else
    return true;
#endif
}

#define STAY_REACHABLE_S 30

void muse_bootguard_tick_reachable(void)
{
#if CONFIG_MUSE_BOOTGUARD
    static int quiet_s;
    static bool logged;
    if (!s_healthy || !s_still_reachable || s_still_reachable()) {
        quiet_s = 0;
        return;
    }
    if (++quiet_s < STAY_REACHABLE_S) return;
    if (s_mode == MUSE_BOOT_NORMAL) {
        muse_bootguard_rec_lost(&s_rec);   /* counted across boots: each of these boots got healthy */
        ESP_LOGE(TAG, "unreachable: no advertising and no link for %d s; restarting (%u in a row, SAFE at %d)",
                 STAY_REACHABLE_S, (unsigned)s_rec.lost, MUSE_BOOTGUARD_LOST);
        esp_restart();   /* unmarked on purpose: not a planned restart */
    }
    if (!logged) {
        logged = true;
        ESP_LOGE(TAG, "SAFE mode and unreachable for %d s: nothing further to escalate to", STAY_REACHABLE_S);
    }
#endif
}

void muse_bootguard_arm(void)
{
#if CONFIG_MUSE_BOOTGUARD
    static esp_timer_handle_t healthy, stable;
    if (healthy) {
        return;
    }
    const esp_timer_create_args_t h = { .callback = on_healthy, .name = "boot_ok" };
    const esp_timer_create_args_t s = { .callback = on_stable, .name = "boot_stable" };
    if (esp_timer_create(&h, &healthy) != ESP_OK || esp_timer_create(&s, &stable) != ESP_OK) {
        ESP_LOGE(TAG, "no timers: this boot can't count as healthy");
        return;
    }
    esp_timer_start_once(healthy, (uint64_t)MUSE_BOOTGUARD_HEALTHY_MS * 1000);
    esp_timer_start_once(stable, (uint64_t)MUSE_BOOTGUARD_STABLE_MS * 1000);
#endif
}

void muse_bootguard_log_state(void)
{
#if CONFIG_MUSE_BOOTGUARD
    /* On link.main: the BLE console's log mirror carries that tag. */
    ESP_LOGI("link.main", "guard: mode %s%s%s, healthy %d, failed %u, crashes %u, lost %u",
             s_mode == MUSE_BOOT_SAFE ? "SAFE" : "normal", s_mode == MUSE_BOOT_SAFE ? ": " : "",
             s_mode == MUSE_BOOT_SAFE ? muse_bootguard_why_name(s_why) : "", (int)s_healthy, (unsigned)s_rec.failed,
             (unsigned)s_rec.crashes, (unsigned)s_rec.lost);
#else
    ESP_LOGI("link.main", "guard: off in this build");
#endif
    char crash[200];   /* the last crash since power-on (muse_crashrec.h) */
    if (muse_crashrec_last(crash, sizeof(crash))) {
        ESP_LOGI("link.main", "guard: last crash: %s", crash);
    } else {
        ESP_LOGI("link.main", "guard: no crash since power-on");
    }
}

bool muse_bootguard_reenable(void)
{
    return muse_bootguard_reenable_from(true);
}

bool muse_bootguard_reenable_from(bool trusted)
{
#if CONFIG_MUSE_BOOTGUARD
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t erased = nvs_erase_key(h, NVS_OFF_KEY);
    bool committed = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    /* A keep-off record still there isn't "Muse on again" (the next boot reads it and stays SAFE). */
    bool record_gone = erased == ESP_ERR_NVS_NOT_FOUND || (erased == ESP_OK && committed);
    bool was = erased == ESP_OK;
    /* Recovery state moves only on what passed a gate. An unproven word (the Nordic UART's safe.off in SAFE, where
     * no token proof runs) still leaves SAFE, but the running-loss count stays: only the key hold, a proven word or
     * the USB console clears it. */
    if (!trusted) {
        if (muse_bootguard_rec_valid(&s_rec) && s_rec.lost) {
            ESP_LOGW(TAG, "safe.off from an unproven link: the loss count stays (%u lost)", (unsigned)s_rec.lost);
        }
    } else if (muse_bootguard_rec_valid(&s_rec) && s_rec.lost) {
        muse_bootguard_rec_stable(&s_rec);   /* the running-loss SAFE mode too */
        was = true;
    }
    if (!record_gone) {
        ESP_LOGE(TAG, "Muse's keep-off record couldn't be erased: the next boot stays SAFE");
        return false;
    }
    if (was) {
        ESP_LOGW(TAG, "Muse on again: the next boot is normal");
    }
    return was;
#else
    (void)trusted;
    return false;
#endif
}

bool muse_bootguard_console_word(const char *line, char *out, size_t n)
{
    return muse_bootguard_console_word_from(line, out, n, true);
}

bool muse_bootguard_console_word_from(const char *line, char *out, size_t n, bool trusted)
{
    if (!strcmp(line, "safe=0") || !strcmp(line, "safe.off")) {
        /* In SAFE: Muse on, then a planned restart 500 ms later (after the
         * answer reaches the console), as the key hold (muse_bootguard_leave_safe;
         * on the C6 arming alone needed a >restart after it). Outside
         * SAFE: the flag cleared if it was set, no restart. */
        bool was = muse_bootguard_reenable_from(trusted);
        bool safe = s_mode == MUSE_BOOT_SAFE;
        bool restart = safe && muse_restart_later(MUSE_RESTART_SAFE_OFF, -1, MUSE_BOOTGUARD_SAFE_OFF_RESTART_MS);
        snprintf(out, n, "safe=0 %s", restart ? "Muse on: restarting normally"
                                      : safe ? "Muse on from the next boot (restart failed: >restart)"
                                      : was ? "Muse on from the next boot" : "Muse wasn't kept off");
        ESP_LOGW(TAG, "%s (console)", out);
        return true;
    }
    if (!strcmp(line, "guard?")) {
        muse_bootguard_log_state();   /* on link.main */
        out[0] = '\0';
        return true;
    }
    return false;
}

#if CONFIG_MUSE_BOOTGUARD
static bool entry_window(void)
{
    return (uint64_t)esp_timer_get_time() / 1000 <= MUSE_BOOTGUARD_ENTRY_WINDOW_MS;
}

/* The key may still ask for a button SAFE: a normal boot, or a SAFE the guard entered by itself. */
static bool entry_allowed(void)
{
    return s_mode == MUSE_BOOT_NORMAL || (s_mode == MUSE_BOOT_SAFE && s_why != MUSE_BOOT_WHY_BUTTON);
}
#endif

bool muse_bootguard_entry_open(void)
{
#if CONFIG_MUSE_BOOTGUARD
    return entry_allowed() && entry_window();
#else
    return false;
#endif
}

bool muse_bootguard_enter_safe(const char *why)
{
#if CONFIG_MUSE_BOOTGUARD
    if (!entry_allowed()) {
        return false;
    }
    muse_bootguard_rec_button(&s_rec);
    ESP_LOGW(TAG, "%s: restarting into SAFE mode (the user's key hold)", why);
    vTaskDelay(pdMS_TO_TICKS(200));   /* the line reaches the log first */
    muse_restart(MUSE_RESTART_SAFE_ON);
    return true;
#else
    (void)why;
    return false;
#endif
}

bool muse_bootguard_leave_safe(const char *why)
{
    if (s_mode != MUSE_BOOT_SAFE) {
        return false;
    }
    muse_bootguard_reenable();
    ESP_LOGW(TAG, "leaving SAFE mode (%s): Muse on, restarting normally", why);
    vTaskDelay(pdMS_TO_TICKS(200));   /* the line reaches the log first */
    muse_restart(MUSE_RESTART_SAFE_OFF);
    return true;
}

#if CONFIG_MUSE_BOOTGUARD
typedef struct {
    bool (*key_down)(void);
    void (*cue)(void);
} safe_watch_t;

static void safe_watch_task(void *arg)
{
    safe_watch_t w = *(safe_watch_t *)arg;
    free(arg);
    muse_bootguard_keys_t k = { 0 };
    ESP_LOGW(TAG, "SAFE mode: hold the talk key %d s to restart normally", MUSE_BOOTGUARD_SAFE_HOLD_MS / 1000);
    for (;;) {
        int r = muse_bootguard_keys_step(&k, w.key_down(), (uint32_t)(esp_timer_get_time() / 1000));
        if (r == MUSE_BOOTGUARD_KEY_ENTER && s_why != MUSE_BOOT_WHY_BUTTON) {
            muse_bootguard_enter_safe("SAFE key held 10 s after boot");
        } else if (r == MUSE_BOOTGUARD_KEY_LEAVE) {
            if (w.cue) {
                w.cue();
            }
            muse_bootguard_leave_safe("talk key held");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

static volatile bool s_setup_press;   /* this normal boot's key press at power-on, let go early: setup mode */

bool muse_bootguard_setup_press(void)
{
    return s_setup_press;
}

#if CONFIG_MUSE_BOOTGUARD
static void entry_watch_task(void *arg)
{
    bool (*key_down)(void) = (bool (*)(void))arg;
    muse_bootguard_hold_t h = { 0 };
    while ((uint64_t)esp_timer_get_time() / 1000 <= MUSE_BOOTGUARD_ENTRY_WINDOW_MS + MUSE_BOOTGUARD_ENTRY_HOLD_MS + 100) {
        if (muse_bootguard_entry_step(&h, key_down(), (uint32_t)(esp_timer_get_time() / 1000))) {
            muse_bootguard_enter_safe("SAFE key held 10 s after boot");
        }
        if (h.released_early && !s_setup_press && s_mode == MUSE_BOOT_NORMAL) {
            s_setup_press = true;
            ESP_LOGI(TAG, "the key held at power-on and let go early: setup mode (a re-setup can be confirmed)");
        }
        if (h.fired) {
            break;   /* a press begun after the window: nothing more this boot */
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}
#endif

void muse_bootguard_entry_watch(bool (*key_down)(void))
{
#if CONFIG_MUSE_BOOTGUARD
    if (s_mode != MUSE_BOOT_NORMAL || !key_down || !entry_window()) {
        return;   /* SAFE's own watch takes the entry hold there (muse_bootguard_keys_step) */
    }
    /* Internal stack, like the SAFE watch: it may restart the chip. */
    if (xTaskCreate(entry_watch_task, "safe_entry", 2560, (void *)key_down, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no SAFE entry watch");
    }
#else
    (void)key_down;
#endif
}

void muse_bootguard_safe_watch(bool (*key_down)(void), void (*cue)(void))
{
#if CONFIG_MUSE_BOOTGUARD
    if (s_mode != MUSE_BOOT_SAFE || !key_down) {
        return;
    }
    safe_watch_t *w = malloc(sizeof(*w));
    if (!w) {
        ESP_LOGE(TAG, "SAFE mode: no memory for the key watch");
        return;
    }
    w->key_down = key_down;
    w->cue = cue;
    /* Internal stack: it restarts the chip (muse_restart maps nothing, but stays out of PSRAM like the console). */
    if (xTaskCreate(safe_watch_task, "safe_key", 2560, w, 2, NULL) != pdPASS) {
        free(w);
        ESP_LOGE(TAG, "SAFE mode: no key watch");
    }
#else
    (void)key_down;
    (void)cue;
#endif
}

/* ---- the gadget watchdog ---------------------------------------------------- */

#if CONFIG_MUSE_GADGET_WDT
static esp_task_wdt_user_handle_t s_users[MUSE_WDT_COUNT];
static const char *const USER_NAMES[MUSE_WDT_COUNT] = { "ble_host", "mg_worker", "muse_input", "link_session",
                                                        "muse_voice" };
#endif

void muse_wdt_add(muse_wdt_user_t u)
{
#if CONFIG_MUSE_GADGET_WDT
    if (u >= MUSE_WDT_COUNT || s_users[u]) {
        return;
    }
    if (esp_task_wdt_add_user(USER_NAMES[u], &s_users[u]) != ESP_OK) {
        ESP_LOGE(TAG, "watchdog: can't watch %s", USER_NAMES[u]);
        s_users[u] = NULL;
    }
#else
    (void)u;
#endif
}

void muse_wdt_feed(muse_wdt_user_t u)
{
#if CONFIG_MUSE_GADGET_WDT
    if (u >= MUSE_WDT_COUNT) {
        return;
    }
    if (!s_users[u]) {
        muse_wdt_add(u);   /* the first feed starts the watch */
    }
    if (s_users[u]) {
        esp_task_wdt_reset_user(s_users[u]);
    }
#else
    (void)u;
#endif
}

void muse_wdt_remove(muse_wdt_user_t u)
{
#if CONFIG_MUSE_GADGET_WDT
    if (u < MUSE_WDT_COUNT && s_users[u]) {
        esp_task_wdt_delete_user(s_users[u]);
        s_users[u] = NULL;
    }
#else
    (void)u;
#endif
}
