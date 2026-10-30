#include <assert.h>
#include <string.h>
#include "reterminal_e100x.h"
#include "reterminal_e100x_board.h"
#include "reterminal_e100x_rtc.h"
#include "freertos/task.h"
#include "e1003_touch.h"
#include "e1003_pmic.h"
#include "esp_wifi.h"
#include "link_power.h"

#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
_Static_assert(RETERMINAL_E100X_BOARD_ID == 3, "E1003 profile");
_Static_assert(E100X_SD_ENABLE_GPIO == 39, "E1003 SD power");
_Static_assert(E100X_BATTERY_ENABLE_GPIO == 40, "E1003 battery enable");
#else
_Static_assert(RETERMINAL_E100X_BOARD_ID == (CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 ? 1 : 2),
               "E1001/E1002 profile");
_Static_assert(E100X_SD_ENABLE_GPIO == 16, "E1001/E1002 SD power");
_Static_assert(E100X_BATTERY_ENABLE_GPIO == 21, "E1001/E1002 battery enable");
#endif

void check_registration(void);
void check_hardware_wire(cJSON *result);
static TaskFunction_t s_task;
static void *s_arg;
static cJSON *s_reply;
static bool s_task_fail;
static int s_hz, s_ms;

static int64_t s_clock = 1000, s_battery_at;
static int s_battery_mv, s_battery_pct;
static bool s_battery_valid;
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t esp_timer_get_time(void) { return s_clock; }
int esp_wifi_get_ps(wifi_ps_type_t *ps) { *ps = WIFI_PS_MIN_MODEM; return ESP_OK; }
bool link_power_acquire(link_power_owner_t owner) { (void)owner; return true; }
void link_power_release(link_power_owner_t owner) { (void)owner; }
link_power_snapshot_t link_power_snapshot(void) { return (link_power_snapshot_t){0}; }
bool link_power_light_sleep_stats(uint32_t *count, uint64_t *total_us,
                                  uint32_t *rejected, char **raw_dump,
                                  const char **reason) {
    (void)count; (void)total_us; (void)rejected;
    *raw_dump = NULL; *reason = "test";
    return false;
}
#include "battery_cache.inc"
void vTaskDelay(int ms) { (void)ms; }
void vTaskDelete(void *task) { (void)task; }
int xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    if (s_task_fail) return 0;
    s_task = task; s_arg = arg;
    return 1;
}
void noise_ctrl_send_command_result(noise_ctrl_session_generation_t gen,
                                    const char *id, cJSON *r) {
    assert(gen == 123 && strcmp(id, "hardware-id") == 0 && !s_reply);
    check_hardware_wire(r);
    s_reply = r;
}
bool battery_adc_read(int *mv, int *pct) { *mv = 3800; *pct = 66; return true; }
bool reterminal_e100x_buttons_init(void) { return true; }
bool reterminal_e100x_buttons_read(reterminal_e100x_buttons_state_t *s) {
    *s = (reterminal_e100x_buttons_state_t){.key1_down = true, .key1_presses = 2};
    return true;
}
bool reterminal_e100x_sd_prepare_bus(void) { return true; }
bool reterminal_e100x_sd_monitor_init(void) { return true; }
bool reterminal_e100x_microphone_init(void) { return true; }
bool reterminal_e100x_buzzer_beep(int hz, int ms) { s_hz = hz; s_ms = ms; return true; }
bool reterminal_e100x_rtc_read(reterminal_e100x_rtc_time_t *t) { *t = (reterminal_e100x_rtc_time_t){2026,10,18,12,34,56,true}; return true; }
bool reterminal_e100x_rtc_set(const reterminal_e100x_rtc_time_t *t) { return t->year == 2026; }
cJSON *reterminal_e100x_sd_command(const char *command, const cJSON *params) { (void)command; (void)params; return cJSON_CreateObject(); }
cJSON *reterminal_e100x_microphone_record(const cJSON *params) { (void)params; return cJSON_CreateObject(); }
cJSON *reterminal_e100x_events_command(const char *command, const cJSON *p, const char *id,
                            noise_ctrl_session_generation_t gen) {
    (void)command; (void)p; (void)id; (void)gen;
    return cJSON_CreateObject();
}
static cJSON *call(const char *command, cJSON *params) {
    return reterminal_e100x_command(command, params, "hardware-id", 123);
}
#if RETERMINAL_E100X_HARDWARE_ENABLED
static cJSON *run(const char *command, const char *json) {
    cJSON *params = json ? cJSON_Parse(json) : NULL;
    cJSON *r = call(command, params);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "_async")));
    cJSON_Delete(params); cJSON_Delete(r);
    s_task(s_arg);
    r = s_reply; s_reply = NULL;
    return r;
}
#endif
int main(void) {
    check_registration();
    assert(reterminal_e100x_handles("power.status"));
    assert(!reterminal_e100x_handles("power.deep_sleep"));
    cJSON *power = call("power.status", NULL);
    cJSON *power_payload = cJSON_GetObjectItem(power, "payload");
    assert(cJSON_GetObjectItem(power_payload, "active_owners"));
    assert(!cJSON_GetObjectItem(power_payload, "deep_sleep_enabled"));
    assert(!cJSON_GetObjectItem(power_payload, "wake_cause"));
    assert(!cJSON_GetObjectItem(power_payload, "idle_ms"));
    cJSON_Delete(power);
    assert(reterminal_e100x_handles("microphone.record") == RETERMINAL_E100X_HARDWARE_ENABLED);
    assert(!reterminal_e100x_handles("display.pages.set"));
    cJSON *r;
#if RETERMINAL_E100X_INPUT_ENABLED
    r = call("buttons.read", NULL);
    cJSON *p = cJSON_GetObjectItem(r, "payload");
    assert(cJSON_GetObjectItem(p, "key1_presses")->valueint == 2);
    check_hardware_wire(r); cJSON_Delete(r);
#endif
#if RETERMINAL_E100X_HARDWARE_ENABLED
    r = call("battery.read", NULL);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "battery_mv")));
    cJSON_Delete(r);
    s_battery_valid=true;s_battery_mv=3800;s_battery_pct=66;s_battery_at=s_clock;
    r=call("battery.read",NULL);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"payload"),"battery_mv")->valueint==3800);
    cJSON_Delete(r);s_clock+=60000001;
    r=call("battery.read",NULL);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(r,"payload"),"battery_mv")));
    cJSON_Delete(r);s_battery_valid=false;s_clock=1000;
    r = run("rtc.read", NULL);
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "payload"), "year")->valueint == 2026);
    cJSON_Delete(r);
    r = run("rtc.set", "{\"year\":2026.5}");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok"))); cJSON_Delete(r);
    r = run("buzzer.beep", "{\"frequency_hz\":0,\"duration_ms\":20}");
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok"))); cJSON_Delete(r);
    // Parameters survive deletion/mutation of the original request.
    cJSON *p2 = cJSON_Parse("{\"frequency_hz\":1000,\"duration_ms\":50}");
    r = call("buzzer.beep", p2); cJSON_Delete(r);
    cJSON_SetNumberValue(cJSON_GetObjectItem(p2, "frequency_hz"), 2000);
    r = call("rtc.read", NULL);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok"))); cJSON_Delete(r);
    cJSON_Delete(p2);
    s_task(s_arg);
    assert(s_hz == 1000 && s_ms == 50);
    cJSON_Delete(s_reply); s_reply = NULL;
    s_task_fail = true;
    r = call("rtc.read", NULL);
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok"))); cJSON_Delete(r);
    s_task_fail = false;
    r = run("rtc.read", NULL); cJSON_Delete(r);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
    r = call("touch.read", NULL); check_hardware_wire(r); cJSON_Delete(r);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    cJSON *led_params=cJSON_Parse("{\"on\":true}");
    r = call("led.set", led_params); cJSON_Delete(led_params); check_hardware_wire(r); cJSON_Delete(r);
    r = run("pmic.read", NULL); check_hardware_wire(r); cJSON_Delete(r);
#endif
    return 0;
}



bool e1003_touch_init(void) { return true; }
void e1003_touch_read(e1003_touch_state_t *s) { *s=(e1003_touch_state_t){.available=true,.sensor_width=1872,.sensor_height=1404}; }
bool e1003_pmic_read(e1003_pmic_status_t *s) { *s=(e1003_pmic_status_t){.charging=true,.input_present=true};return true; }
bool e1003_pmic_read_power(bool *c,bool *p) { *c=true;*p=true;return true; }
const char *e1003_pmic_input_name(uint8_t c) { (void)c;return "none"; }
const char *e1003_pmic_charge_name(uint8_t c) { (void)c;return "fast_charging"; }
bool reterminal_e100x_led_read(bool *on) { *on=true;return true; }
bool reterminal_e100x_led_set(bool on) { (void)on;return true; }
