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
#include <assert.h>
#include <stdio.h>
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/task.h"
/* SDK stubs always succeed; only charging and capture boundaries are observed. */
enum { CONFIG_ESP_CONSOLE_UART_NUM = 0, ESP_PM_CPU_FREQ_MAX = 2, ESP_PM_NO_LIGHT_SLEEP = 1 };
typedef int esp_pm_lock_handle_t;
static inline esp_err_t esp_pm_lock_create(int type, int arg, const char *name, esp_pm_lock_handle_t *out) { *out = 1; return ESP_OK; }
STUB(esp_err_t, esp_pm_lock_acquire, (esp_pm_lock_handle_t lock), ESP_OK)
STUB(esp_err_t, esp_pm_lock_release, (esp_pm_lock_handle_t lock), ESP_OK)
/* Separate translation units let the backend override weak core hooks. */
#ifdef TEST_CORE
enum { ESP_LOG_NONE = 0, ESP_LOG_ERROR = 1 };
#define esp_log_set_vprintf(fn) ((void)(fn))
#define esp_log_level_set(tag, level) ((void)0)
#define esp_rom_install_channel_putc(channel, fn) ((void)0)
enum { ESP_SLEEP_WAKEUP_TIMER = 4, ESP_SLEEP_WAKEUP_ALL = 0, ESP_PD_DOMAIN_CPU = 0,
       ESP_PD_OPTION_ON = 1, ESP_PD_OPTION_OFF = 0, ESP_ERR_WIFI_NOT_STARTED = 999,
       WIFI_EVENT = 0, WIFI_EVENT_SCAN_DONE = 1, WIFI_STORAGE_RAM = 0, WIFI_MODE_STA = 0, WIFI_SCAN_TYPE_ACTIVE = 0 };
#define WIFI_INIT_CONFIG_DEFAULT() {0}
typedef int esp_event_base_t, esp_event_handler_instance_t;
typedef struct { bool nvs_enable; } wifi_init_config_t;
typedef struct { unsigned status; } wifi_event_sta_scan_done_t;
typedef struct { int scan_type; struct { struct { int min, max; } active; } scan_time; } wifi_scan_config_t;
typedef struct { int min_freq_mhz, max_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
static esp_pm_config_t test_pm;
static void (*test_scan_done)(void *, esp_event_base_t, int32_t, void *);
static inline esp_err_t esp_pm_configure(const esp_pm_config_t *p) { test_pm = *p; return ESP_OK; }
static inline esp_err_t esp_pm_get_configuration(esp_pm_config_t *p) { *p = test_pm; return ESP_OK; }
STUB(int, esp_clk_cpu_freq, (void), test_pm.max_freq_mhz * 1000000)
STUB(uint32_t, esp_random, (void), 111)
STUB(int64_t, esp_rtc_get_time_us, (void), esp_timer_get_time())
STUB(uint32_t, esp_sleep_get_wakeup_causes, (void), 0)
STUB(esp_err_t, esp_sleep_pd_config, (int domain, int option), ESP_OK)
STUB(esp_err_t, esp_sleep_disable_wakeup_source, (int source), ESP_OK)
STUB(esp_err_t, esp_sleep_enable_timer_wakeup, (uint64_t us), ESP_OK)
static inline void esp_deep_sleep_start(void) { assert(false); }
STUB(esp_err_t, esp_netif_init, (void), ESP_OK)
STUB(esp_err_t, esp_event_loop_create_default, (void), ESP_OK)
static inline esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
    void (*fn)(void *, esp_event_base_t, int32_t, void *), void *arg, esp_event_handler_instance_t *out) { test_scan_done = fn; *out = 1; return ESP_OK; }
STUB(esp_err_t, esp_wifi_init, (const wifi_init_config_t *cfg), ESP_OK)
STUB(esp_err_t, esp_wifi_set_storage, (int storage), ESP_OK)
STUB(esp_err_t, esp_wifi_set_mode, (int mode), ESP_OK)
STUB(esp_err_t, esp_wifi_start, (void), ESP_OK)
static inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg, bool block) { wifi_event_sta_scan_done_t event = {0}; test_scan_done(NULL, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &event); return ESP_OK; }
STUB(esp_err_t, esp_wifi_scan_stop, (void), ESP_OK)
STUB(esp_err_t, esp_wifi_stop, (void), ESP_OK)
STUB(esp_err_t, esp_wifi_deinit, (void), ESP_OK)
STUB(esp_err_t, esp_wifi_clear_ap_list, (void), ESP_OK)
STUB(esp_err_t, uart_wait_tx_done, (int port, unsigned ticks), ESP_FAIL)
STUB(int, uart_write_bytes, (int port, const void *p, size_t n), 0)
STUB(int, uart_read_bytes, (int port, void *p, size_t n, unsigned ticks), 0)
STUB(esp_err_t, uart_driver_install, (int port, int rx, int tx, int queue, void *handle, int flags), ESP_FAIL)
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs) { return ESP_OK; }
#include <stddef.h>
#include "../components/muse/muse_power_test.c"
/* The new journal bytes sit in #168's padding: its v3 layout is unchanged. */
_Static_assert(sizeof(run_t) == 7648, "v3 run size");
_Static_assert(sizeof(s_saved) == 7664, "v3 saved size");
_Static_assert(offsetof(run_t, error) == 156, "v3 padding boundary");
extern int64_t test_now;
/* Exercise error cleanup directly; SDK fakes always succeed. */
void test_error(void) { abort_run(ESP_FAIL, true); }
bool test_running(void) { return s_run.status == RUN_RUNNING; }
bool test_capture(void)
{
    if (s_run.status != RUN_RUNNING || !s_run.count) { return false; }
    record_t *r = &s_run.records[s_run.count - 1];
    return r->measure_start_us && !r->end_us;
}
void test_command(const char *line) { command(line); }
void test_advance(unsigned ms)
{
    for (unsigned i = 0; i < ms && (s_run.status == RUN_ARMED || s_run.status == RUN_RUNNING); i += 10) {
        test_now += 10000; tick();
    }
}
#else
#define MUSE_PTEST_HOST 1
#define CONFIG_MUSE_POWER_TEST 1
static int test_aipi, test_216;
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_175C (!test_aipi && !test_216)
#define CONFIG_MUSE_BOARD_WAVESHARE_S3_216 test_216
#define CONFIG_MUSE_BOARD_AIPI test_aipi
#define CONFIG_IDF_TARGET_ESP32S3 1
#include <setjmp.h>
#include <stdlib.h>
#define MUSE_UI_PRIORITY 5
#define MUSE_UI_CORE 1
STUB(TaskHandle_t, xTaskGetCurrentTaskHandle, (void), (TaskHandle_t)2)
static inline void vTaskDelete(TaskHandle_t task) {}
static inline int xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out, int core) { fn(arg); return pdPASS; }
STUB(esp_err_t, bsp_i2c_init, (void), ESP_OK)
#include "../components/muse/boards/board_power_test_generic.c"
extern bool test_capture(void), test_running(void);
extern void test_command(const char *), test_advance(unsigned), test_error(void);
int64_t test_now = 1000000;
static int pmu;
static uint8_t pmu_reg; /* Charger disabled before boot recovery. */
static unsigned pmu_failures; /* Next PMIC writes that fail without changing the register. */
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
static jmp_buf guard_wait;
static void (*guard_entry)(void *);
static unsigned notifications, guard_timeouts;
static int speaker, microphone, bus;
int esp_codec_dev_open(esp_codec_dev_handle_t d, esp_codec_dev_sample_info_t *s) { assert(!test_capture()); return ESP_OK; }
int esp_codec_dev_close(esp_codec_dev_handle_t d) { assert(!test_capture()); return ESP_OK; }
int64_t esp_timer_get_time(void) { return test_now; }
static const char *console_input;
int usb_serial_jtag_read_bytes(void *p, size_t n, uint32_t ticks) { if (!console_input || !*console_input) { return 0; } *(uint8_t *)p = *console_input++; return 1; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out) { guard_entry = fn; *out = (void *)1; return pdPASS; }
void xTaskNotifyGive(TaskHandle_t task) { notifications++; }
uint32_t ulTaskNotifyTake(int clear, TickType_t wait)
{
    if (notifications) { notifications = 0; return 1; }
    if (!wait) { return 0; }
    if (guard_timeouts) { assert(wait != (TickType_t)portMAX_DELAY); guard_timeouts--; test_now += (int64_t)wait * 1000; return 0; }
    longjmp(guard_wait, 1);
}
static lv_display_t *display_start(lv_indev_t **touch) { static lv_display_t display; return &display; }
static bool display_lock(int timeout) { return true; }
static void display_unlock(void) {}
static void brightness(int value) {}
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic) { *spk = &speaker; *mic = &microphone; return ESP_OK; }
static esp_err_t power(muse_power_t *p) { p->usb = true; return ESP_OK; }
static muse_board_t board = { .name = "Generic test board", .init = bsp_i2c_init, .display_start = display_start, .display_lock = display_lock,
    .display_unlock = display_unlock, .set_brightness = brightness,
    .audio_init = audio_init, .read_power = power, .mic_slot = -1 };
const muse_board_t *muse_board_get(void) { return &board; }
esp_err_t i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out) { *out = &bus; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t addr, int ms) { return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *d) { *d = cfg->device_address == 0x34 ? &pmu : cfg->device_address == 0x40 ? &microphone : &speaker; return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int ms)
{
    if (d == &pmu) { assert(*tx == 0x18); *rx = pmu_reg; return ESP_OK; }
    assert(!test_capture()); *rx = 0; return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *tx, size_t n, int ms)
{
    assert(d == &pmu && n == 2 && tx[0] == 0x18);
    if (pmu_failures) { pmu_failures--; return ESP_FAIL; }
    /* Charging must stay off through the final capture, until cleanup. */
    if (tx[1] & 2) { assert(!test_running() || s_charge_expired == CHARGE_EXPIRED_MAGIC); }
    pmu_reg = tx[1]; return ESP_OK;
}
lv_obj_t *lv_display_get_screen_active(lv_display_t *d) { static lv_obj_t screen; return &screen; }
lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t ms, void *arg) { static lv_timer_t timer; return &timer; }
int usb_serial_jtag_write_bytes(const void *p, size_t n, uint32_t ticks) { return fwrite(p, 1, n, stdout); }
int main(int argc, char **argv)
{
    test_216 = !strcmp(argv[1], "216");
    test_aipi = !strcmp(argv[1], "aipi");
    muse_power_test_run();
}
void vTaskDelay(unsigned ticks)
{
    assert(!console_input || !*console_input);
    static char line[512];
    unsigned value;
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "advance %u", &value) == 1) { test_advance(value); }
        else if (!strcmp(line, "capture")) { assert(test_capture()); }
        else if (sscanf(line, "charging %u", &value) == 1) { assert(!!(pmu_reg & 2) == (bool)value); }
        else if (sscanf(line, "pmufail %u", &value) == 1) { pmu_failures = value; }
        else if (!strncmp(line, "poll ", 5)) { strcat(line, "\n"); console_input = line + 5; return; }
        else if (!strcmp(line, "guard")) { guard_timeouts = 1; if (!setjmp(guard_wait)) { assert(guard_entry); guard_entry(NULL); } assert(pmu_reg & 2); test_advance(10); }
        else if (!strcmp(line, "error")) { test_error(); }
        else { test_command(line); }
    }
    exit(0);
}
#endif
