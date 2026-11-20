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

#include "wifi_known.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "link.wifi_known";

#define SSID_KEY     "ssid"
#define PASSWORD_KEY "password"
#define HIDDEN_KEY   "wifi_hidden"
#define OTHERS_KEY   "wifi_others"
#define CHANNEL_KEY  "wifi_channel"
#define PROXY_KEY    "wifi_proxy"
#define PROXY_JSON_MAX 1280

// The NVS string limit; saving drops the oldest networks until their
// credentials and proxy settings fit.
#define OTHERS_JSON_MAX 4000

static SemaphoreHandle_t s_lock;

static void lock(void) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

static void wipe(void *buf, size_t len) {
    volatile unsigned char *p = buf;
    while (len--) *p++ = 0;
}

void wifi_known_wipe(wifi_known_list_t *list) {
    if (list) wipe(list, sizeof(*list));
}

void wifi_known_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static bool valid(const char *ssid, const char *password) {
    return ssid && ssid[0] && strlen(ssid) <= WIFI_KNOWN_SSID_MAX
           && strlen(password ? password : "") <= WIFI_KNOWN_PASS_MAX;
}

static int find(const wifi_known_list_t *list, const char *ssid) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

// Adds a network at the end, unless the list is full or already has it.
static void append(wifi_known_list_t *list, const char *ssid,
                   const char *password, bool hidden) {
    if (list->count >= WIFI_KNOWN_MAX || !valid(ssid, password)
        || find(list, ssid) >= 0) {
        return;
    }
    wifi_known_net_t *net = &list->nets[list->count++];
    memset(net, 0, sizeof(*net));
    strcpy(net->ssid, ssid);
    strcpy(net->password, password ? password : "");
    net->hidden = hidden;
}

// Frees a parsed or built list, clearing the passwords cJSON copied.
static void delete_json(cJSON *root) {
    network_proxy_delete_json(root);
}

static void free_json_text(char *text) {
    if (!text) return;
    wipe(text, strlen(text));
    cJSON_free(text);
}

static void load_locked(wifi_known_list_t *list) {
    memset(list, 0, sizeof(*list));
    char ssid[WIFI_KNOWN_SSID_MAX + 1] = {0};
    char pass[WIFI_KNOWN_PASS_MAX + 1] = {0};
    char hidden[2] = {0};
    if (config_get_str(SSID_KEY, ssid, sizeof(ssid)) && ssid[0]) {
        if (!config_get_str(PASSWORD_KEY, pass, sizeof(pass))) pass[0] = '\0';
        append(list, ssid, pass,
               config_get_str(HIDDEN_KEY, hidden, sizeof(hidden))
               && hidden[0] == '1');
        wipe(pass, sizeof(pass));
    }

    char *json = malloc(OTHERS_JSON_MAX);
    if (!json) {
        if (list->count) list->nets[0].proxy.type = NETWORK_PROXY_INVALID;
        ESP_LOGW(TAG, "no memory for the other saved networks");
        return;
    }
    if (list->count && config_key_lookup(PROXY_KEY) != CONFIG_KEY_NOT_FOUND) {
        cJSON *proxy = config_get_str(PROXY_KEY, json, PROXY_JSON_MAX) ? cJSON_Parse(json) : NULL;
        cJSON *owner = cJSON_GetObjectItemCaseSensitive(proxy, "ssid");
        if (!proxy || !cJSON_IsString(owner)) {
            list->nets[0].proxy.type = NETWORK_PROXY_INVALID;
        } else if (!strcmp(owner->valuestring, list->nets[0].ssid)) {
            network_proxy_parse(proxy, &list->nets[0].proxy);
        }
        // Older firmware can change the first SSID while ignoring this key.
        // Its stale proxy must never be applied to a different network.
        delete_json(proxy);
        wipe(json, OTHERS_JSON_MAX);
    }
    if (config_get_str(OTHERS_KEY, json, OTHERS_JSON_MAX)) {
        cJSON *root = cJSON_Parse(json);
        if (cJSON_IsArray(root)) {
            cJSON *item;
            cJSON_ArrayForEach(item, root) {
                cJSON *s = cJSON_GetObjectItemCaseSensitive(item, "s");
                cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "p");
                if (!cJSON_IsString(s)) continue;
                int before = list->count;
                append(list, s->valuestring,
                       cJSON_IsString(p) ? p->valuestring : "",
                       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "h")));
                if (list->count > before) {
                    network_proxy_parse(cJSON_GetObjectItemCaseSensitive(item, "proxy"),
                                         &list->nets[before].proxy);
                }
            }
        } else if (root || json[0]) {
            ESP_LOGW(TAG, "ignoring unreadable saved networks");
        }
        delete_json(root);
    }
    wipe(json, OTHERS_JSON_MAX);
    free(json);
}

// Writes a value unless NVS already has it; *changed says which.
static bool put(const char *key, const char *value, bool *changed) {
    size_t len = strlen(value) + 2;   // one more than fits reads as different
    char *current = malloc(len);
    bool same = current && config_get_str(key, current, len)
                && strcmp(current, value) == 0;
    if (current) {
        wipe(current, len);
        free(current);
    }
    if (changed) *changed = !same;
    return same || config_set_str(key, value);
}

// The networks after the first as JSON, or NULL if there are none. Drops the
// oldest until the text fits in OTHERS_JSON_MAX.
static bool others_json(wifi_known_list_t *list, char **out) {
    *out = NULL;
    while (list->count > 1) {
        cJSON *root = cJSON_CreateArray();
        bool built = root != NULL;
        for (int i = 1; built && i < list->count; i++) {
            const wifi_known_net_t *net = &list->nets[i];
            cJSON *item = cJSON_CreateObject();
            if (!cJSON_AddItemToArray(root, item)) {
                cJSON_Delete(item);
                built = false;
                break;
            }
            built = cJSON_AddStringToObject(item, "s", net->ssid)
                    && cJSON_AddStringToObject(item, "p", net->password)
                    && (!net->hidden || cJSON_AddTrueToObject(item, "h"));
            if (built && net->proxy.type != NETWORK_PROXY_OFF) {
                cJSON *proxy = network_proxy_json(&net->proxy);
                built = proxy && cJSON_AddItemToObject(item, "proxy", proxy);
                if (!built) delete_json(proxy);
            }
        }
        char *text = built ? cJSON_PrintUnformatted(root) : NULL;
        delete_json(root);
        if (!text) return false;
        if (strlen(text) < OTHERS_JSON_MAX) {
            *out = text;
            return true;
        }
        free_json_text(text);
        list->count--;
        ESP_LOGW(TAG, "saved networks too long; forgetting %s",
                 list->nets[list->count].ssid);
        wipe(&list->nets[list->count], sizeof(list->nets[0]));
    }
    return true;
}

static bool save_locked(wifi_known_list_t *list) {
    bool ok = true;
    if (list->count == 0) {
        static const char *const keys[] = {
            SSID_KEY, PASSWORD_KEY, HIDDEN_KEY, OTHERS_KEY, CHANNEL_KEY, PROXY_KEY,
        };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (!config_erase_key(keys[i])) ok = false;
        }
        return ok;
    }

    const wifi_known_net_t *first = &list->nets[0];
    bool moved = false;
    if (!put(SSID_KEY, first->ssid, &moved)) ok = false;
    // The saved channel was the previous first network's.
    if (moved && !config_erase_key(CHANNEL_KEY)) ok = false;
    if (!put(PASSWORD_KEY, first->password, NULL)) ok = false;
    if (first->hidden ? !put(HIDDEN_KEY, "1", NULL)
                      : !config_erase_key(HIDDEN_KEY)) {
        ok = false;
    }

    if (first->proxy.type == NETWORK_PROXY_OFF) {
        if (!config_erase_key(PROXY_KEY)) ok = false;
    } else {
        cJSON *proxy = network_proxy_json(&first->proxy);
        bool built = proxy && cJSON_AddStringToObject(proxy, "ssid", first->ssid);
        char *text = built ? cJSON_PrintUnformatted(proxy) : NULL;
        if (!text || strlen(text) >= PROXY_JSON_MAX || !put(PROXY_KEY, text, NULL)) ok = false;
        delete_json(proxy);
        free_json_text(text);
    }

    char *json = NULL;
    if (!others_json(list, &json)) {
        ok = false;
    } else if (json) {
        if (!put(OTHERS_KEY, json, NULL)) ok = false;
        free_json_text(json);
    } else if (!config_erase_key(OTHERS_KEY)) {
        ok = false;
    }
    if (!ok) ESP_LOGW(TAG, "saving the Wi-Fi networks failed");
    return ok;
}

int wifi_known_load(wifi_known_list_t *list) {
    lock();
    load_locked(list);
    unlock();
    return list->count;
}

// Loads the list, lets edit() build the new one from it, and saves that.
static bool update(void (*edit)(const wifi_known_list_t *cur,
                                wifi_known_list_t *next, const void *arg),
                   const void *arg) {
    wifi_known_list_t *lists = malloc(2 * sizeof(*lists));
    if (!lists) {
        ESP_LOGW(TAG, "no memory to update the saved networks");
        return false;
    }
    lock();
    load_locked(&lists[0]);
    memset(&lists[1], 0, sizeof(lists[1]));
    edit(&lists[0], &lists[1], arg);
    bool ok = save_locked(&lists[1]);
    unlock();
    wipe(lists, 2 * sizeof(*lists));
    free(lists);
    return ok;
}

typedef struct {
    const char *ssid;
    const char *password;
    int hidden;
} remember_arg_t;

static void edit_remember(const wifi_known_list_t *cur,
                          wifi_known_list_t *next, const void *arg) {
    const remember_arg_t *r = arg;
    int at = find(cur, r->ssid);
    bool hidden = r->hidden < 0 ? at >= 0 && cur->nets[at].hidden
                                : r->hidden != 0;
    append(next, r->ssid, r->password, hidden);
    if (at >= 0) next->nets[0].proxy = cur->nets[at].proxy;
    for (int i = 0; i < cur->count; i++) {
        int before = next->count;
        append(next, cur->nets[i].ssid, cur->nets[i].password,
               cur->nets[i].hidden);
        if (next->count > before) next->nets[before].proxy = cur->nets[i].proxy;
    }
}

bool wifi_known_remember(const char *ssid, const char *password, int hidden) {
    if (!valid(ssid, password)) return false;
    remember_arg_t arg = { ssid, password ? password : "", hidden };
    return update(edit_remember, &arg);
}

static void edit_forget(const wifi_known_list_t *cur,
                        wifi_known_list_t *next, const void *arg) {
    const char *ssid = arg;
    for (int i = 0; i < cur->count; i++) {
        if (strcmp(cur->nets[i].ssid, ssid) == 0) continue;
        int before = next->count;
        append(next, cur->nets[i].ssid, cur->nets[i].password,
               cur->nets[i].hidden);
        if (next->count > before) next->nets[before].proxy = cur->nets[i].proxy;
    }
}

bool wifi_known_forget(const char *ssid) {
    if (!ssid || !ssid[0]) return true;
    return update(edit_forget, ssid);
}

static void edit_clear(const wifi_known_list_t *cur,
                       wifi_known_list_t *next, const void *arg) {
    (void)cur;
    (void)next;
    (void)arg;
}

bool wifi_known_forget_all(void) {
    return update(edit_clear, NULL);
}

bool wifi_known_get_proxy(const char *ssid, network_proxy_config_t *out) {
    memset(out, 0, sizeof(*out));
    wifi_known_list_t *list = malloc(sizeof(*list));
    if (!list) { out->type = NETWORK_PROXY_INVALID; return false; }
    wifi_known_load(list);
    int at = find(list, ssid);
    if (at >= 0) *out = list->nets[at].proxy;
    wifi_known_wipe(list);
    free(list);
    return at >= 0;
}

bool wifi_known_set_proxy(const char *ssid, const network_proxy_config_t *proxy) {
    if (!ssid || !network_proxy_valid(proxy)) return false;
    wifi_known_list_t *list = malloc(sizeof(*list));
    if (!list) return false;
    lock();
    load_locked(list);
    int at = find(list, ssid);
    bool ok = false;
    if (at >= 0) {
        list->nets[at].proxy = *proxy;
        if (proxy->type == NETWORK_PROXY_OFF) memset(&list->nets[at].proxy, 0, sizeof(*proxy));
        ok = save_locked(list);
    }
    unlock();
    wifi_known_wipe(list);
    free(list);
    return ok;
}
