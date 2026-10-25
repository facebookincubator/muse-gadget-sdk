/*
 * Muse Gadget TTS proxy transport (Apache License, Version 2.0).
 *
 * Worker: POST completed reply text to a private HTTPS proxy; enqueue
 * streaming MP3 pieces. The session task consumes them on its own thread,
 * preserving the original minimp3 decoder, captions and Jollybot UI.
 * The vendor API key exists only on the server (never in the ESP32).
 */
#include "muse_tts_proxy.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_MUSE_TTS_PROXY

namespace {
constexpr size_t kTextCap = 1024;       // Same per-message limit as Muse's captions.
constexpr size_t kMaxAudio = 512 * 1024; // 128 seconds at MP3 32 kb/s.
constexpr int kConnectTimeoutMs = 8000;
constexpr int kWorkerStack = 20 * 1024;
const char *const TAG = "muse_tts_proxy";

struct Job {
    uint32_t id;
    char text[kTextCap];
};
struct Response {
    uint32_t id;
    size_t bytes;
    bool mp3;
    bool failed;
};
QueueHandle_t s_jobs;
QueueHandle_t s_audio;
std::atomic<uint32_t> s_active{0};
bool s_ready;

bool still_current(uint32_t id) { return s_active.load() == id; }

bool emit_packet(muse_tts_packet_t &packet)
{
    while (still_current(packet.id)) {
        if (xQueueSend(s_audio, &packet, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    }
    return false;
}

/* Executed in the HTTPS worker, never on the Muse/Noise socket task. */
esp_err_t http_event(esp_http_client_event_t *event)
{
    auto *res = static_cast<Response *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value
        && strcasecmp(event->header_key, "Content-Type") == 0) {
        res->mp3 = strncasecmp(event->header_value, "audio/mpeg", 10) == 0
                   || strncasecmp(event->header_value, "audio/mp3", 9) == 0;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (!still_current(res->id)) return ESP_FAIL;
    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK; // Never enqueue error JSON.
    if (!res->mp3 || !event->data || event->data_len < 0) {
        res->failed = true;
        return ESP_FAIL;
    }
    const auto *data = static_cast<const uint8_t *>(event->data);
    size_t left = static_cast<size_t>(event->data_len);
    if (res->bytes + left > kMaxAudio) {
        res->failed = true;
        ESP_LOGW(TAG, "TTS response exceeded audio limit");
        return ESP_FAIL;
    }
    while (left) {
        muse_tts_packet_t packet{};
        packet.kind = MUSE_TTS_DATA;
        packet.id = res->id;
        packet.len = left > MUSE_TTS_PACKET_BYTES ? MUSE_TTS_PACKET_BYTES : left;
        memcpy(packet.bytes, data, packet.len);
        if (!emit_packet(packet)) return ESP_FAIL;
        data += packet.len;
        left -= packet.len;
        res->bytes += packet.len;
    }
    return ESP_OK;
}

void fetch_speech(const Job &job)
{
    if (!still_current(job.id)) return;

    cJSON *body_obj = cJSON_CreateObject();
    if (!body_obj) return;
    cJSON_AddStringToObject(body_obj, "text", job.text);
    char *body = cJSON_PrintUnformatted(body_obj);
    cJSON_Delete(body_obj);

    Response response{job.id, 0, false, false};
    bool ok = false;
    if (body) {
        esp_http_client_config_t cfg{};
        cfg.url = CONFIG_MUSE_TTS_PROXY_URL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.event_handler = http_event;
        cfg.user_data = &response;
        cfg.timeout_ms = kConnectTimeoutMs;
        cfg.buffer_size = 2048;
        cfg.buffer_size_tx = 2048;
        cfg.disable_auto_redirect = true; // Never forward a bearer token to redirects.
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            char bearer[256];
            int used = snprintf(bearer, sizeof(bearer), "Bearer %s", CONFIG_MUSE_TTS_PROXY_TOKEN);
            if (used > 0 && static_cast<size_t>(used) < sizeof(bearer)) {
                esp_http_client_set_method(client, HTTP_METHOD_POST);
                esp_http_client_set_header(client, "Authorization", bearer);
                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_post_field(client, body, strlen(body));
                esp_err_t result = esp_http_client_perform(client);
                int status = esp_http_client_get_status_code(client);
                ok = result == ESP_OK && status == 200 && response.mp3 && !response.failed && response.bytes > 0;
                if (!ok && still_current(job.id)) ESP_LOGW(TAG, "proxy TTS failed (HTTP %d, ESP %s)", status, esp_err_to_name(result));
            }
            memset(bearer, 0, sizeof(bearer));
            esp_http_client_cleanup(client);
        }
        cJSON_free(body);
    }

    muse_tts_packet_t end{};
    end.id = job.id;
    end.kind = ok ? MUSE_TTS_DONE : MUSE_TTS_FAILED;
    if (still_current(job.id)) emit_packet(end);
}

void worker(void *)
{
    Job job{};
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) == pdTRUE && still_current(job.id)) {
            fetch_speech(job);
        }
    }
}

} // namespace

extern "C" bool muse_tts_proxy_init(void)
{
    if (s_ready) return true;
    // Host verification is mandatory: a publicly trusted TLS certificate and
    // a separate proxy access token. Never allow unauthenticated HTTP.
    const char *url = CONFIG_MUSE_TTS_PROXY_URL;
    const char *secret = CONFIG_MUSE_TTS_PROXY_TOKEN;
    if (strncmp(url, "https://", 8) != 0 || strlen(secret) < 24) {
        ESP_LOGW(TAG, "HTTPS URL + >=24-character proxy token required; keeping silent captions");
        return false;
    }
    s_jobs = xQueueCreateWithCaps(1, sizeof(Job), MALLOC_CAP_SPIRAM);
    s_audio = xQueueCreateWithCaps(16, sizeof(muse_tts_packet_t), MALLOC_CAP_SPIRAM);
    if (!s_jobs || !s_audio ||
        xTaskCreatePinnedToCoreWithCaps(worker, "muse_tts", kWorkerStack, nullptr, 4, nullptr, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "couldn't initialize TTS worker");
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ElevenLabs proxy transport ready (HTTPS)");
    return true;
}
extern "C" bool muse_tts_proxy_ready(void) { return s_ready; }

extern "C" bool muse_tts_proxy_submit(uint32_t id, const char *text)
{
    if (!s_ready || !id || !text || !text[0]) return false;
    Job job{};
    job.id = id;
    strlcpy(job.text, text, sizeof(job.text));
    s_active.store(id);
    return xQueueOverwrite(s_jobs, &job) == pdTRUE;
}
extern "C" bool muse_tts_proxy_poll(muse_tts_packet_t *packet)
{
    return s_ready && packet && xQueueReceive(s_audio, packet, 0) == pdTRUE;
}
extern "C" void muse_tts_proxy_cancel(void) { s_active.store(0); }

#endif // CONFIG_MUSE_TTS_PROXY
