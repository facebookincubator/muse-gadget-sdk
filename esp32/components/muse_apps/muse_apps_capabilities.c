/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Runtime discovery for device.guide/dev_describe; app.api serves only API parts.
 * Descriptors only: no sensor reads, driver calls, network or LVGL lock. */
#include "muse_apps_priv.h"
#include "muse_app_view.h"
#include "muse_board.h"
#include "muse_settings.h"
#include "muse_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPABILITIES_PART_BYTES 6000
extern const uint8_t api_capabilities_json_start[] asm("_binary_api_capabilities_json_start");
extern const uint8_t api_capabilities_json_end[] asm("_binary_api_capabilities_json_end");

/* Fail as a whole, never report an incomplete descriptor as usable hardware. */
#define JSON_REQUIRE(expr) do { if (!(expr)) goto fail; } while (0)

static cJSON *range_json(const capability_field_t *f)
{
    cJSON *o = cJSON_CreateObject();
    JSON_REQUIRE(cJSON_AddStringToObject(o, "name", f->name));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "unit", f->unit ? f->unit : ""));
    if (f->max > f->min) {
        JSON_REQUIRE(cJSON_AddNumberToObject(o, "min", f->min));
        JSON_REQUIRE(cJSON_AddNumberToObject(o, "max", f->max));
    }
    return o;
fail:
    cJSON_Delete(o);
    return NULL;
}

/* AddItem transfers ownership only on success. */
static bool add_array_item(cJSON *array, cJSON *item)
{
    if (item && cJSON_AddItemToArray(array, item)) return true;
    cJSON_Delete(item);
    return false;
}

cJSON *muse_apps_describe_device(const capability_t *d)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *action = NULL;
    JSON_REQUIRE(cJSON_AddStringToObject(o, "name", d->name));
    JSON_REQUIRE(cJSON_AddStringToObject(o, "desc", d->desc ? d->desc : ""));
    cJSON *fields = cJSON_AddArrayToObject(o, "fields");
    JSON_REQUIRE(fields);
    for (int i = 0; i < d->n_fields; i++) {
        JSON_REQUIRE(add_array_item(fields, range_json(&d->fields[i])));
    }
    JSON_REQUIRE(cJSON_AddNumberToObject(o, "rate", d->rate));
    cJSON *actions = cJSON_AddArrayToObject(o, "actions");
    JSON_REQUIRE(actions);
    for (int i = 0; i < d->n_actions; i++) {
        const capability_action_t *a = &d->actions[i];
        action = cJSON_CreateObject();
        JSON_REQUIRE(cJSON_AddStringToObject(action, "name", a->name));
        JSON_REQUIRE(cJSON_AddStringToObject(action, "desc", a->desc ? a->desc : ""));
        cJSON *params = cJSON_AddArrayToObject(action, "params");
        JSON_REQUIRE(params);
        for (int k = 0; k < a->n_params; k++) {
            JSON_REQUIRE(add_array_item(params, range_json(&a->params[k])));
        }
        if (a->text) JSON_REQUIRE(cJSON_AddBoolToObject(action, "text", true));
        JSON_REQUIRE(cJSON_AddItemToArray(actions, action));
        action = NULL;
    }
    cJSON *events = cJSON_AddArrayToObject(o, "events");
    JSON_REQUIRE(events);
    for (int i = 0; i < d->n_events; i++) {
        JSON_REQUIRE(add_array_item(events, cJSON_CreateString(d->events[i])));
    }
    return o;
fail:
    cJSON_Delete(action);
    cJSON_Delete(o);
    return NULL;
}

static size_t api_bytes(void)
{
    size_t n = (size_t)(api_capabilities_json_end - api_capabilities_json_start);
    while (n && (api_capabilities_json_start[n - 1] == '\n' || api_capabilities_json_start[n - 1] == '\0')) n--;
    return n;
}

cJSON *muse_apps_capabilities(void)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *board = cJSON_AddObjectToObject(payload, "board");
    JSON_REQUIRE(board);
    int w, h;
    bool round;
    muse_app_view_size(&w, &h, &round);
    JSON_REQUIRE(cJSON_AddStringToObject(board, "name", muse_board ? muse_board->name : "?"));
    JSON_REQUIRE(cJSON_AddNumberToObject(board, "width", w));
    JSON_REQUIRE(cJSON_AddNumberToObject(board, "height", h));
    JSON_REQUIRE(cJSON_AddBoolToObject(board, "round", round));
    JSON_REQUIRE(cJSON_AddBoolToObject(board, "touch", muse_board && muse_board->touch));
    JSON_REQUIRE(cJSON_AddNumberToObject(board, "safe_inset", muse_apps_safe_inset()));
    cJSON *buttons = cJSON_AddArrayToObject(board, "buttons");
    JSON_REQUIRE(buttons);
    static const char *const roles[] = {NULL, "primary", "secondary", "up", "down", "left", "right", "enter", "back"};
    unsigned present = muse_app_view_buttons_present();
    for (int i = 1; i <= MUSE_APP_BUTTON_BACK; i++) {
        if (present & (1u << i)) JSON_REQUIRE(add_array_item(buttons, cJSON_CreateString(roles[i])));
    }
    JSON_REQUIRE(cJSON_AddStringToObject(board, "exit_hold", roles[muse_app_view_exit_button()]));
    cJSON *devices = cJSON_AddArrayToObject(payload, "devices");
    JSON_REQUIRE(devices);
    for (int i = 0; i < capabilities_registry_count(); i++) {
        JSON_REQUIRE(add_array_item(devices, muse_apps_describe_device(capabilities_registry_at(i))));
    }
    cJSON *wasm = cJSON_AddObjectToObject(payload, "wasm");
    JSON_REQUIRE(wasm);
    char version[16];
    snprintf(version, sizeof(version), "%d.%d", MUSE_APPS_API_MAJOR, MUSE_APPS_API_MINOR);
    JSON_REQUIRE(cJSON_AddBoolToObject(wasm, "enabled", muse_settings_apps_on()));
    JSON_REQUIRE(cJSON_AddStringToObject(wasm, "api", version));
    JSON_REQUIRE(cJSON_AddNumberToObject(wasm, "budget_kb", (double)(muse_apps_mem_budget() / 1024)));
    JSON_REQUIRE(cJSON_AddStringToObject(wasm, "device_dir", muse_apps_device_dir()));
    JSON_REQUIRE(cJSON_AddStringToObject(wasm, "api_command", "app.api"));
    JSON_REQUIRE(cJSON_AddStringToObject(wasm, "api_file", "api.capabilities.json"));
    JSON_REQUIRE(cJSON_AddNumberToObject(wasm, "api_parts", (api_bytes() + CAPABILITIES_PART_BYTES - 1) / CAPABILITIES_PART_BYTES));
    return payload;
fail:
    cJSON_Delete(payload);
    return NULL;
}

bool muse_apps_add_capabilities(cJSON *payload)
{
    cJSON *capabilities = muse_apps_capabilities();
    if (capabilities && cJSON_AddItemToObject(payload, "capabilities", capabilities)) return true;
    cJSON_Delete(capabilities);
    return false;
}

cJSON *muse_apps_api_command(cJSON *params)
{
    size_t n = api_bytes();
    int parts = (int)((n + CAPABILITIES_PART_BYTES - 1) / CAPABILITIES_PART_BYTES);
    cJSON *part = cJSON_GetObjectItemCaseSensitive(params, "part");
    if (!cJSON_IsNumber(part) || part->valuedouble != part->valueint || part->valueint < 0 || part->valueint >= parts) {
        return muse_apps_error("invalid_param", "part is required: an integer from 0 to api_parts - 1");
    }
    cJSON *payload = NULL, *result = cJSON_CreateObject();
    char *text = NULL;
    JSON_REQUIRE(cJSON_AddBoolToObject(result, "ok", true));
    payload = cJSON_CreateObject();
    size_t off = (size_t)part->valueint * CAPABILITIES_PART_BYTES;
    size_t len = n - off < CAPABILITIES_PART_BYTES ? n - off : CAPABILITIES_PART_BYTES;
    text = malloc(len + 1);
    JSON_REQUIRE(text);
    memcpy(text, api_capabilities_json_start + off, len);
    text[len] = '\0';
    JSON_REQUIRE(cJSON_AddNumberToObject(payload, "part", part->valueint));
    JSON_REQUIRE(cJSON_AddNumberToObject(payload, "parts", parts));
    JSON_REQUIRE(cJSON_AddStringToObject(payload, "text", text));
    JSON_REQUIRE(cJSON_AddItemToObject(result, "payload", payload));
    free(text);
    return result;
fail:
    free(text);
    cJSON_Delete(payload);
    cJSON_Delete(result);
    return muse_apps_error("out_of_memory", "could not build the API part");
}
