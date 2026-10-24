#include <assert.h>
#include <string.h>

#include "reterminal_e100x_events.h"
#include "freertos/task.h"
#include "reterminal_e100x.h"

void check_wire_reply(cJSON *result);

static cJSON *data(cJSON *result) {
    cJSON *payload = cJSON_GetObjectItem(result, "payload");
    return payload ? payload : result;
}

static int64_t s_time;
static TaskFunction_t s_task;
static void *s_arg;
static cJSON *s_reply;
static bool s_connected = true;
static bool s_task_fail;

int64_t esp_timer_get_time(void) { return s_time; }
void vTaskDelay(int ticks) { s_time += ticks * 1000; }
void vTaskDelete(void *task) { (void)task; }
int xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle) {
    (void)name; (void)stack; (void)priority; (void)handle;
    if (s_task_fail) return 0;
    s_task = task;
    s_arg = arg;
    return 1;
}
bool noise_ctrl_is_connected(void) { return s_connected; }
void noise_ctrl_send_command_result(noise_ctrl_session_generation_t generation,
                                    const char *id, cJSON *result) {
    assert(generation == 42);
    assert(strcmp(id, "wait-id") == 0);
    assert(!s_reply);
    check_wire_reply(result);
    s_reply = result;
}

static double number(cJSON *o, const char *name) {
    cJSON *n = cJSON_GetObjectItem(data(o), name);
    assert(cJSON_IsNumber(n));
    return n->valuedouble;
}
static cJSON *call(const char *command, const char *params) {
    cJSON *p = params ? cJSON_Parse(params) : NULL;
    cJSON *r = reterminal_e100x_events_command(command, p, "wait-id", 42);
    cJSON_Delete(p);
    assert(r);
    check_wire_reply(r);
    return r;
}
static void check_error(cJSON *r, const char *code) {
    assert(cJSON_IsFalse(cJSON_GetObjectItem(r, "ok")));
    cJSON *error = cJSON_GetObjectItem(r, "error");
    assert(strcmp(cJSON_GetObjectItem(error, "code")->valuestring, code) == 0);
    cJSON_Delete(r);
}

int main(void) {
    cJSON *r = call("events.read", NULL);
    assert(number(r, "next_sequence") == 0);
    cJSON_Delete(r);
    reterminal_e100x_events_button(1, 4, true, 1);
    reterminal_e100x_events_button(1, 4, false, 1);
    reterminal_e100x_events_sd(true);
    r = call("events.read", "{\"after\":0}");
    cJSON *events = cJSON_GetObjectItem(data(r), "events");
    assert(cJSON_GetArraySize(events) == 3);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(events, 0), "pressed")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(events, 1), "pressed")));
    assert(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(events, 2), "type")->valuestring, "sd.changed") == 0);
    cJSON_Delete(r);
    // Reading does not consume records; batched history reports overwritten data.
    for (int i = 0; i < 40; i++) reterminal_e100x_events_button(2, 5, i % 2, i / 2);
    r = call("events.read", "{\"after\":0}");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data(r), "history_lost")));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data(r), "has_more")));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(data(r), "events")) == 16);
    assert(number(r, "next_sequence") == 27);
    cJSON_Delete(r);
    r = call("events.read", "{\"after\":27}");
    assert(number(r, "next_sequence") == 43);
    cJSON_Delete(r);
    r = call("events.read", "{\"after\":999}");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data(r), "cursor_reset")));
    cJSON_Delete(r);
    check_error(call("events.wait", "{\"after\":-1}"), "invalid_param");
    check_error(call("events.wait", "{\"after\":1.5}"), "invalid_param");
    check_error(call("events.wait", "{\"timeout_ms\":30001}"), "invalid_param");
    r = call("events.wait", "{\"after\":43,\"timeout_ms\":100}");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "_async")));
    cJSON_Delete(r);
    check_error(call("events.wait", "{\"after\":43}"), "busy");
    // A change during a pending wait returns on its original request/session.
    reterminal_e100x_events_button(0, 3, true, 1);
    s_task(s_arg);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(data(s_reply), "events")) == 1);
    cJSON_Delete(s_reply); s_reply = NULL;
    r = call("events.wait", "{\"after\":44,\"timeout_ms\":100}");
    cJSON_Delete(r);
    s_task(s_arg);
    assert(s_time == 100000);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(data(s_reply), "events")) == 0);
    cJSON_Delete(s_reply); s_reply = NULL;
    s_task_fail = true;
    check_error(call("events.wait", "{\"after\":44}"), "out_of_memory");
    s_task_fail = false;
    r = call("events.wait", "{\"after\":44}");
    cJSON_Delete(r);
    s_connected = false;
    s_task(s_arg);
    assert(s_time == 100000);
    cJSON_Delete(s_reply);
    // The same envelope retains button, RTC and storage result fields.
    r = cJSON_Parse("{\"ok\":true,\"key1_presses\":2,\"year\":2026,\"inserted\":true,\"text\":\"hello\"}");
    r = reterminal_e100x_data_reply(r);
    assert(number(r, "key1_presses") == 2);
    assert(number(r, "year") == 2026);
    check_wire_reply(r);
    cJSON_Delete(r);
    return 0;
}
