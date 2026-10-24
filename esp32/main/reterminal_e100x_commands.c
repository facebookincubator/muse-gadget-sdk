// SPDX-License-Identifier: Apache-2.0

#include "reterminal_e100x.h"
#include "link_power.h"

#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"

#include "battery_adc.h"
#include "reterminal_e100x_board.h"
#include "reterminal_e100x_events.h"
#include "reterminal_e100x_microphone.h"
#include "reterminal_e100x_rtc.h"
#include "reterminal_e100x_sd.h"
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
#include "e1003_pmic.h"
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
#include "e1003_touch.h"
#endif
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;

static const char *const COMMANDS[] = {
    "power.status",
#if RETERMINAL_E100X_INPUT_ENABLED
    "buttons.read", "events.read", "events.wait",
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
    "touch.read",
#endif
#if RETERMINAL_E100X_HARDWARE_ENABLED
    "buzzer.beep", "battery.read", "rtc.read", "rtc.set", "sd.status", "sd.list",
    "sd.read_text", "sd.write_text", "sd.read", "sd.write", "microphone.record",
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    "pmic.read", "led.read", "led.set",
#endif
};
bool reterminal_e100x_handles(const char *command) {
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); ++i)
        if (strcmp(command, COMMANDS[i]) == 0) return true;
    return false;
}
void reterminal_e100x_health(cJSON *metrics) {
#if RETERMINAL_E100X_HARDWARE_ENABLED
    cJSON_DeleteItemFromObjectCaseSensitive(metrics, "battery_mv");
    cJSON_DeleteItemFromObjectCaseSensitive(metrics, "battery_pct");
    reterminal_e100x_battery_metrics(metrics);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    bool charging, input_present;
    if (e1003_pmic_read_power(&charging, &input_present)) {
        cJSON_ReplaceItemInObjectCaseSensitive(metrics, "charging", cJSON_CreateBool(charging));
        cJSON_ReplaceItemInObjectCaseSensitive(metrics, "usb_power", cJSON_CreateBool(input_present));
    }
#endif
    (void)metrics;
}
#if RETERMINAL_E100X_HARDWARE_ENABLED
static bool integer(const cJSON *params, const char *name, int low, int high, int *out) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsNumber(p) || p->valuedouble != p->valueint
        || p->valueint < low || p->valueint > high) return false;
    *out = p->valueint;
    return true;
}

#endif
static cJSON *execute(const char *command, const cJSON *params) {
#if RETERMINAL_E100X_HARDWARE_ENABLED
    if (strcmp(command, "microphone.record") == 0) return reterminal_e100x_microphone_record(params);
    if (strncmp(command, "sd.", 3) == 0) return reterminal_e100x_sd_command(command, params);
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    if (strcmp(command, "pmic.read") == 0) {
        e1003_pmic_status_t status;
        if (!e1003_pmic_read(&status)) return reterminal_e100x_error("unavailable", "SY6974B status read failed");
        cJSON *result = reterminal_e100x_reply(), *payload = cJSON_GetObjectItem(result, "payload");
        cJSON_AddStringToObject(payload, "input_source", e1003_pmic_input_name(status.input_code));
        cJSON_AddStringToObject(payload, "charge_state", e1003_pmic_charge_name(status.charge_code));
        cJSON_AddBoolToObject(payload, "input_present", status.input_present);
        cJSON_AddBoolToObject(payload, "bus_good", status.bus_good);
        cJSON_AddBoolToObject(payload, "charging", status.charging);
        cJSON_AddBoolToObject(payload, "charge_done", status.charge_done);
        cJSON_AddNumberToObject(payload, "status_reg", status.status);
        cJSON_AddNumberToObject(payload, "fault_latched_reg", status.fault_latched);
        cJSON_AddNumberToObject(payload, "fault_reg", status.fault);
        cJSON_AddBoolToObject(payload, "battery_overvoltage", (status.fault & 0x08) != 0);
        cJSON_AddNumberToObject(payload, "bus_reg", status.bus);
        return result;
    }
#endif
    if (strcmp(command, "buzzer.beep") == 0) {
        int hz, ms;
        if (!integer(params, "frequency_hz", 100, 5000, &hz)
            || !integer(params, "duration_ms", 20, 1000, &ms)) {
            return reterminal_e100x_error("invalid_param", "frequency_hz 100..5000 and duration_ms 20..1000 required");
        }
        return reterminal_e100x_buzzer_beep(hz, ms) ? reterminal_e100x_reply() : reterminal_e100x_error("unavailable", "buzzer unavailable");
    }
    if (strcmp(command, "rtc.set") == 0) {
        reterminal_e100x_rtc_time_t t = {0};
        if (!integer(params, "year", 2000, 2099, &t.year)
            || !integer(params, "month", 1, 12, &t.month)
            || !integer(params, "day", 1, 31, &t.day)
            || !integer(params, "hour", 0, 23, &t.hour)
            || !integer(params, "minute", 0, 59, &t.minute)
            || !integer(params, "second", 0, 59, &t.second)) {
            return reterminal_e100x_error("invalid_param", "integer RTC calendar fields required");
        }
        return reterminal_e100x_rtc_set(&t) ? reterminal_e100x_reply() : reterminal_e100x_error("unavailable", "invalid calendar date or RTC unavailable");
    }
    if (strcmp(command, "rtc.read") == 0) {
        reterminal_e100x_rtc_time_t t;
        if (!reterminal_e100x_rtc_read(&t)) return reterminal_e100x_error("unavailable", "RTC read failed");
        cJSON *r = reterminal_e100x_reply(), *p = cJSON_GetObjectItem(r, "payload");
        cJSON_AddBoolToObject(p, "valid", t.valid);
        cJSON_AddNumberToObject(p, "year", t.year);
        cJSON_AddNumberToObject(p, "month", t.month);
        cJSON_AddNumberToObject(p, "day", t.day);
        cJSON_AddNumberToObject(p, "hour", t.hour);
        cJSON_AddNumberToObject(p, "minute", t.minute);
        cJSON_AddNumberToObject(p, "second", t.second);
        return r;
    }
#endif
    (void)command;
    (void)params;
    return reterminal_e100x_error("unsupported", "unknown reTerminal command");
}

typedef struct {
    char command[32], request_id[64];
    cJSON *params;
    noise_ctrl_session_generation_t generation;
} command_args_t;

static void command_task(void *arg) {
    command_args_t *args = arg;
    cJSON *result = execute(args->command, args->params);
    if (!result) result = reterminal_e100x_error("out_of_memory", "hardware response allocation failed");
    noise_ctrl_send_command_result(args->generation, args->request_id, result);
    cJSON_Delete(args->params);
    free(args);
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    link_power_release(LINK_POWER_HARDWARE_COMMAND);
    vTaskDelete(NULL);
}

static cJSON *command_unprotected(const char *command, const cJSON *params,
                              const char *request_id,
                              noise_ctrl_session_generation_t generation) {
    if (strcmp(command, "power.status") == 0) {
        cJSON *result = reterminal_e100x_reply();
        if (!result) return NULL;
        cJSON *payload = cJSON_GetObjectItem(result, "payload");
        link_power_snapshot_t snapshot = link_power_snapshot();
        cJSON_AddNumberToObject(payload, "active_owners", snapshot.active_owners);
        wifi_ps_type_t ps;
        if (esp_wifi_get_ps(&ps) == ESP_OK) {
            cJSON_AddStringToObject(payload, "wifi_ps", ps == WIFI_PS_MIN_MODEM ? "min_modem" :
                                    ps == WIFI_PS_MAX_MODEM ? "max_modem" : "none");
        } else {
            cJSON_AddNullToObject(payload, "wifi_ps");
        }
        uint32_t count = 0, rejected = 0;
        uint64_t total_us = 0;
        char *raw_dump = NULL;
        const char *reason = NULL;
        bool available = link_power_light_sleep_stats(&count, &total_us, &rejected,
                                                       &raw_dump, &reason);
#if CONFIG_PM_PROFILING
        cJSON_AddBoolToObject(payload, "pm_profiling_compiled", true);
#else
        cJSON_AddBoolToObject(payload, "pm_profiling_compiled", false);
#endif
        cJSON_AddBoolToObject(payload, "light_sleep_stats_available", available);
        cJSON_AddStringToObject(payload, "light_sleep_stats_reason", reason ? reason : "unknown");
        if (raw_dump) {
            // Temporary, on-demand IDF diagnostic: mode times, sleep counters,
            // and every PM lock's type, active count, acquisitions and held time.
            cJSON_AddStringToObject(payload, "pm_dump", raw_dump);
            free(raw_dump);
        }
        if (available) {
            cJSON_AddNumberToObject(payload, "light_sleep_count", count);
            cJSON_AddNumberToObject(payload, "light_sleep_total_us", (double)total_us);
            cJSON_AddNumberToObject(payload, "light_sleep_rejected_count", rejected);
        }
        return result;
    }
#if RETERMINAL_E100X_INPUT_ENABLED
    if (strcmp(command, "events.read") == 0 || strcmp(command, "events.wait") == 0) {
        return reterminal_e100x_events_command(command, params, request_id, generation);
    }
#endif
#if RETERMINAL_E100X_HARDWARE_ENABLED
    if (strcmp(command, "battery.read") == 0) {
        cJSON *r = reterminal_e100x_reply();
        reterminal_e100x_battery_metrics(cJSON_GetObjectItem(r, "payload"));
        return r;
    }
#endif
#if RETERMINAL_E100X_INPUT_ENABLED
    if (strcmp(command, "buttons.read") == 0) {
        reterminal_e100x_buttons_state_t state;
        if (!reterminal_e100x_buttons_read(&state)) return reterminal_e100x_error("unavailable", "buttons unavailable");
        cJSON *r = reterminal_e100x_reply(), *p = cJSON_GetObjectItem(r, "payload");
        cJSON_AddBoolToObject(p, "key0_down", state.key0_down);
        cJSON_AddBoolToObject(p, "key1_down", state.key1_down);
        cJSON_AddBoolToObject(p, "key2_down", state.key2_down);
        cJSON_AddNumberToObject(p, "key0_presses", state.key0_presses);
        cJSON_AddNumberToObject(p, "key1_presses", state.key1_presses);
        cJSON_AddNumberToObject(p, "key2_presses", state.key2_presses);
        return r;
    }
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
    if (strcmp(command, "touch.read") == 0) {
        cJSON *result = reterminal_e100x_reply(), *payload = cJSON_GetObjectItem(result, "payload");
        e1003_touch_state_t state;
        e1003_touch_read(&state);
        cJSON_AddBoolToObject(payload, "available", state.available);
        cJSON_AddBoolToObject(payload, "pressed", state.pressed);
        cJSON_AddNumberToObject(payload, "x", state.x);
        cJSON_AddNumberToObject(payload, "y", state.y);
        cJSON_AddNumberToObject(payload, "width", E100X_WIDTH);
        cJSON_AddNumberToObject(payload, "height", E100X_HEIGHT);
        cJSON_AddNumberToObject(payload, "sensor_width", state.sensor_width);
        cJSON_AddNumberToObject(payload, "sensor_height", state.sensor_height);
        return result;
    }
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    if (strcmp(command, "led.read") == 0 || strcmp(command, "led.set") == 0) {
        bool on;
        if (strcmp(command, "led.set") == 0) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "on");
            if (!cJSON_IsBool(value)) return reterminal_e100x_error("invalid_param", "boolean on required");
            if (!reterminal_e100x_led_set(cJSON_IsTrue(value))) return reterminal_e100x_error("unavailable", "user LED unavailable");
        }
        if (!reterminal_e100x_led_read(&on)) return reterminal_e100x_error("unavailable", "user LED unavailable");
        cJSON *result = reterminal_e100x_reply();
        cJSON_AddBoolToObject(cJSON_GetObjectItem(result, "payload"), "on", on);
        return result;
    }
#endif
    if (!request_id || !generation || strlen(request_id) >= 64 || strlen(command) >= 32) {
        return reterminal_e100x_error("invalid_param", "hardware operation requires a valid cloud request");
    }
    command_args_t *args = calloc(1, sizeof(*args));
    cJSON *async = cJSON_CreateObject();
    if (!args || !async || !cJSON_AddBoolToObject(async, "_async", true)) {
        free(args); cJSON_Delete(async);
        return reterminal_e100x_error("out_of_memory", "cannot allocate hardware task");
    }
    if (params && !(args->params = cJSON_Duplicate(params, true))) {
        free(args); cJSON_Delete(async);
        return reterminal_e100x_error("out_of_memory", "cannot copy hardware parameters");
    }
    strcpy(args->command, command);
    strcpy(args->request_id, request_id);
    args->generation = generation;
    portENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    if (!busy) s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (!busy && xTaskCreate(command_task, "e100x_command", 8192, args, 2, NULL) == pdPASS) return async;
    if (!busy) {
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
    }
    cJSON_Delete(args->params); free(args); cJSON_Delete(async);
    return reterminal_e100x_error(busy ? "busy" : "out_of_memory", "hardware task busy or unavailable");
}

cJSON *reterminal_e100x_command(const char *command, const cJSON *params,
                              const char *request_id,
                              noise_ctrl_session_generation_t generation) {
    // Passive diagnostics report the current owner count, excluding themselves.
    if (strcmp(command, "events.read") == 0 || strcmp(command, "events.wait") == 0
        || strcmp(command, "power.status") == 0)
        return command_unprotected(command, params, request_id, generation);
    if (!link_power_acquire(LINK_POWER_HARDWARE_COMMAND))
        return reterminal_e100x_error("busy", "hardware ownership unavailable");
    cJSON *result = command_unprotected(command, params, request_id, generation);
    const cJSON *async = cJSON_GetObjectItemCaseSensitive(result, "_async");
    // Async ownership transfers to command_task, including response delivery.
    if (!cJSON_IsTrue(async)) link_power_release(LINK_POWER_HARDWARE_COMMAND);
    return result;
}

static cJSON *parameter(const char *type, const char *description) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", type);
    cJSON_AddStringToObject(p, "description", description);
    return p;
}

static void advertise(cJSON *commands, const char *name, const char *description,
                      cJSON *required, cJSON *optional) {
    cJSON *c = cJSON_AddObjectToObject(commands, name);
    cJSON_AddStringToObject(c, "description", description);
    cJSON_AddItemToObject(c, "required", required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(c, "optional", optional ? optional : cJSON_CreateObject());
}

void reterminal_e100x_register(cJSON *commands) {
    advertise(commands, "power.status", "On-demand E100x power state. Light Sleep counts and time require the opt-in ESP-IDF PM profiler; no per-sleep logging.", NULL, NULL);
#if RETERMINAL_E100X_INPUT_ENABLED
    advertise(commands, "buttons.read", "KEY0 GPIO3, KEY1 GPIO4, KEY2 GPIO5 state and press counts. KEY0 retains pairing/reset gestures. Other actions are caller-defined.", NULL, NULL);
    cJSON *cursor = cJSON_CreateObject();
    cJSON_AddItemToObject(cursor, "after", parameter("integer", "Last next_sequence; 0 replays history. Omit for current cursor; reset after reboot."));
    advertise(commands, "events.read", "Read retained button.changed (key,pressed,presses,gpio), sd.changed (inserted)"
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
              " and touch.changed (pressed,x,y,gpio)"
#endif
              " events. Payload: events,next_sequence,has_more,history_lost,cursor_reset. Latest 32 events in RAM, batches of 16.", NULL, cursor);
    cJSON *wait = cJSON_Duplicate(cursor, true);
    cJSON_AddItemToObject(wait, "timeout_ms", parameter("integer", "1..30000; default 25000."));
    advertise(commands, "events.wait", "Wait for changes; same payload as events.read, empty events on timeout. One wait at a time. Cloud listener repeats with next_sequence; pressed=true identifies a press. No local actions installed.", NULL, wait);
    cJSON_AddNumberToObject(cJSON_GetObjectItem(commands, "events.wait"), "timeout_ms", 35000);
#endif
#if RETERMINAL_E100X_HARDWARE_ENABLED
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    const char *battery_description = "GPIO1/40 voltage and E1003-specific voltage-curve percentage; null until sampled. No capacity estimate.";
#else
    const char *battery_description = "Cached battery_mv and estimated battery_pct using Seeed's E1001/E1002 voltage curve and averaged ADC samples. Sampled every 30s, null if unavailable. Not charging state.";
#endif
    advertise(commands, "battery.read", battery_description, NULL, NULL);
    cJSON *beep = cJSON_CreateObject();
    cJSON_AddItemToObject(beep, "frequency_hz", parameter("integer", "100..5000 Hz."));
    cJSON_AddItemToObject(beep, "duration_ms", parameter("integer", "20..1000 ms."));
    advertise(commands, "buzzer.beep", "Play one bounded tone.", beep, NULL);
    advertise(commands, "rtc.read", "Read PCF8563 calendar and valid flag. No timezone; no hardware alarm/wakeup connection.", NULL, NULL);
    cJSON *time = cJSON_CreateObject();
    cJSON_AddItemToObject(time, "year", parameter("integer", "2000..2099."));
    cJSON_AddItemToObject(time, "month", parameter("integer", "1..12."));
    cJSON_AddItemToObject(time, "day", parameter("integer", "Valid day of month."));
    cJSON_AddItemToObject(time, "hour", parameter("integer", "0..23."));
    cJSON_AddItemToObject(time, "minute", parameter("integer", "0..59."));
    cJSON_AddItemToObject(time, "second", parameter("integer", "0..59."));
    advertise(commands, "rtc.set", "Set RTC time in the caller's intended timezone.", time, NULL);
    advertise(commands, "sd.status", "Report FAT card detection, mount status and capacity. Never formats.", NULL, NULL);
    cJSON *list = cJSON_CreateObject();
    cJSON_AddItemToObject(list, "path", parameter("string", "Relative directory; default card root."));
    advertise(commands, "sd.list", "List up to 32 directory names.", NULL, list);
    cJSON *read = cJSON_CreateObject();
    cJSON_AddItemToObject(read, "path", parameter("string", "Relative file path on SD."));
    cJSON *write = cJSON_Duplicate(read, true);
    cJSON_AddItemToObject(write, "text", parameter("string", "UTF-8 text, at most 2048 bytes; overwrites file."));
    advertise(commands, "sd.read_text", "Read a text file, at most 2048 bytes.", read, NULL);
    advertise(commands, "sd.write_text", "Write a text file; parent directory must exist.", write, NULL);
    cJSON *path = cJSON_CreateObject();
    cJSON_AddItemToObject(path, "path", parameter("string", "Relative file path on SD."));
    cJSON *offset = cJSON_CreateObject();
    cJSON_AddItemToObject(offset, "offset", parameter("integer", "Byte offset 0..2147483647; default 0."));
    cJSON *binary_write = cJSON_Duplicate(path, true);
    cJSON_AddItemToObject(binary_write, "data_base64", parameter("string", "Base64 of 1..1024 binary bytes."));
    cJSON *write_offset = cJSON_Duplicate(offset, true);
    advertise(commands, "sd.read", "Read up to 1024 binary bytes. Payload: data_base64,bytes,next_offset,eof.", path, offset);
    advertise(commands, "sd.write", "Write binary chunk. Offset 0 truncates/creates file; positive offset writes into existing file. Payload: bytes,next_offset.", binary_write, write_offset);
    cJSON *mic = cJSON_CreateObject();
    cJSON_AddItemToObject(mic, "duration_ms", parameter("integer", "100..500 ms."));
    advertise(commands, "microphone.record", "On-demand U18 PDM capture. Payload: audio_base64, format=pcm_s16le, sample_rate_hz=16000, channels=1, duration_ms,bytes. Mic powers off after capture. No continuous recording or recognition.", mic, NULL);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
    advertise(commands, "touch.read", "Latest GT911 single-contact state in 1872x1404 panel coordinates; available=false if probe failed.", NULL, NULL);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
    advertise(commands, "pmic.read", "Read-only SY6974B input source, charge state and raw status/fault registers; no charger settings changed.", NULL, NULL);
    advertise(commands, "led.read", "Read commanded state of the active-low GPIO16 user LED.", NULL, NULL);
    cJSON *led = cJSON_CreateObject();
    cJSON_AddItemToObject(led, "on", parameter("boolean", "true lights the user LED"));
    advertise(commands, "led.set", "Set the active-low GPIO16 user LED; no status animation is installed.", led, NULL);
#endif
}
