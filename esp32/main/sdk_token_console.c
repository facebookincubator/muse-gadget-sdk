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

#include "muse_bootguard.h"
#include "muse_local_press.h"
#include "muse_restart.h"
#include "sdk_token_console.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_app_desc.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "esp_wifi.h"
#include "identity.h"
#include "wifi_known.h"
#include "wifi_mgr.h"
#include "network_proxy_tls.h"
#if CONFIG_MUSE_ENABLED
#include "muse_link.h"
#endif
#include "token_fp.h"
#if CONFIG_HOMEHUB_VOICE && !CONFIG_MUSE_ENABLED
#include "voice.h"   // vol= (the bench volume)
#endif

// The chip's own USB port, where it has one and the log goes to it.
#define USJ_CONSOLE (SOC_USB_SERIAL_JTAG_SUPPORTED && \
                     (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG))
#if USJ_CONSOLE
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#endif

static const char *TAG = "link.sdk_token";

#define COMMAND "sdk_token="
// SDK-token or per-network proxy JSON command, including the leading '>'.
#define COMMAND_LINE_MAX 1024

static bool proxy_command(const char *line) {
    const char *prefix = "wifi_proxy=";
    if (strncmp(line, prefix, strlen(prefix))) return false;
    cJSON *root = cJSON_Parse(line + strlen(prefix));
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "proxy");
    network_proxy_config_t proxy = {0};
    const char *result = "error invalid";
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]
        || strlen(ssid->valuestring) > WIFI_KNOWN_SSID_MAX) goto done;
    if (object) {
        if (!network_proxy_parse(object, &proxy)) goto done;
#if CONFIG_MUSE_ENABLED
        result = muse_link_wifi_proxy_set(ssid->valuestring, &proxy) ? "saving" : "error busy_or_unknown_network";
#else
        bool ok = wifi_known_set_proxy(ssid->valuestring, &proxy);
        network_proxy_config_t check;
        ok = ok && wifi_known_get_proxy(ssid->valuestring, &check)
             && network_proxy_equal(&check, &proxy);
        network_proxy_wipe(&check, sizeof(check));
        result = ok ? "saved" : "error storage_or_unknown_network";
        wifi_config_t current;
        if (ok && esp_wifi_get_config(WIFI_IF_STA, &current) == ESP_OK
            && !strncmp((const char *)current.sta.ssid, ssid->valuestring, sizeof(current.sta.ssid))) {
            // The normal reconnect path picks up the stored proxy.
            network_proxy_set_active(&proxy);
        }
#endif
    } else {
#if CONFIG_MUSE_ENABLED
        int state = muse_link_wifi_proxy_get(ssid->valuestring, &proxy);
#else
        int state = wifi_known_get_proxy(ssid->valuestring, &proxy) ? 0 : -1;
#endif
        if (state < 0) { result = state == -2 ? "error storage" : "error unknown_network"; goto done; }
        // Status deliberately excludes user names and passwords.
        cJSON *status = cJSON_CreateObject();
        bool built = status && cJSON_AddStringToObject(status, "ssid", ssid->valuestring)
            && cJSON_AddStringToObject(status, "type", proxy.type == NETWORK_PROXY_OFF ? "off"
                : proxy.type == NETWORK_PROXY_HTTP ? "http" : proxy.type == NETWORK_PROXY_SOCKS5 ? "socks5" : "invalid")
            && cJSON_AddStringToObject(status, "host", proxy.host)
            && cJSON_AddNumberToObject(status, "port", proxy.port)
            && cJSON_AddBoolToObject(status, "authenticated", proxy.username[0] != 0)
            && cJSON_AddBoolToObject(status, "saving", state == 1);
        char *text = built ? cJSON_PrintUnformatted(status) : NULL;
        if (text) printf("@wifi_proxy %s\n", text);
        else printf("@wifi_proxy error memory\n");
        cJSON_free(text);
        cJSON_Delete(status);
        result = NULL;
    }
done:
    network_proxy_wipe(&proxy, sizeof(proxy));
    network_proxy_delete_json(root);
    if (result) printf("@wifi_proxy %s\n", result);
    fflush(stdout);
    return true;
}

bool sdk_token_console_command(const char *line) {
    if (proxy_command(line)) return true;
    if (!strcmp(line, "version")) {
        // The web flasher reads the version back before it says Done.
        printf("@version %s\n", esp_app_get_description()->version);
        fflush(stdout);
        return true;
    }
#if CONFIG_HOMEHUB_VOICE && !CONFIG_MUSE_ENABLED
    // The bench volume (RAM only); the Muse boards' console has its own.
    if (voice_volume_word(line)) return true;
#endif
    // hang=task|int (the watchdogs' hardware check: a task or interrupt watchdog reset
    // on purpose, muse_restart_test_hang). USB only: the Nordic UART doesn't
    // reach this console.
    if (!strncmp(line, "hang", 4) && (!line[4] || line[4] == '=')) {
        char out[80];
        muse_restart_hang_word(line, out, sizeof(out));
        printf("@%s\n", out);
        fflush(stdout);
        return true;
    }
    // safe=0 / safe.off (in SAFE: Muse on and a planned restart) and guard?.
    char safe[64];
    if (muse_bootguard_console_word(line, safe, sizeof(safe))) {
        if (safe[0]) {
            printf("@%s\n", safe);
            fflush(stdout);
        }
        return true;
    }
    if (strncmp(line, COMMAND, strlen(COMMAND)) != 0) return false;
    const char *token = line + strlen(COMMAND);
    if (!identity_sdk_token_valid(token)) {
        ESP_LOGW(TAG, "not an SDK token");
        printf("@sdk_token error invalid\n");
        fflush(stdout);
        return true;
    }
    if (!identity_set_sdk_token(token)) {
        ESP_LOGE(TAG, "couldn't store the SDK token");
        printf("@sdk_token error storage\n");
        fflush(stdout);
        return true;
    }
    char fp[TOKEN_FP_TEXT_LEN];
    ESP_LOGI(TAG, "stored the SDK token (%s), restarting to use it", token_fp_text(token, fp));
    printf("@sdk_token ok\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
    muse_restart(MUSE_RESTART_SETUP);
    return true;
}

// Collects bytes into a line and runs it on '\n' if it starts with '>'.
// Longer lines are dropped.
typedef struct {
    char buf[COMMAND_LINE_MAX];
    size_t len;
    bool overflow;
} line_reader_t;

static void feed(line_reader_t *r, char c) {
    if (c == '\r') return;
    if (c != '\n') {
        if (r->len < sizeof(r->buf) - 1) {
            r->buf[r->len++] = c;
        } else {
            r->overflow = true;
        }
        return;
    }
    r->buf[r->len] = '\0';
    // update.arm (the wired console's arm: this link updates with no press) only here, the wired console's line
    // reader: never a word another path relays.
    if (!r->overflow && r->buf[0] == '>' && !muse_local_press_console(r->buf + 1)) sdk_token_console_command(r->buf + 1);
    network_proxy_wipe(r->buf, sizeof(r->buf));
    r->len = 0;
    r->overflow = false;
}

#if CONFIG_ESP_CONSOLE_UART
static void uart_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

#if USJ_CONSOLE
static void usj_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

void sdk_token_console_start(void) {
#if CONFIG_ESP_CONSOLE_UART
    // With the driver installed, the log goes through it too, so it can't
    // interleave with what the driver sends.
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, COMMAND_LINE_MAX, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        xTaskCreate(uart_task, "sdk_token_uart", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no console UART; can't set the SDK token over it");
    }
#endif
#if USJ_CONSOLE
    // Firmware flashed before this one can leave the port's interrupts on,
    // and they outlast a reset: M5Stack's CoreMatrix factory firmware leaves
    // the 1 ms start-of-frame one on, which the driver doesn't handle, so its
    // handler would run without end and stop the boot. The driver turns on
    // the ones it uses.
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_LL_INTR_MASK);
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = COMMAND_LINE_MAX;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        xTaskCreate(usj_task, "sdk_token_usj", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no USB Serial/JTAG driver; can't set the SDK token over it");
    }
#endif
}
