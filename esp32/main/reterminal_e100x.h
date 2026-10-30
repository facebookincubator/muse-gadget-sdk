// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "sdkconfig.h"
#include "cJSON.h"
#include "noise_control.h"

#if CONFIG_HOMEHUB_RETERMINAL_E100X_HARDWARE || CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT
#define RETERMINAL_E100X_INPUT_ENABLED 1
#else
#define RETERMINAL_E100X_INPUT_ENABLED 0
#endif
#if CONFIG_HOMEHUB_RETERMINAL_E100X_HARDWARE || CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE
#define RETERMINAL_E100X_HARDWARE_ENABLED 1
#else
#define RETERMINAL_E100X_HARDWARE_ENABLED 0
#endif
#ifdef __cplusplus
extern "C" {
#endif
void reterminal_e100x_prepare_display(void);
void reterminal_e100x_init(void);
void reterminal_e100x_register(cJSON *commands);
bool reterminal_e100x_handles(const char *command);
cJSON *reterminal_e100x_command(const char *command, const cJSON *params,
    const char *request_id, noise_ctrl_session_generation_t generation);
void reterminal_e100x_health(cJSON *metrics);
void reterminal_e100x_battery_metrics(cJSON *payload);
#ifdef __cplusplus
}
#endif

// link.result transports only ok, payload and error. Keep hardware data in
// payload; otherwise wrap_result_json discards top-level result fields.
// Consumes result. Failures and acknowledgement-only replies stay unchanged.
static inline cJSON *reterminal_e100x_data_reply(cJSON *result) {
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(result, "ok");
    if (!cJSON_IsTrue(ok) || cJSON_GetArraySize(result) <= 1) return result;
    cJSON *reply = cJSON_CreateObject();
    if (!reply) {
        cJSON_Delete(result);
        return NULL;
    }
    ok = cJSON_DetachItemFromObjectCaseSensitive(result, "ok");
    if (!cJSON_AddItemToObject(reply, "ok", ok)) {
        cJSON_Delete(ok);
        cJSON_Delete(result);
        cJSON_Delete(reply);
        return NULL;
    }
    if (!cJSON_AddItemToObject(reply, "payload", result)) {
        cJSON_Delete(result);
        cJSON_Delete(reply);
        return NULL;
    }
    return reply;
}

// Common Link envelopes. data_reply consumes a legacy flat data object.
static inline cJSON *reterminal_e100x_error(const char *code, const char *message) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON *e = cJSON_AddObjectToObject(r, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    return r;
}
static inline cJSON *reterminal_e100x_ack(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return r;
}
static inline cJSON *reterminal_e100x_reply(void) {
    cJSON *r = reterminal_e100x_ack();
    cJSON_AddObjectToObject(r, "payload");
    return r;
}
