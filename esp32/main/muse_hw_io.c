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

// The board's expansion ports for the agent (muse_board->ports): the Grove
// port's power and I2C bus, a header's spare UART, the chip's own temperature
// sensor, and the clock: system time from NTP once on Wi-Fi, kept in the
// board's PCF8563 RTC (0x51) and restored from it at boot. On-board chips
// share the Grove bus: the agent may read them, never write them.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "config_store.h"
#include "muse_board.h"
#include "muse_hw_commands_priv.h"
#include "muse_wifi.h"

static const char *TAG = "link.ports";

#define HEADER_UART UART_NUM_2
#define UART_BUF 1024
#define I2C_MAX 256
#define I2C_TIMEOUT_MS 100
#define RTC_ADDR 0x51
#define TIME_VALID_FROM 1735689600     // 2025-01-01: before this, the clock was never set
#define CLOCK_CHECK_US (5 * 1000000LL)     // cheap: the RTC once, NTP once
#define TZ_MAX 64

// ---- Host-tested: hex --------------------------------------------------------

// "0a ff 12", "0aff12" or "0x0a,0xff" into bytes; -1 if malformed or over cap.
static int parse_hex(const char *s, uint8_t *out, int cap) {
    int n = 0;
    while (*s) {
        if (*s == ' ' || *s == ',' || *s == ':') {
            s++;
            continue;
        }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        int v = 0;
        for (int k = 0; k < 2; k++, s++) {
            char c = *s;
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return -1;
            v = v << 4 | d;
        }
        if (n == cap) return -1;
        out[n++] = (uint8_t)v;
    }
    return n;
}

// Bytes as "0a ff 12"; out holds n * 3.
static void to_hex(const uint8_t *in, int n, char *out) {
    static const char D[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 3] = D[in[i] >> 4];
        out[i * 3 + 1] = D[in[i] & 15];
        out[i * 3 + 2] = i + 1 < n ? ' ' : '\0';
    }
    if (!n) out[0] = '\0';
}

// Days from 1970-01-01 to a civil date (Howard Hinnant's algorithm).
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int from_bcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint8_t to_bcd(int v) { return (uint8_t)(v / 10 << 4 | v % 10); }

// PCF8563 registers 0x02-0x08 (seconds .. years) into Unix time; -1 if its
// voltage-low flag says the time was lost. Years are 2000-2099.
static int64_t rtc_decode(const uint8_t r[7]) {
    if (r[0] & 0x80) return -1;
    int sec = from_bcd(r[0] & 0x7f), min = from_bcd(r[1] & 0x7f), hour = from_bcd(r[2] & 0x3f);
    int day = from_bcd(r[3] & 0x3f), mon = from_bcd(r[5] & 0x1f), year = 2000 + from_bcd(r[6]);
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return -1;
    return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

// Unix time into registers 0x02-0x08, the voltage-low flag cleared.
static void rtc_encode(int64_t t, uint8_t r[7]) {
    int64_t days = t / 86400;
    int rem = (int)(t % 86400);
    // civil_from_days (Hinnant)
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    int y = (int)(yoe + era * 400) + (m <= 2);
    r[0] = to_bcd(rem % 60);
    r[1] = to_bcd(rem / 60 % 60);
    r[2] = to_bcd(rem / 3600);
    r[3] = to_bcd(d);
    r[4] = (uint8_t)((days + 4) % 7);   // 1970-01-01 was a Thursday
    r[5] = to_bcd(m);
    r[6] = to_bcd(y % 100);
}

// ---- Host-tested end

static const muse_ports_t *ports(void) {
    return muse_board && muse_board->ports ? muse_board->ports() : NULL;
}

static bool onboard(const muse_ports_t *p, int addr) {
    for (const uint8_t *a = p->i2c_onboard; a && *a; a++) {
        if (*a == addr) return true;
    }
    return false;
}

static cJSON *bytes_payload(const uint8_t *b, int n) {
    cJSON *pl = cJSON_CreateObject();
    char *hex = malloc(n * 3 + 1);
    if (hex) {
        to_hex(b, n, hex);
        cJSON_AddStringToObject(pl, "hex", hex);
        free(hex);
    }
    cJSON *arr = cJSON_AddArrayToObject(pl, "bytes");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(b[i]));
    return pl;
}

// ---- grove.power -------------------------------------------------------------

static cJSON *grove_power(cJSON *params) {
    const muse_ports_t *p = ports();
    if (!p || !p->grove_power) return hw_error("unsupported", "this board has no switched Grove port");
    cJSON *on = cJSON_GetObjectItem(params, "on");
    if (!cJSON_IsBool(on)) return hw_error("missing_param", "on (true or false) is required");
    esp_err_t err = p->grove_power(cJSON_IsTrue(on));
    if (err != ESP_OK) return hw_error("failed", esp_err_to_name(err));
    return hw_ok(NULL);
}

// ---- i2c.* -------------------------------------------------------------------

static esp_err_t i2c_xfer(const muse_ports_t *p, int addr, int hz, const uint8_t *out, int nout, uint8_t *in,
                          int nin);

// A one-byte read at each address: only a device there acknowledges it.
// (i2c_master_probe() answers wrongly while another task uses the bus, as the
// input task does for the expander every 10 ms.)
static cJSON *i2c_scan(void) {
    const muse_ports_t *p = ports();
    if (!p || !p->i2c_bus) return hw_error("unsupported", "this board has no I2C port");
    cJSON *pl = cJSON_CreateObject();
    cJSON *found = cJSON_AddArrayToObject(pl, "devices");
    for (int a = 0x08; a <= 0x77; a++) {
        uint8_t byte;
        if (i2c_xfer(p, a, 100000, NULL, 0, &byte, 1) != ESP_OK) continue;
        cJSON *d = cJSON_CreateObject();
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x", a);
        cJSON_AddNumberToObject(d, "address", a);
        cJSON_AddStringToObject(d, "hex", hex);
        cJSON_AddBoolToObject(d, "onboard", onboard(p, a));
        cJSON_AddItemToArray(found, d);
    }
    return hw_ok(pl);
}

// One transfer with a device at addr: optional bytes out, then optional bytes in.
static esp_err_t i2c_xfer(const muse_ports_t *p, int addr, int hz, const uint8_t *out, int nout, uint8_t *in,
                          int nin) {
    i2c_master_dev_handle_t dev;
    const i2c_device_config_t cfg = { .device_address = (uint16_t)addr, .scl_speed_hz = (uint32_t)hz };
    esp_err_t err = i2c_master_bus_add_device(p->i2c_bus, &cfg, &dev);
    if (err != ESP_OK) return err;
    if (nout && nin) err = i2c_master_transmit_receive(dev, out, nout, in, nin, I2C_TIMEOUT_MS);
    else if (nout) err = i2c_master_transmit(dev, out, nout, I2C_TIMEOUT_MS);
    else err = i2c_master_receive(dev, in, nin, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);
    return err;
}

static cJSON *i2c_access(cJSON *params, bool write) {
    const muse_ports_t *p = ports();
    if (!p || !p->i2c_bus) return hw_error("unsupported", "this board has no I2C port");
    int addr, reg = -1, len = 1, hz = 100000;
    if (!hw_int(params, "address", &addr) || addr < 0x08 || addr > 0x77) {
        return hw_error("invalid_params", "address is a 7-bit address, 8 to 119 (0x08-0x77)");
    }
    hw_int(params, "register", &reg);
    hw_int(params, "length", &len);
    hw_int(params, "speed_hz", &hz);
    hz = hw_clamp(hz, 10000, 400000);
    if (reg > 255) return hw_error("invalid_params", "register is 0 to 255");
    uint8_t buf[I2C_MAX + 1];
    int nout = 0;
    if (reg >= 0) buf[nout++] = (uint8_t)reg;
    esp_err_t err;
    if (write) {
        if (onboard(p, addr)) return hw_error("not_permitted", "that address is one of the device's own chips");
        const char *data = hw_str(params, "data");
        int n = data ? parse_hex(data, buf + nout, I2C_MAX) : -1;
        if (n < 0) return hw_error("invalid_params", "data is hex bytes, e.g. \"0a ff 12\", up to 256");
        nout += n;
        if (!nout) return hw_error("invalid_params", "nothing to write");
        err = i2c_xfer(p, addr, hz, buf, nout, NULL, 0);
        if (err != ESP_OK) return hw_error("i2c_failed", esp_err_to_name(err));
        return hw_ok(NULL);
    }
    len = hw_clamp(len, 1, I2C_MAX);
    uint8_t in[I2C_MAX];
    err = i2c_xfer(p, addr, hz, buf, nout, in, len);
    if (err != ESP_OK) return hw_error("i2c_failed", esp_err_to_name(err));
    return hw_ok(bytes_payload(in, len));
}

// ---- uart.* ------------------------------------------------------------------

static int s_uart_baud;

static esp_err_t uart_ready(const muse_ports_t *p, int baud) {
    if (s_uart_baud == baud) return ESP_OK;
    if (!s_uart_baud) {
        esp_err_t err = uart_driver_install(HEADER_UART, UART_BUF, 0, 0, NULL, 0);
        if (err != ESP_OK) return err;
        err = uart_set_pin(HEADER_UART, p->uart_tx, p->uart_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) return err;
    }
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(HEADER_UART, &cfg);
    if (err == ESP_OK) s_uart_baud = baud;
    return err;
}

static cJSON *uart_access(cJSON *params, bool write) {
    const muse_ports_t *p = ports();
    if (!p || p->uart_tx < 0) return hw_error("unsupported", "this board has no spare UART");
    int baud = 115200;
    hw_int(params, "baud", &baud);
    esp_err_t err = uart_ready(p, hw_clamp(baud, 1200, 2000000));
    if (err != ESP_OK) return hw_error("failed", esp_err_to_name(err));
    if (write) {
        const char *text = hw_str(params, "text");
        const char *hex = hw_str(params, "hex");
        uint8_t buf[UART_BUF];
        int n;
        if (text) {
            n = (int)strlen(text);
            if (n > UART_BUF) return hw_error("invalid_params", "at most 1024 bytes");
            memcpy(buf, text, n);
        } else if (hex) {
            n = parse_hex(hex, buf, UART_BUF);
            if (n < 0) return hw_error("invalid_params", "hex is bytes, e.g. \"0a ff\", up to 1024");
        } else {
            return hw_error("missing_param", "text or hex is required");
        }
        uart_write_bytes(HEADER_UART, buf, n);
        uart_wait_tx_done(HEADER_UART, pdMS_TO_TICKS(1000));
        cJSON *pl = cJSON_CreateObject();
        cJSON_AddNumberToObject(pl, "sent", n);
        return hw_ok(pl);
    }
    int max = UART_BUF, wait = 1000;
    hw_int(params, "max_bytes", &max);
    hw_int(params, "wait_ms", &wait);
    max = hw_clamp(max, 1, UART_BUF);
    uint8_t buf[UART_BUF];
    int n = uart_read_bytes(HEADER_UART, buf, max, pdMS_TO_TICKS(hw_clamp(wait, 0, 10000)));
    if (n < 0) n = 0;
    cJSON *pl = bytes_payload(buf, n);
    bool printable = true;
    for (int i = 0; i < n; i++) {
        if ((buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') || buf[i] >= 0x7f) printable = false;
    }
    if (printable) {
        char *text = malloc(n + 1);
        if (text) {
            memcpy(text, buf, n);
            text[n] = '\0';
            cJSON_AddStringToObject(pl, "text", text);
            free(text);
        }
    }
    return hw_ok(pl);
}

// ---- The chip's temperature (device.status) ----------------------------------

void muse_hw_io_status(cJSON *pl) {
    static temperature_sensor_handle_t tsens;
    if (!tsens) {
        const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&cfg, &tsens) != ESP_OK || temperature_sensor_enable(tsens) != ESP_OK) {
            ESP_LOGW(TAG, "no temperature sensor");
            tsens = NULL;
            return;
        }
    }
    float c;
    if (temperature_sensor_get_celsius(tsens, &c) == ESP_OK) {
        cJSON_AddNumberToObject(pl, "chip_temp_c", (double)(int)(c * 10) / 10);
    }
}

// ---- The clock: NTP, the RTC, the timezone ------------------------------------

static const char *s_time_source = "none";
static bool s_rtc_checked, s_sntp;

static bool time_valid(void) {
    return time(NULL) >= TIME_VALID_FROM;
}

static bool rtc_present(const muse_ports_t *p) {
    if (!p || !p->i2c_bus) return false;
    for (const uint8_t *a = p->i2c_onboard; a && *a; a++) {
        if (*a == RTC_ADDR) return true;
    }
    return false;
}

static int64_t rtc_read(const muse_ports_t *p) {
    uint8_t reg = 0x02, r[7];
    if (!rtc_present(p) || i2c_xfer(p, RTC_ADDR, 100000, &reg, 1, r, 7) != ESP_OK) return -1;
    return rtc_decode(r);
}

static esp_err_t rtc_write(const muse_ports_t *p, int64_t t) {
    uint8_t buf[8] = { 0x02 };
    if (!rtc_present(p)) return ESP_ERR_NOT_SUPPORTED;
    rtc_encode(t, buf + 1);
    return i2c_xfer(p, RTC_ADDR, 100000, buf, 8, NULL, 0);
}

static void set_system_time(int64_t t) {
    struct timeval tv = { .tv_sec = (time_t)t };
    settimeofday(&tv, NULL);
}

static void on_ntp(struct timeval *tv) {
    s_time_source = "ntp";
    rtc_write(ports(), tv->tv_sec);
    ESP_LOGI(TAG, "clock set from NTP; RTC updated");
}

// Every 5 s: the RTC once, then NTP once Wi-Fi is up.
static void clock_check(void *arg) {
    (void)arg;
    const muse_ports_t *p = ports();
    if (!p) return;
    if (!s_rtc_checked) {
        s_rtc_checked = true;
        char tz[TZ_MAX];
        if (config_get_str("tz", tz, sizeof(tz)) && tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
        }
        int64_t t = rtc_read(p);
        if (t >= TIME_VALID_FROM) {
            if (!time_valid()) {
                set_system_time(t);
                ESP_LOGI(TAG, "clock set from the RTC");
            }
            s_time_source = "rtc";
        }
    }
    if (!s_sntp && muse_wifi_connected()) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        cfg.sync_cb = on_ntp;
        s_sntp = esp_netif_sntp_init(&cfg) == ESP_OK;
    }
}

// Before NVS is up: the timezone and the RTC wait for the first check.
void muse_hw_io_init(void) {
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t a = { .callback = clock_check, .name = "hw_clock" };
    if (esp_timer_create(&a, &timer) == ESP_OK) esp_timer_start_periodic(timer, CLOCK_CHECK_US);
}

static void add_time(cJSON *o, const char *key, const struct tm *tm) {
    char iso[32];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", tm);
    cJSON_AddStringToObject(o, key, iso);
}

static cJSON *device_time(cJSON *params, bool may_save) {
    const muse_ports_t *p = ports();
    int unix_s;
    if (hw_int(params, "unix", &unix_s)) {
        if (unix_s < TIME_VALID_FROM) return hw_error("invalid_params", "unix is seconds since 1970, 2025 or later");
        set_system_time(unix_s);
        s_time_source = "set";
        rtc_write(p, unix_s);
    }
    const char *tz = hw_str(params, "tz");
    if (tz) {
        if (!may_save) return hw_error("not_permitted", "scripts can't change the timezone");
        if (strlen(tz) >= TZ_MAX) return hw_error("invalid_params", "tz is a POSIX TZ string, e.g. PST8PDT,M3.2.0,M11.1.0");
        config_set_str("tz", tz);
        setenv("TZ", tz, 1);
        tzset();
    }
    time_t now = time(NULL);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddBoolToObject(pl, "valid", time_valid());
    cJSON_AddStringToObject(pl, "source", s_time_source);
    if (time_valid()) {
        struct tm utc, local;
        gmtime_r(&now, &utc);
        localtime_r(&now, &local);
        cJSON_AddNumberToObject(pl, "unix", (double)now);
        add_time(pl, "utc_time", &utc);
        add_time(pl, "local_time", &local);   // not "local": a Lua keyword
        cJSON_AddNumberToObject(pl, "hour", local.tm_hour);
        cJSON_AddNumberToObject(pl, "minute", local.tm_min);
        cJSON_AddNumberToObject(pl, "weekday", local.tm_wday);   // 0 = Sunday
    }
    const char *env_tz = getenv("TZ");
    cJSON_AddStringToObject(pl, "tz", env_tz ? env_tz : "UTC0");
    cJSON_AddBoolToObject(pl, "rtc", rtc_present(p) && rtc_read(p) >= 0);
    return hw_ok(pl);
}

// ---- Registration and dispatch -----------------------------------------------

void muse_hw_io_register(cJSON *commands) {
    cJSON *t = cJSON_CreateObject();
    cJSON_AddItemToObject(t, "unix", hw_param("integer", "Set the clock: seconds since 1970 (UTC)."));
    cJSON_AddItemToObject(t, "tz", hw_param("string", "Set the timezone, POSIX TZ, e.g. CET-1CEST,M3.5.0,M10.5.0/3; kept."));
    hw_add(commands, "device.time",
           "The device's clock (set from NTP on Wi-Fi, kept in its RTC): valid, unix, utc_time, "
           "local_time, hour, minute, weekday (0 Sunday), tz. Set the time or timezone with the options.",
           NULL, t, 0);
    const muse_ports_t *p = ports();
    if (!p) return;
    if (p->grove_power) {
        hw_add(commands, "grove.power", "Switch the Grove port's 3.3 V on or off (off at boot).",
               hw_params("on", hw_param("boolean", "true for on.")), NULL, 0);
    }
    if (p->i2c_bus) {
        hw_add(commands, "i2c.scan",
               "List the I2C addresses that answer on the Grove port's bus (onboard: the device's own chips). "
               "Turn on grove.power first for a Grove sensor.",
               NULL, NULL, 10000);
        cJSON *rd = cJSON_CreateObject();
        cJSON_AddItemToObject(rd, "register", hw_param("integer", "Written first, 0-255; omit for a plain read."));
        cJSON_AddItemToObject(rd, "length", hw_param("integer", "Bytes to read, 1-256; default 1."));
        cJSON_AddItemToObject(rd, "speed_hz", hw_param("integer", "Default 100000."));
        hw_add(commands, "i2c.read", "Read bytes from an I2C device on the Grove bus; returns hex and bytes.",
               hw_params("address", hw_param("integer", "7-bit address, e.g. 68 for 0x44.")), rd, 5000);
        cJSON *wr = cJSON_CreateObject();
        cJSON_AddItemToObject(wr, "address", hw_param("integer", "7-bit address."));
        cJSON_AddItemToObject(wr, "data", hw_param("string", "Hex bytes, e.g. \"24 00\"."));
        cJSON *wr_opt = cJSON_CreateObject();
        cJSON_AddItemToObject(wr_opt, "register", hw_param("integer", "Sent before data, 0-255."));
        cJSON_AddItemToObject(wr_opt, "speed_hz", hw_param("integer", "Default 100000."));
        hw_add(commands, "i2c.write", "Write bytes to an I2C device on the Grove bus (not the device's own chips).",
               wr, wr_opt, 5000);
    }
    if (p->uart_tx >= 0) {
        cJSON *w = cJSON_CreateObject();
        cJSON_AddItemToObject(w, "text", hw_param("string", "Text to send; or hex."));
        cJSON_AddItemToObject(w, "hex", hw_param("string", "Bytes to send, e.g. \"01 02\"."));
        cJSON_AddItemToObject(w, "baud", hw_param("integer", "Default 115200."));
        hw_add(commands, "uart.write", "Send on the header's UART (TX GPIO19, RX GPIO20, 3.3 V).", NULL, w, 5000);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddItemToObject(r, "max_bytes", hw_param("integer", "1-1024; default 1024."));
        cJSON_AddItemToObject(r, "wait_ms", hw_param("integer", "0-10000; default 1000."));
        cJSON_AddItemToObject(r, "baud", hw_param("integer", "Default 115200."));
        hw_add(commands, "uart.read", "Read what arrived on the header's UART; returns hex, bytes and text.",
               NULL, r, 15000);
    }
}

cJSON *muse_hw_io_command(const char *command, cJSON *params, bool may_save) {
    if (!strcmp(command, "device.time")) return device_time(params, may_save);
    if (!strcmp(command, "grove.power")) return grove_power(params);
    if (!strcmp(command, "i2c.scan")) return i2c_scan();
    if (!strcmp(command, "i2c.read")) return i2c_access(params, false);
    if (!strcmp(command, "i2c.write")) return i2c_access(params, true);
    if (!strcmp(command, "uart.write")) return uart_access(params, true);
    if (!strcmp(command, "uart.read")) return uart_access(params, false);
    return NULL;
}
