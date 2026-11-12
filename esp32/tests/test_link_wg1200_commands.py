# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ESP32_DIR = Path(__file__).resolve().parents[1]
MAIN_DIR = ESP32_DIR / "main"
MUSE_DIR = ESP32_DIR / "components" / "muse"
CJSON_DIR = Path(
    os.environ.get(
        "CJSON_SOURCE_DIR",
        ESP32_DIR / "managed_components/espressif__cjson/cJSON",
    )
)


def extract_c_block(source: str, anchor: str) -> str:
    start = source.index(anchor)
    brace = source.index("{", start)
    depth = 0
    in_string = False
    escape = False
    i = brace
    while i < len(source):
        ch = source[i]
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
        else:
            if ch == '"':
                in_string = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return source[start : i + 1]
        i += 1
    raise ValueError(f"Unclosed block for {anchor}")


def extract_c_span(source: str, start_anchor: str, end_anchor: str) -> str:
    start = source.index(start_anchor)
    end = source.index(end_anchor, start)
    return source[start:end]


class LinkWg1200CommandsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if not cxx or not cc or not (CJSON_DIR / "cJSON.c").exists():
            raise unittest.SkipTest("Host C/C++ compiler or cJSON not available")

        noise_src = (MAIN_DIR / "noise_control.cpp").read_text(encoding="utf-8")
        app_src = (MAIN_DIR / "app.c").read_text(encoding="utf-8")
        weather_src = (MUSE_DIR / "wg1200_weather.c").read_text(encoding="utf-8")

        string_param_fn = extract_c_block(noise_src, "static cJSON *string_param(")
        int_param_fn = extract_c_block(noise_src, "static cJSON *int_param(")
        row_params_fn = extract_c_block(noise_src, "static cJSON *row_params(")
        add_command_fn = extract_c_block(noise_src, "static void add_command(")
        add_meta_fn = extract_c_block(
            noise_src, "static void add_register_metadata_string("
        )
        build_register_fn = extract_c_block(
            noise_src, "static char *build_register_json("
        )
        command_error_fn = extract_c_block(app_src, "static cJSON *command_error(")
        wg1200_notify_fn = extract_c_block(
            app_src, "static cJSON *wg1200_notify_command("
        )
        on_ws_command_fn = extract_c_block(app_src, "static cJSON *on_ws_command(")

        wg1200_branch_match = re.search(
            r"#if CONFIG_MUSE_BOARD_WXM_WG1200\s+if \(strcmp\(command, \"sensors\.read\"\).*?#endif",
            on_ws_command_fn,
            flags=re.DOTALL,
        )
        assert wg1200_branch_match is not None, "WG1200 branch missing in on_ws_command"
        wg1200_branch = wg1200_branch_match.group(0)

        region_tables_span = extract_c_span(
            weather_src,
            "static const uint32_t k_region_freqs[WG1200_REGION_COUNT]",
            "/* ── BMP390 Implementation",
        )
        rain_types_span = extract_c_span(
            weather_src,
            "#define WS_DATA_PRECIP_RATE_DECAY_TIMEOUT_SEC",
            "static void reset_precip_rate_params_locked(void)",
        )
        sx_wait_busy_fn = extract_c_block(weather_src, "static bool sx1262_wait_busy(")
        sx_cs_select_fn = extract_c_block(weather_src, "static bool sx1262_cs_select(")
        sx_cs_deselect_fn = extract_c_block(
            weather_src, "static void sx1262_cs_deselect("
        )
        sx_read_cmd_fn = extract_c_block(weather_src, "static void sx1262_read_cmd(")
        get_precip_rate_fn = extract_c_block(
            weather_src, "static float get_precip_rate_locked("
        )
        init_rain_fn = extract_c_block(
            weather_src, "static void init_rain_tracking_state("
        )
        load_region_fn = extract_c_block(
            weather_src, "static void load_initial_region("
        )
        sync_factory_nvs_fn = extract_c_block(
            weather_src, "static void sync_factory_nvs_readonly("
        )
        get_state_fn = extract_c_block(weather_src, "void wg1200_weather_get_state(")
        add_metric_fn = extract_c_block(weather_src, "static void add_sensor_metric(")
        sensors_cmd_fn = extract_c_block(
            weather_src, "cJSON *wg1200_weather_sensors_command("
        )
        boot_err_fn = extract_c_block(weather_src, "static cJSON *boot_cmd_error(")
        boot_cmd_fn = extract_c_block(
            weather_src, "cJSON *wg1200_boot_weatherxm_command("
        )

        cls._tmpdir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._tmpdir.cleanup)
        out_dir = Path(cls._tmpdir.name)

        (out_dir / "esp_err.h").write_text(
            """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define ESP_ERR_NVS_NOT_FOUND 0x1102
""",
            encoding="utf-8",
        )
        (out_dir / "esp_partition.h").write_text(
            """#pragma once
#include <stdint.h>
typedef struct {
    char label[17];
    uint32_t address;
    uint32_t size;
} esp_partition_t;
""",
            encoding="utf-8",
        )

        weather_extract_c = f"""
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wg1200_weather.h"
#include "wxm_boot_mgr.h"

#define RTC_NOINIT_ATTR
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFU
#define WG1200_EXP_LORA_NSS 0
#define WG1200_EXP_LORA_BUSY 2

typedef void *SemaphoreHandle_t;
typedef uint32_t nvs_handle_t;
typedef enum {{ NVS_READONLY = 0, NVS_READWRITE = 1 }} nvs_open_mode_t;

static const char *TAG = "wg1200_weather_test";
static SemaphoreHandle_t s_state_mutex = NULL;
static wg1200_weather_state_t s_state = {{
    .region = WG1200_REGION_EU868,
    .freq_hz = 868000000UL,
}};

static int64_t s_now_us = 10000000LL;
static int64_t esp_timer_get_time(void) {{ return s_now_us; }}
static void vTaskDelay(uint32_t ticks) {{ (void)ticks; }}
static void xSemaphoreTake(SemaphoreHandle_t m, uint32_t t) {{ (void)m; (void)t; }}
static void xSemaphoreGive(SemaphoreHandle_t m) {{ (void)m; }}

/* Simulated NVS state for muse_nvs vs stock WeatherXM "nvs" */
static bool s_stock_nvs_mounted = false;
static int s_stock_nvs_init_calls = 0;
static int s_stock_nvs_deinit_calls = 0;
static bool s_muse_has_region = false;
static uint8_t s_muse_region_u8 = 0;
static char s_stock_freq_name[16] = "US915";
static int64_t s_stock_ws_id = 0x1234ABCDLL;
static uint32_t s_stock_tz_u32 = (uint32_t)(-5);
static bool s_stock_has_daily = true;

#define HANDLE_MUSE_NVS 10U
#define HANDLE_STOCK_NVS 20U

esp_err_t __real_nvs_flash_init_partition(const char *partition_label) {{
    if (!partition_label || strcmp(partition_label, "nvs") != 0) {{
        return ESP_ERR_INVALID_ARG;
    }}
    s_stock_nvs_init_calls++;
    s_stock_nvs_mounted = true;
    return ESP_OK;
}}

esp_err_t nvs_flash_deinit_partition(const char *partition_label) {{
    if (!partition_label || strcmp(partition_label, "nvs") != 0) {{
        return ESP_ERR_INVALID_ARG;
    }}
    s_stock_nvs_deinit_calls++;
    s_stock_nvs_mounted = false;
    return ESP_OK;
}}

esp_err_t __real_nvs_open_from_partition(const char *part_name,
                                         const char *namespace_name,
                                         nvs_open_mode_t open_mode,
                                         nvs_handle_t *out_handle) {{
    if (!part_name || strcmp(part_name, "nvs") != 0 ||
        !namespace_name || strcmp(namespace_name, "settings") != 0 ||
        open_mode != NVS_READONLY) {{
        return ESP_ERR_INVALID_ARG;
    }}
    if (!s_stock_nvs_mounted) {{
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }}
    *out_handle = HANDLE_STOCK_NVS;
    return ESP_OK;
}}

static esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {{
    (void)mode;
    if (!ns || strcmp(ns, "wxm_weather") != 0) return ESP_ERR_NVS_NOT_FOUND;
    *out = HANDLE_MUSE_NVS;
    return ESP_OK;
}}

static void nvs_close(nvs_handle_t h) {{ (void)h; }}

static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {{
    if (h == HANDLE_MUSE_NVS && strcmp(key, "region") == 0 && s_muse_has_region) {{
        *out = s_muse_region_u8;
        return ESP_OK;
    }}
    return ESP_ERR_NVS_NOT_FOUND;
}}

static esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len) {{
    if (h == HANDLE_STOCK_NVS && s_stock_nvs_mounted && strcmp(key, "freq_name") == 0) {{
        snprintf(out, *len, "%s", s_stock_freq_name);
        *len = strlen(out) + 1;
        return ESP_OK;
    }}
    return ESP_ERR_NVS_NOT_FOUND;
}}

static esp_err_t nvs_get_i64(nvs_handle_t h, const char *key, int64_t *out) {{
    if (h == HANDLE_STOCK_NVS && s_stock_nvs_mounted && strcmp(key, "ws_id") == 0) {{
        *out = s_stock_ws_id;
        return ESP_OK;
    }}
    return ESP_ERR_NVS_NOT_FOUND;
}}

static esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {{
    if (h == HANDLE_STOCK_NVS && s_stock_nvs_mounted && strcmp(key, "tz_offset") == 0) {{
        *out = s_stock_tz_u32;
        return ESP_OK;
    }}
    return ESP_ERR_NVS_NOT_FOUND;
}}

/* Simulated SX1262 CS / SPI state */
static bool s_sim_nss_timeout = false;
static int s_spi_tx_calls = 0;
static int s_spi_rx_calls = 0;

static int wg1200_io_get_level(uint8_t pin) {{
    (void)pin;
    return 0; /* BUSY low */
}}

static esp_err_t wg1200_io_set_level(uint8_t pin, uint32_t level) {{
    if (pin == WG1200_EXP_LORA_NSS && level == 0 && s_sim_nss_timeout) {{
        return ESP_ERR_TIMEOUT;
    }}
    return ESP_OK;
}}

static esp_err_t sx1262_spi_tx(const uint8_t *tx, size_t len) {{
    (void)tx;
    (void)len;
    s_spi_tx_calls++;
    return ESP_OK;
}}

static esp_err_t sx1262_spi_rx(uint8_t *rx, size_t len) {{
    s_spi_rx_calls++;
    if (rx && len > 0) {{
        memset(rx, 0xA5, len);
    }}
    return ESP_OK;
}}

/* Simulated partition boot manager state */
static esp_partition_t s_part_factory = {{ "factory", 0x10000, 0x3F0000 }};
static esp_partition_t s_part_ota0 = {{ "ota_0", 0x400000, 0x3F0000 }};
static bool s_sim_factory_present = true;
static bool s_sim_ota0_present = true;
static int s_boot_calls = 0;
static char s_booted_label[32] = {{0}};

void wxm_boot_get_info(wxm_boot_info_t *out) {{
    memset(out, 0, sizeof(*out));
    if (s_sim_factory_present) {{
        out->factory_part = &s_part_factory;
        snprintf(out->factory_project, sizeof(out->factory_project), "wg1200-firmware");
        snprintf(out->factory_version, sizeof(out->factory_version), "0.1.9");
    }}
    if (s_sim_ota0_present) {{
        out->ota0_part = &s_part_ota0;
        snprintf(out->ota0_project, sizeof(out->ota0_project), "wg1200-firmware");
        snprintf(out->ota0_version, sizeof(out->ota0_version), "0.2.4");
    }}
    if (out->ota0_part) {{
        out->wxm_part = out->ota0_part;
        snprintf(out->wxm_label, sizeof(out->wxm_label), "ota_0");
        snprintf(out->wxm_project, sizeof(out->wxm_project), "%s", out->ota0_project);
        snprintf(out->wxm_version, sizeof(out->wxm_version), "%s", out->ota0_version);
        if (out->factory_part) {{
            out->alt_part = out->factory_part;
            snprintf(out->alt_label, sizeof(out->alt_label), "factory");
            snprintf(out->alt_project, sizeof(out->alt_project), "%s", out->factory_project);
            snprintf(out->alt_version, sizeof(out->alt_version), "%s", out->factory_version);
        }}
    }} else if (out->factory_part) {{
        out->wxm_part = out->factory_part;
        snprintf(out->wxm_label, sizeof(out->wxm_label), "factory");
        snprintf(out->wxm_project, sizeof(out->wxm_project), "%s", out->factory_project);
        snprintf(out->wxm_version, sizeof(out->wxm_version), "%s", out->factory_version);
    }}
}}

esp_err_t wxm_boot_to_partition(const esp_partition_t *part) {{
    if (!part) return ESP_ERR_INVALID_ARG;
    s_boot_calls++;
    snprintf(s_booted_label, sizeof(s_booted_label), "%s", part->label);
    return ESP_OK;
}}

{region_tables_span}
{rain_types_span}

static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len) {{
    if (h == HANDLE_STOCK_NVS && s_stock_nvs_mounted && s_stock_has_daily &&
        strcmp(key, "wx_daily") == 0 && *len >= sizeof(daily_precip_nvs_blob_t)) {{
        daily_precip_nvs_blob_t b = {{
            .magic = DAILY_PRECIP_NVS_MAGIC,
            .day_idx = 20405,
            .station_id = 0x1234ABCDU,
            .baseline_cml = 10.0f,
            .accum_rollover = 0.0f,
            .cached = 2.5f,
        }};
        memcpy(out, &b, sizeof(b));
        *len = sizeof(b);
        return ESP_OK;
    }}
    return ESP_ERR_NVS_NOT_FOUND;
}}

{sx_wait_busy_fn}
{sx_cs_select_fn}
{sx_cs_deselect_fn}
{sx_read_cmd_fn}
{get_precip_rate_fn}
{init_rain_fn}
{load_region_fn}
{sync_factory_nvs_fn}
{get_state_fn}
{add_metric_fn}
{sensors_cmd_fn}
{boot_err_fn}
{boot_cmd_fn}

void test_weather_seed_valid_readings(void) {{
    memset(&s_state, 0, sizeof(s_state));
    s_state.region = WG1200_REGION_US915;
    s_state.freq_hz = 915000000UL;
    s_state.sx1262_detected = true;
    s_state.rx_packets_total = 12;
    s_state.rx_packets_valid = 11;
    s_state.bmp390_valid = true;
    s_state.indoor_temp_c = 32.4f;
    s_state.indoor_pressure_hpa = 1004.5f;
    s_state.indoor_updated_us = 8000000LL;
    s_state.outdoor_valid = true;
    snprintf(s_state.station_model, sizeof(s_state.station_model), "WS1001");
    s_state.station_id = 0x1234ABCDU;
    s_state.outdoor_temp_c = 19.2f;
    s_state.outdoor_humidity_pct = 64.0f;
    s_state.wind_speed_ms = 2.1f;
    s_state.wind_gust_ms = 4.8f;
    s_state.wind_dir_deg = 215;
    s_state.rain_rate_mm_h = 0.0f;
    s_state.rain_daily_mm = 1.2f;
    s_state.rain_accum_mm = 42.0f;
    s_state.uv_index = 3.4f;
    s_state.illuminance_lux = 18500;
    s_state.solar_rad_wm2 = 146.0f;
    s_state.battery_mv = 3120;
    s_state.rssi_dbm = -74;
    s_state.snr_db = 9;
    s_state.outdoor_updated_us = 5000000LL;
}}

void test_weather_clear_readings(void) {{
    memset(&s_state, 0, sizeof(s_state));
    s_state.region = WG1200_REGION_EU868;
    s_state.freq_hz = 868000000UL;
}}

void test_boot_configure(bool factory_ok, bool ota0_ok) {{
    s_sim_factory_present = factory_ok;
    s_sim_ota0_present = ota0_ok;
    s_boot_calls = 0;
    s_booted_label[0] = '\\0';
}}

int test_boot_calls(void) {{ return s_boot_calls; }}
const char *test_booted_label(void) {{ return s_booted_label; }}

cJSON *test_run_factory_nvs_sync(void) {{
    (void)s_paired_station_last_seen_us;
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_rain_rtc, 0, sizeof(s_rain_rtc));
    s_paired_station_id = 0;
    s_tz_offset_hours = 0;
    s_stock_nvs_mounted = false;
    s_stock_nvs_init_calls = 0;
    s_stock_nvs_deinit_calls = 0;

    sync_factory_nvs_readonly();

    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "init_calls", s_stock_nvs_init_calls);
    cJSON_AddNumberToObject(res, "deinit_calls", s_stock_nvs_deinit_calls);
    cJSON_AddBoolToObject(res, "still_mounted", s_stock_nvs_mounted);
    cJSON_AddStringToObject(res, "region", wg1200_weather_region_name(s_state.region));
    cJSON_AddNumberToObject(res, "freq_hz", s_state.freq_hz);
    cJSON_AddNumberToObject(res, "paired_station_id", (double)s_paired_station_id);
    cJSON_AddNumberToObject(res, "tz_offset_hours", s_tz_offset_hours);
    cJSON_AddNumberToObject(res, "daily_rain_mm", s_rain_rtc.daily_precip_cached);
    return res;
}}

cJSON *test_run_sx1262_cs_guard(void) {{
    uint8_t irq_buf[2] = {{0xFF, 0xFF}};
    s_sim_nss_timeout = true;
    s_spi_tx_calls = 0;
    s_spi_rx_calls = 0;
    sx1262_read_cmd(0x12, irq_buf, sizeof(irq_buf));
    int timeout_tx = s_spi_tx_calls;
    int timeout_rx = s_spi_rx_calls;
    int timeout_zeroed = (irq_buf[0] == 0 && irq_buf[1] == 0) ? 1 : 0;

    s_sim_nss_timeout = false;
    sx1262_read_cmd(0x12, irq_buf, sizeof(irq_buf));

    cJSON *res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "timeout_tx", timeout_tx);
    cJSON_AddNumberToObject(res, "timeout_rx", timeout_rx);
    cJSON_AddBoolToObject(res, "timeout_zeroed", timeout_zeroed != 0);
    cJSON_AddNumberToObject(res, "ok_tx", s_spi_tx_calls);
    cJSON_AddNumberToObject(res, "ok_rx", s_spi_rx_calls);
    cJSON_AddNumberToObject(res, "ok_byte0", irq_buf[0]);
    return res;
}}
"""

        harness_cpp = f"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern "C" {{
#include "cJSON.h"
#include "wg1200_weather.h"
void test_weather_seed_valid_readings(void);
void test_weather_clear_readings(void);
void test_boot_configure(bool factory_ok, bool ota0_ok);
int test_boot_calls(void);
const char *test_booted_label(void);
cJSON *test_run_factory_nvs_sync(void);
cJSON *test_run_sx1262_cs_guard(void);
}}

#define CONFIG_MUSE_BOARD_WXM_WG1200 1
#define CONFIG_HOMEHUB_DISPLAY_COMMANDS 1
#define CONFIG_HOMEHUB_LED_BACKEND_MUSE 1
#define CONFIG_HOMEHUB_TUNNEL 1
#define MUSE_DEVICE_FAMILY "gadget"
#define CONFIG_MUSE_DEVICE_MODEL_ID "muse-gadget-wxm-wg1200"
#define CONFIG_MUSE_DEVICE_MODEL_NAME "WeatherXM WG1200"

typedef uint32_t noise_ctrl_session_generation_t;
typedef struct {{
    const char *version;
}} esp_app_desc_t;

static const char *s_node_id = "homelink-a1b2c3";
static const char *s_display_name = "WeatherXM WG1200";
static char s_wifi_ssid[33] = "WeatherNet";
static char s_register_req_id[40] = {{0}};
static bool s_speaker_on = true;
static bool s_ui_ready = true;
static int s_last_beep_ms = 0;
static int s_last_duration_s = -1;
static char s_last_title[128] = {{0}};
static char s_last_msg[256] = {{0}};

static void copy_wifi_ssid(char *dst, size_t cap) {{
    snprintf(dst, cap, "%s", s_wifi_ssid);
}}
static void make_uuid(char *dst, size_t cap) {{
    snprintf(dst, cap, "00000000-0000-4000-8000-000000a1b2c3");
}}
static const esp_app_desc_t *esp_app_get_description(void) {{
    static const esp_app_desc_t desc = {{ "0.1.2-wg1200" }};
    return &desc;
}}
static bool led_status_display_info(int *w, int *h) {{
    *w = 480;
    *h = 480;
    return true;
}}
static int led_status_display_bits(void) {{ return 16; }}
static bool ota_is_enabled(void) {{ return true; }}

static bool muse_settings_speaker_on(void) {{ return s_speaker_on; }}
static void wg1200_buzzer_beep(uint32_t duration_ms) {{ s_last_beep_ms = (int)duration_ms; }}
static bool muse_ui_notify_show(const char *title, const char *message, int duration_s) {{
    if (!s_ui_ready) return false;
    snprintf(s_last_title, sizeof(s_last_title), "%s", title ? title : "");
    snprintf(s_last_msg, sizeof(s_last_msg), "%s", message ? message : "");
    s_last_duration_s = duration_s;
    return true;
}}

{string_param_fn}
{int_param_fn}
{row_params_fn}
{add_command_fn}
{add_meta_fn}
{build_register_fn}
{command_error_fn}
{wg1200_notify_fn}

static cJSON *dispatch_wg1200_command(const char *command, cJSON *params) {{
    {wg1200_branch}
    return command_error("unknown_command", command);
}}

int main(int argc, char **argv) {{
    if (argc < 2) return 2;
    if (strcmp(argv[1], "register") == 0) {{
        char *s = build_register_json();
        puts(s ? s : "{{}}");
        free(s);
        return 0;
    }}
    if (strcmp(argv[1], "factory_nvs") == 0) {{
        cJSON *res = test_run_factory_nvs_sync();
        char *out = cJSON_PrintUnformatted(res);
        puts(out ? out : "{{}}");
        free(out);
        cJSON_Delete(res);
        return 0;
    }}
    if (strcmp(argv[1], "sx1262_cs") == 0) {{
        cJSON *res = test_run_sx1262_cs_guard();
        char *out = cJSON_PrintUnformatted(res);
        puts(out ? out : "{{}}");
        free(out);
        cJSON_Delete(res);
        return 0;
    }}
    if (strcmp(argv[1], "invoke") == 0 && argc >= 4) {{
        test_weather_seed_valid_readings();
        test_boot_configure(true, true);
        if (argc >= 5) {{
            if (strcmp(argv[4], "muted") == 0) {{
                s_speaker_on = false;
            }} else if (strcmp(argv[4], "no_readings") == 0) {{
                test_weather_clear_readings();
            }} else if (strcmp(argv[4], "no_factory") == 0) {{
                test_boot_configure(false, true);
            }} else if (strcmp(argv[4], "no_partitions") == 0) {{
                test_boot_configure(false, false);
            }}
        }}
        cJSON *params = cJSON_Parse(argv[3]);
        cJSON *res = dispatch_wg1200_command(argv[2], params);
        cJSON_AddNumberToObject(res, "_beep_called", s_last_beep_ms);
        cJSON_AddStringToObject(res, "_ui_title", s_last_title);
        cJSON_AddStringToObject(res, "_ui_msg", s_last_msg);
        cJSON_AddNumberToObject(res, "_boot_calls", test_boot_calls());
        cJSON_AddStringToObject(res, "_booted_label", test_booted_label());
        char *out = cJSON_PrintUnformatted(res);
        puts(out ? out : "{{}}");
        free(out);
        cJSON_Delete(res);
        cJSON_Delete(params);
        return 0;
    }}
    return 2;
}}
"""
        cls.bin_path = out_dir / "test_wg1200_harness"
        cjson_obj = out_dir / "cjson.o"
        weather_obj = out_dir / "weather_extract.o"
        weather_c_path = out_dir / "weather_extract.c"
        src_path = out_dir / "harness.cpp"
        weather_c_path.write_text(weather_extract_c, encoding="utf-8")
        src_path.write_text(harness_cpp, encoding="utf-8")

        subprocess.run(
            [
                cc,
                "-std=c11",
                "-O2",
                f"-I{CJSON_DIR}",
                "-c",
                str(CJSON_DIR / "cJSON.c"),
                "-o",
                str(cjson_obj),
            ],
            check=True,
        )
        subprocess.run(
            [
                cc,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{out_dir}",
                f"-I{MUSE_DIR}",
                f"-I{CJSON_DIR}",
                "-c",
                str(weather_c_path),
                "-o",
                str(weather_obj),
            ],
            check=True,
        )
        subprocess.run(
            [
                cxx,
                "-std=c++17",
                "-O2",
                f"-I{out_dir}",
                f"-I{MUSE_DIR}",
                f"-I{CJSON_DIR}",
                str(src_path),
                str(weather_obj),
                str(cjson_obj),
                "-o",
                str(cls.bin_path),
            ],
            check=True,
        )

    def run_harness(self, *args: str) -> dict:
        proc = subprocess.run(
            [str(self.bin_path), *args],
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(proc.stdout.strip())

    def test_register_advertises_wg1200_and_display_commands(self) -> None:
        reg = self.run_harness("register")
        self.assertEqual(reg["method"], "link.register")
        self.assertEqual(reg["params"]["device_family"], "gadget")
        self.assertEqual(reg["params"]["model_id"], "muse-gadget-wxm-wg1200")
        self.assertEqual(reg["params"]["metadata"]["model_name"], "WeatherXM WG1200")
        commands = reg["params"]["commands_v2"]
        self.assertIn("device.guide", commands)
        self.assertIn("device.discover", commands)
        self.assertNotIn("lan.fetch", commands)

        self.assertIn("sensors.read", commands)
        self.assertIn("BMP390", commands["sensors.read"]["description"])
        self.assertIn("WS1001/WS1300", commands["sensors.read"]["description"])
        self.assertIn("gateway_temperature", commands["sensors.read"]["description"])
        self.assertIn("barometric_pressure", commands["sensors.read"]["description"])
        self.assertIn("outdoor_temperature", commands["sensors.read"]["description"])
        self.assertIn("wind_gust", commands["sensors.read"]["description"])
        self.assertIn("rain_rate", commands["sensors.read"]["description"])
        self.assertIn("rain_daily", commands["sensors.read"]["description"])

        self.assertIn("device.boot_weatherxm", commands)
        self.assertIn("slot", commands["device.boot_weatherxm"]["optional"])
        self.assertEqual(
            commands["device.boot_weatherxm"]["optional"]["slot"]["type"], "string"
        )

        self.assertIn("wg1200.notify", commands)
        notify_cmd = commands["wg1200.notify"]
        self.assertIn("message", notify_cmd["required"])
        self.assertEqual(notify_cmd["required"]["message"]["type"], "string")
        self.assertIn("title", notify_cmd["optional"])
        self.assertEqual(notify_cmd["optional"]["title"]["type"], "string")
        self.assertIn("beep_ms", notify_cmd["optional"])
        self.assertEqual(notify_cmd["optional"]["beep_ms"]["type"], "integer")
        self.assertIn("duration_s", notify_cmd["optional"])
        self.assertEqual(notify_cmd["optional"]["duration_s"]["type"], "integer")

        self.assertIn("display.draw_url", commands)
        self.assertIn("480x480", commands["display.draw_url"]["description"])
        self.assertIn("display.draw_file", commands)
        self.assertIn("display.show_animation", commands)

    def test_dispatch_sensors_read_and_no_readings_envelope(self) -> None:
        empty = self.run_harness("invoke", "sensors.read", "{}", "no_readings")
        self.assertFalse(empty["ok"])
        self.assertEqual(empty["error"]["code"], "no_readings")
        self.assertNotIn("payload", empty)

        res = self.run_harness("invoke", "sensors.read", "{}")
        self.assertTrue(res["ok"])
        payload = res["payload"]
        self.assertEqual(payload["gateway_temperature"]["value"], 32.4)
        self.assertEqual(payload["gateway_temperature"]["unit"], "C")
        self.assertEqual(payload["barometric_pressure"]["value"], 1004.5)
        self.assertEqual(payload["barometric_pressure"]["unit"], "hPa")
        self.assertEqual(payload["outdoor_temperature"]["value"], 19.2)
        self.assertEqual(payload["wind_gust"]["value"], 4.8)
        self.assertEqual(payload["rain_rate"]["value"], 0.0)
        self.assertEqual(payload["rain_daily"]["value"], 1.2)
        self.assertEqual(payload["lora_radio"]["region"], "US 915 MHz")
        self.assertEqual(payload["lora_radio"]["station_model"], "WS1001")
        self.assertEqual(payload["lora_radio"]["station_id"], "0x1234ABCD")

    def test_dispatch_wg1200_notify_and_validation(self) -> None:
        err = self.run_harness("invoke", "wg1200.notify", "{}")
        self.assertEqual(err["error"]["code"], "missing_param")

        bad_beep = self.run_harness(
            "invoke", "wg1200.notify", json.dumps({"message": "Hi", "beep_ms": 2000})
        )
        self.assertEqual(bad_beep["error"]["code"], "invalid_params")

        ok = self.run_harness(
            "invoke",
            "wg1200.notify",
            json.dumps(
                {
                    "title": "Wind Alert",
                    "message": "Gusts reaching 14 m/s",
                    "beep_ms": 250,
                    "duration_s": 45,
                }
            ),
        )
        self.assertTrue(ok["ok"])
        self.assertEqual(ok["title"], "Wind Alert")
        self.assertEqual(ok["beep_ms"], 250)
        self.assertEqual(ok["duration_s"], 45)
        self.assertEqual(ok["_beep_called"], 250)
        self.assertEqual(ok["_ui_title"], "Wind Alert")
        self.assertEqual(ok["_ui_msg"], "Gusts reaching 14 m/s")

        muted = self.run_harness(
            "invoke",
            "wg1200.notify",
            json.dumps({"message": "Silent update", "beep_ms": 200}),
            "muted",
        )
        self.assertTrue(muted["ok"])
        self.assertTrue(muted["muted"])
        self.assertEqual(muted["beep_ms"], 0)
        self.assertEqual(muted["_beep_called"], 0)

    def test_dispatch_boot_weatherxm_slots_and_validation(self) -> None:
        default_res = self.run_harness("invoke", "device.boot_weatherxm", "{}")
        self.assertTrue(default_res["ok"])
        self.assertEqual(default_res["boot_target"], "ota_0")
        self.assertEqual(default_res["payload"]["wxm_label"], "ota_0")
        self.assertEqual(default_res["payload"]["wxm_version"], "0.2.4")
        self.assertEqual(default_res["_boot_calls"], 1)
        self.assertEqual(default_res["_booted_label"], "ota_0")

        factory_res = self.run_harness(
            "invoke", "device.boot_weatherxm", json.dumps({"slot": "factory"})
        )
        self.assertTrue(factory_res["ok"])
        self.assertEqual(factory_res["boot_target"], "factory")
        self.assertEqual(factory_res["payload"]["wxm_label"], "factory")
        self.assertEqual(factory_res["payload"]["wxm_version"], "0.1.9")
        self.assertEqual(factory_res["_boot_calls"], 1)
        self.assertEqual(factory_res["_booted_label"], "factory")

        bad_slot = self.run_harness(
            "invoke", "device.boot_weatherxm", json.dumps({"slot": "ota_1"})
        )
        self.assertFalse(bad_slot["ok"])
        self.assertEqual(bad_slot["error"]["code"], "invalid_params")
        self.assertEqual(bad_slot["_boot_calls"], 0)

        bad_type = self.run_harness(
            "invoke", "device.boot_weatherxm", json.dumps({"slot": 123})
        )
        self.assertFalse(bad_type["ok"])
        self.assertEqual(bad_type["error"]["code"], "invalid_params")
        self.assertEqual(bad_type["_boot_calls"], 0)

        missing_factory = self.run_harness(
            "invoke",
            "device.boot_weatherxm",
            json.dumps({"slot": "factory"}),
            "no_factory",
        )
        self.assertFalse(missing_factory["ok"])
        self.assertEqual(missing_factory["error"]["code"], "unavailable")
        self.assertEqual(missing_factory["_boot_calls"], 0)

        missing_all = self.run_harness(
            "invoke", "device.boot_weatherxm", "{}", "no_partitions"
        )
        self.assertFalse(missing_all["ok"])
        self.assertEqual(missing_all["error"]["code"], "unavailable")
        self.assertEqual(missing_all["_boot_calls"], 0)

    def test_factory_nvs_mounts_reads_and_unmounts_cleanly(self) -> None:
        res = self.run_harness("factory_nvs")
        self.assertEqual(res["init_calls"], 1)
        self.assertEqual(res["deinit_calls"], 1)
        self.assertFalse(res["still_mounted"])
        self.assertEqual(res["region"], "US 915 MHz")
        self.assertEqual(res["freq_hz"], 915000000)
        self.assertEqual(int(res["paired_station_id"]), 0x1234ABCD)
        self.assertEqual(res["tz_offset_hours"], -5)
        self.assertAlmostEqual(res["daily_rain_mm"], 2.5)

    def test_sx1262_aborts_spi_transfer_when_nss_assert_fails(self) -> None:
        res = self.run_harness("sx1262_cs")
        self.assertEqual(res["timeout_tx"], 0)
        self.assertEqual(res["timeout_rx"], 0)
        self.assertTrue(res["timeout_zeroed"])
        self.assertEqual(res["ok_tx"], 1)
        self.assertEqual(res["ok_rx"], 1)
        self.assertEqual(res["ok_byte0"], 0xA5)

    def test_board_is_wired_into_the_lists(self) -> None:
        muse = ESP32_DIR / "components" / "muse"
        self.assertIn("MUSE_BOARD_WXM_WG1200", (muse / "Kconfig").read_text())
        self.assertIn("boards/board_wxm_wg1200.c", (muse / "CMakeLists.txt").read_text())
        self.assertIn("wg1200)  profile=wxm-wg1200", (ESP32_DIR / "tools" / "muse" / "board.sh").read_text())
        overlay = (ESP32_DIR / "devices" / "sdkconfig.muse-wxm-wg1200").read_text()
        self.assertIn('CONFIG_MUSE_DEVICE_MODEL_ID="muse-gadget-wxm-wg1200"', overlay)
        self.assertIn('CONFIG_MUSE_DEVICE_MODEL_NAME="WeatherXM WG1200"', overlay)
        self.assertIn('CONFIG_MUSE_DEVICE_GUIDE="devices/guides/wxm-wg1200.md"', overlay)

    def test_ports_wg1200_onboard_hub_prefers_usj(self) -> None:
        import importlib.util
        from types import SimpleNamespace

        spec = importlib.util.spec_from_file_location(
            "muse_ports", ESP32_DIR / "tools" / "muse" / "ports.py"
        )
        assert spec and spec.loader
        ports_mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ports_mod)

        usj = SimpleNamespace(
            device="/dev/cu.usbmodem11301",
            vid=0x303A,
            pid=0x1001,
            serial_number="24:58:7C:E3:EF:C0",
        )
        ch340 = SimpleNamespace(
            device="/dev/cu.usbserial-1110",
            vid=0x1A86,
            pid=0x7523,
            serial_number=None,
        )
        self.assertEqual(ports_mod.find("wg1200", ports=[usj, ch340]), "/dev/cu.usbmodem11301")
        self.assertEqual(ports_mod.find("wg1200", ports=[ch340]), "/dev/cu.usbserial-1110")
        self.assertEqual(
            ports_mod.find("wg1200", "24:58:7c:e3:ef:c0", ports=[usj, ch340]),
            "/dev/cu.usbmodem11301",
        )
        usj2 = SimpleNamespace(
            device="/dev/cu.usbmodem11401",
            vid=0x303A,
            pid=0x1001,
            serial_number="AA:BB:CC:DD:EE:FF",
        )
        with self.assertRaises(ports_mod.NotFound):
            ports_mod.find("wg1200", ports=[usj, usj2, ch340])


if __name__ == "__main__":
    unittest.main()
