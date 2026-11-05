#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "reterminal_e100x.h"

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool s_fail_register_alloc;
static void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return s_fail_register_alloc ? nullptr : malloc(size);
}
#define ESP_LOGI(tag, fmt, ...) ((void)sizeof(std::fprintf(stderr, fmt, ##__VA_ARGS__)))
#define ESP_LOGE(tag, fmt, ...) ((void)sizeof(std::fprintf(stderr, fmt, ##__VA_ARGS__)))


#define CONFIG_HOMEHUB_DISPLAY_COMMANDS 1
#define CONFIG_HOMEHUB_RETERMINAL_SHT4X 1
#define CONFIG_HOMEHUB_TUNNEL 1
static char s_wifi_ssid[33], s_register_req_id[40];
static char s_node_id[64], s_display_name[64];
struct esp_app_desc_t { char version[32]; };
static const esp_app_desc_t *esp_app_get_description() {
    static esp_app_desc_t desc = {"999.0.0"};
    return &desc;
}
static void copy_wifi_ssid(char *dst, size_t size) { snprintf(dst, size, "%s", s_wifi_ssid); }
static void make_uuid(char *dst, size_t size) { snprintf(dst, size, "00000000-0000-0000-0000-000000000000"); }
static bool led_status_display_info(int *w, int *h) {
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
    *w = 1872; *h = 1404;
#else
    *w = 800; *h = 480;
#endif
    return true;
}
static int led_status_display_bits() {
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6
    return 4;
#else
    return 1;
#endif
}
static bool ota_is_enabled() { return true; } // Include optional OTA in size budget.
#include "noise_register.inc"
#include "noise_result.inc"

extern "C" void check_registration(void) {
    memset(s_wifi_ssid, 's', sizeof(s_wifi_ssid) - 1);
    memset(s_node_id, 'n', sizeof(s_node_id) - 1);
    memset(s_display_name, 'd', sizeof(s_display_name) - 1);
    char *json = build_register_json();
    if (json) fprintf(stderr, "register_bytes=%zu\n", strlen(json));
    assert(json && strlen(json) < 16384);
    cJSON *root = cJSON_Parse(json);
    cJSON *commands = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "params"), "commands_v2");
    assert(cJSON_GetObjectItem(commands, "display.draw_file"));
    assert(cJSON_GetObjectItem(commands, "power.status"));
    assert(!cJSON_GetObjectItem(commands, "power.deep_sleep"));
    assert(!!cJSON_GetObjectItem(commands, "microphone.record") == RETERMINAL_E100X_HARDWARE_ENABLED);
    assert(!!cJSON_GetObjectItem(commands, "events.wait") == RETERMINAL_E100X_INPUT_ENABLED);
    assert(!cJSON_GetObjectItem(commands, "display.pages.set"));
    assert(!cJSON_GetObjectItem(commands, "sd.display"));
    char *snapshot = cJSON_PrintUnformatted(commands);
    assert(snapshot); puts(snapshot); free(snapshot);
    cJSON_Delete(root);
    free(json);
    s_fail_register_alloc = true;
    assert(!build_register_json());
    s_fail_register_alloc = false;
}

extern "C" void check_hardware_wire(cJSON *result) {
    cJSON *expected = cJSON_GetObjectItem(result, "payload");
    char *wire = wrap_result_json("hardware-id", cJSON_Duplicate(result, true));
    assert(wire);
    cJSON *actual = cJSON_Parse(wire);
    if (expected) assert(cJSON_Compare(expected, cJSON_GetObjectItem(actual, "payload"), true));
    cJSON_Delete(actual);
    free(wire);
}
