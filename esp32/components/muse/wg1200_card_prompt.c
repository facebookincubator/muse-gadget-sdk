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

/*
 * WeatherXM WG1200 swipe-down card prompt template, kept in muse_nvs
 * (namespace "wxm_weather", key "card_prompt"). See wg1200_card_prompt.h.
 */

#include "wg1200_card_prompt.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "wg1200_prompt";

#define NVS_NS "wxm_weather"
#define NVS_KEY "card_prompt"

/* The stored template, or "" when none is (then the default applies). */
static void load_custom(char *out, size_t cap)
{
    out[0] = '\0';
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = cap;
    if (nvs_get_str(h, NVS_KEY, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
}

static bool save_custom(const char *text)
{
    nvs_handle_t h = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = text[0] ? nvs_set_str(h, NVS_KEY, text) : nvs_erase_key(h, NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* resetting a template that was never set */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void wg1200_card_prompt_get(char *out, size_t cap)
{
    load_custom(out, cap);
    if (!out[0]) {
        strlcpy(out, WG1200_CARD_PROMPT_DEFAULT, cap);
    }
}

/* One @card.prompt line on the console: {"custom":…,"len":…,"text":"…"}. */
static void print_template(void)
{
    char *text = malloc(WG1200_CARD_PROMPT_MAX + 1);
    if (!text) {
        return;
    }
    load_custom(text, WG1200_CARD_PROMPT_MAX + 1);
    bool custom = text[0] != '\0';
    if (!custom) {
        strlcpy(text, WG1200_CARD_PROMPT_DEFAULT, WG1200_CARD_PROMPT_MAX + 1);
    }
    printf("@card.prompt {\"custom\":%s,\"len\":%u,\"text\":\"", custom ? "true" : "false",
           (unsigned)strlen(text));
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            printf("\\%c", *p);
        } else if ((unsigned char)*p < 0x20) {
            printf("\\u%04x", (unsigned char)*p);
        } else {
            putchar(*p);
        }
    }
    printf("\"}\n");
    fflush(stdout);
    free(text);
}

bool wg1200_card_prompt_command(const char *cmd, const char *value, const char **result)
{
    static char answer[64];
    bool append = !strcmp(cmd, "card.prompt+");
    if (!strcmp(cmd, "card.prompt?")) {
        print_template();
        *result = "ok";
        return true;
    }
    if (!append && strcmp(cmd, "card.prompt") != 0) {
        return false;
    }

    char *text = malloc(WG1200_CARD_PROMPT_MAX + 1);
    if (!text) {
        *result = "error: out of memory";
        return true;
    }
    load_custom(text, WG1200_CARD_PROMPT_MAX + 1);
    if (!wg1200_card_prompt_apply(text, WG1200_CARD_PROMPT_MAX + 1, append, value)) {
        *result = append && !text[0] ? "error: send card.prompt= first"
                                     : "error: prompt too long (max 1023)";
    } else if (!save_custom(text)) {
        *result = "error: could not save";
    } else if (text[0]) {
        snprintf(answer, sizeof(answer), "ok: %u chars", (unsigned)strlen(text));
        *result = answer;
        ESP_LOGI(TAG, "swipe-down card prompt set (%u chars)", (unsigned)strlen(text));
    } else {
        *result = "ok: default";
        ESP_LOGI(TAG, "swipe-down card prompt reset to the default");
    }
    free(text);
    return true;
}
