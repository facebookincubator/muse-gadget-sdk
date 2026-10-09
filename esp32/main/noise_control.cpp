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

#include "noise_control.h"
#include "stack_monitor.h"
#include "noise_upgrade.h"
#include "noise_tunnel_internal.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

extern "C" {
#include "cJSON.h"
#include "battery_adc.h"
#include "led_status.h"
#include "ota.h"
#include "sht40.h"
#if CONFIG_MUSE_ENABLED
extern "C" {
#include "muse_state.h"
}
#endif
}

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

using namespace musegadgets::noise::core;

static const char *TAG = "link.noise_ctrl";

#define NOISE_DEFAULT_HOST "hatch.metaaivm.com"
static char s_noise_host[256] = NOISE_DEFAULT_HOST;
#define NOISE_PATH       "/v1/noise"
#define NOISE_PORT       443
#define CTRL_STREAM_ID   1
// One-shot GET /identity for the agent's name (TUNNEL_STREAM_ID is 2).
#define IDENTITY_STREAM_ID 3
// Without include_markdown the identity reply is ~3 KB of JSON, mostly avatar
// asset paths. Held only until it is parsed.
#define IDENTITY_BODY_MAX  8192
#if CONFIG_HOMEHUB_SHOW_AGENT_NAME
#define SHOW_AGENT_NAME 1
#else
#define SHOW_AGENT_NAME 0
#endif

#define RECONNECT_MIN_MS          1000
#define RECONNECT_MAX_MS          15000
#define RECONNECT_AUTH_FAILED_MS  60000
#define RECONNECT_DELAY_STEP_MS   250
#define KEEPALIVE_INTERVAL_MS     60000
#define POWER_SAVE_QUIET_MS       1000    // noise_ctrl_set_power_save(): quiet this long,
#define POWER_SAVE_POLL_MS        100     // then poll this often
#define IDLE_QUIET_MS             50      // otherwise quiet this long,
#define IDLE_POLL_MS              10      // then wait on the socket at most this long
#define STABLE_SESSION_MS         30000
#define VM_REFRESH_FAILURES       3

// On a board with the full UI but no PSRAM or tunnel, only control JSON and Muse's
// voice notes and live chat replies cross this session, and the whole session has
// to fit in what internal RAM Wi-Fi, TLS and the display leave, so the buffers
// below are smaller.
#if CONFIG_MUSE_ENABLED && !CONFIG_SPIRAM && !CONFIG_HOMEHUB_TUNNEL
#define SMALL_CONTROL_SESSION 1
#else
#define SMALL_CONTROL_SESSION 0
#endif

// Max inbound service frame scratch. Daemon control responses are modest JSON,
// but the tunnel stream (multiplexed on this session) carries ~8 KB IP-packet
// batches, so scratch must fit a full batch plus ServiceFrame/envelope overhead.
// Chat subscriptions deliver 16 KB body chunks plus framing, even on boards
// without PSRAM. Reserve enough inbound space for those frames. Not on the
// ESP32-C3 AI Passport: three 17 KB buffers beside its UI leave too little to
// upload a voice note. It takes 12 KB, as the PSRAM boards' session does.
// Chat events arrive one per frame; a frame over that ends the session (it
// can't be skipped: Noise's nonces and the ordered framer would desync), the
// request in flight ends with NOISE_CTRL_REQ_TOO_LARGE, and the session
// reconnects.
#if SMALL_CONTROL_SESSION && CONFIG_MUSE_BOARD_AI_PASSPORT
#define SVC_FRAME_SCRATCH (12 * 1024)
#else
#define SVC_FRAME_SCRATCH (SMALL_CONTROL_SESSION ? 17 * 1024 : 12288)
#endif

// The ADV and the ESP32-C3 AI Passport cannot allocate the session with the
// larger inbound buffers and the usual outbound buffers together. Keep this
// reduction local to those boards.
#if SMALL_CONTROL_SESSION && (CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || CONFIG_MUSE_BOARD_AI_PASSPORT)
#define CARDPUTER_CONTROL_SESSION 1
#else
#define CARDPUTER_CONTROL_SESSION 0
#endif

// Outbound scratch buffers. Sized for tunnel batches (8 KB) + framing overhead.
#define OUT_SVC_SCRATCH   (CARDPUTER_CONTROL_SESSION ? 3072 : SMALL_CONTROL_SESSION ? 6144 : 12288)
#define OUT_ENV_SCRATCH   (CARDPUTER_CONTROL_SESSION ? 3072 : SMALL_CONTROL_SESSION ? 6144 : 12288)

// Max BodyChunk payload for a single control-stream ServiceFrame. Kept well
// under the outbound scratch above so the ServiceFrame/envelope always fit;
// larger control messages (e.g. device.discover results) are split across
// multiple BodyChunks. The daemon reassembles by the u32-LE length prefix, so
// chunk boundaries are transparent. Matches the proven ~8 KB tunnel batch size.
#define CTRL_BODY_CHUNK_MAX (CARDPUTER_CONTROL_SESSION ? 2048 : SMALL_CONTROL_SESSION ? 4096 : 8192)

// WebSocket send/receive buffers. The protocol allows 64 KB frames, but
// outbound frames are bounded by OUT_ENV_SCRATCH and inbound ones by
// SVC_FRAME_SCRATCH (the decrypted frame must fit there), so without PSRAM use
// buffers just large enough for those plus framing and the AEAD tag.
#if CONFIG_SPIRAM
#define WS_BUF_SIZE ClientSession::kMaxOutboundWebSocketPayloadSize
#elif CARDPUTER_CONTROL_SESSION
#define WS_BUF_SIZE (4 * 1024)
#else
#define WS_BUF_SIZE (SMALL_CONTROL_SESSION ? 7 * 1024 : 16 * 1024)
#endif
// Inbound WebSocket frames: SVC_FRAME_SCRATCH plus framing and the AEAD tag.
#if SMALL_CONTROL_SESSION && CONFIG_MUSE_BOARD_AI_PASSPORT
#define WS_RX_BUF_SIZE (SVC_FRAME_SCRATCH + 1024)
#else
#define WS_RX_BUF_SIZE (SMALL_CONTROL_SESSION ? 17 * 1024 : WS_BUF_SIZE)
#endif

static char s_node_id[64];
// A frame too big for this board's buffers is ending the session.
static bool s_req_too_large;
static char s_display_name[64];
static char s_wifi_ssid[33];
static noise_ctrl_status_cb s_status_cb = nullptr;
static noise_ctrl_command_cb s_command_cb = nullptr;
static noise_ctrl_agent_name_cb s_agent_name_cb = nullptr;

static char s_vm_id[128];
static char *s_auth_token = nullptr;
static volatile bool s_connected = false;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_power_save{false};
// link.register request id for the current session, and whether its reply has
// arrived. The tunnel data-plane stream is only opened AFTER the register reply,
// because the server authorizes /link-tunnel by checking that a device has
// registered on this same Noise connection (see ingress-rev-proxy
// link_tunnel_authorized_by_daemon). Opening earlier races the registration and
// draws a 403 that would otherwise wait out the tunnel reopen interval (~5s+).
// Session-task-owned: written before the event loop, read/reset within it.
static char s_register_req_id[40] = {0};
static bool s_register_acked = false;
static bool s_heartbeat_registered = false;
static TaskHandle_t s_task = nullptr;

#define NOISE_CTRL_STACK 12288
#if CONFIG_MUSE_ENABLED && CONFIG_SPIRAM
// Muse's display and audio leave internal RAM fragmented by the time a session
// starts, often without a 12 KB block for this stack. Hold one from boot and
// hand it over just before the task is created. The idle task frees a
// deleted session task's stack, so let it run before either step.
static void *s_stack_reserve;
static void reserve_stack(void) {
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!s_stack_reserve) {
        s_stack_reserve = heap_caps_malloc(NOISE_CTRL_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
static void release_stack(void) {
    vTaskDelay(pdMS_TO_TICKS(20));
    free(s_stack_reserve);
    s_stack_reserve = nullptr;
}
#else
static void reserve_stack(void) {}
static void release_stack(void) {}
#endif
static SemaphoreHandle_t s_connect_mutex = nullptr;
// Only the single Noise session task advances this nonzero generation.
static noise_ctrl_session_generation_t s_session_generation_counter = 0;

// The session is owned by the session task; outbound calls from other tasks
// go through a queue.
struct pending_result {
    noise_ctrl_session_generation_t session_generation;
    char *request_id;
    cJSON *result;
};
static QueueHandle_t s_result_q = nullptr;

// Forward declarations.

// ---- helpers ----------------------------------------------------------------

static void notify_status(const char *s) {
    if (s_status_cb) s_status_cb(s);
}

static void copy_wifi_ssid(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    size_t len = strnlen(s_wifi_ssid, out_len - 1);
    memcpy(out, s_wifi_ssid, len);
    out[len] = '\0';
}

static void store_wifi_ssid(const char *ssid) {
    if (!ssid || !*ssid) {
        s_wifi_ssid[0] = '\0';
        return;
    }
    size_t len = strnlen(ssid, sizeof(s_wifi_ssid) - 1);
    memcpy(s_wifi_ssid, ssid, len);
    s_wifi_ssid[len] = '\0';
}

static constexpr int64_t WS_FRAME_TIMEOUT_US = 15000000;

static ssize_t write_all(esp_tls_t *tls, const void *buf, size_t len) {
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t off = 0;
    int64_t deadline = esp_timer_get_time() + WS_FRAME_TIMEOUT_US;
    while (off < len) {
        if (!s_running || esp_timer_get_time() >= deadline) return -1;
        ssize_t n = esp_tls_conn_write(tls, p + off, len - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return n;
        }
    }
    return static_cast<ssize_t>(off);
}

static void make_uuid(char *out, size_t n) {
    uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
    snprintf(out, n, "%08lx-%04lx-%04lx-%04lx-%04lx%08lx",
             (unsigned long)a,
             (unsigned long)(b & 0xffff),
             (unsigned long)((b >> 16) & 0x0fff) | 0x4000UL,
             (unsigned long)((c & 0x3fffUL) | 0x8000UL),
             (unsigned long)((c >> 16) & 0xffffUL),
             (unsigned long)d);
}

// ---- WebSocket framing (binary frames only) ---------------------------------

// Minimal WS client framer. We only send binary frames (opcode 0x2) with
// masking, and receive binary frames. The server (nginx) expects a standard
// WS upgrade and then passes raw binary payloads to ingress-rev-proxy.

// Send a masked binary WS frame.
static bool ws_send_binary(esp_tls_t *tls, const uint8_t *payload, size_t len) {
    // Frame header: FIN + opcode 0x2, MASK + payload length, 4-byte mask key.
    uint8_t hdr[14];
    size_t hdr_len = 0;
    hdr[0] = 0x82; // FIN | binary
    if (len < 126) {
        hdr[1] = 0x80 | static_cast<uint8_t>(len);
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1] = 0x80 | 126;
        hdr[2] = static_cast<uint8_t>((len >> 8) & 0xff);
        hdr[3] = static_cast<uint8_t>(len & 0xff);
        hdr_len = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) {
            hdr[2 + i] = static_cast<uint8_t>((len >> (56 - 8 * i)) & 0xff);
        }
        hdr_len = 10;
    }

    // Mask key
    uint32_t mask_key = esp_random();
    memcpy(hdr + hdr_len, &mask_key, 4);
    hdr_len += 4;

    if (write_all(tls, hdr, hdr_len) != static_cast<ssize_t>(hdr_len)) {
        return false;
    }

    // Mask and send payload in chunks to avoid a full copy.
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&mask_key);
    uint8_t chunk[512];
    size_t off = 0;
    while (off < len) {
        size_t n = (len - off < sizeof(chunk)) ? (len - off) : sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = payload[off + i] ^ mk[(off + i) & 3];
        }
        if (write_all(tls, chunk, n) != static_cast<ssize_t>(n)) {
            return false;
        }
        off += n;
    }
    return true;
}

// Send a zero-length masked WebSocket ping (opcode 0x9). The peer (the LB /
// ingress-rev-proxy) replies with a pong, so this generates traffic in BOTH
// directions — unlike the Noise empty-BodyChunk keepalive, which the daemon
// does not answer, leaving the downstream (server→device) path idle and prone
// to middlebox idle-timeout resets.
static bool ws_send_ping(esp_tls_t *tls) {
    // FIN | ping, MASK | len 0, then the 4-byte mask key.
    uint8_t frame[6];
    frame[0] = 0x89;
    frame[1] = 0x80;
    uint32_t mask_key = esp_random();
    memcpy(frame + 2, &mask_key, 4);
    return write_all(tls, frame, sizeof(frame)) == static_cast<ssize_t>(sizeof(frame));
}

// Read a complete WS frame. Returns payload length, or -1 on error, 0 on
// connection close. `buf` must be large enough for the payload. Handles
// ping/pong frames transparently. Does NOT handle fragmentation (the
// ingress-rev-proxy controls framing and won't fragment).
// Does NOT handle text frames — all Noise traffic is binary.
static ssize_t ws_recv_frame(esp_tls_t *tls, uint8_t *buf, size_t buf_cap) {
  int64_t deadline = esp_timer_get_time() + WS_FRAME_TIMEOUT_US;
  for (;;) {
    if (!s_running || esp_timer_get_time() >= deadline) return -1;
    // Read 2-byte header
    uint8_t hdr[2];
    size_t have = 0;
    while (have < 2) {
        if (!s_running || esp_timer_get_time() >= deadline) return -1;
        ssize_t n = esp_tls_conn_read(tls, hdr + have, 2 - have);
        if (n > 0) {
            have += static_cast<size_t>(n);
        } else if (n == 0) {
            return 0; // connection closed
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return -1;
        }
    }

    bool fin = (hdr[0] & 0x80) != 0;
    uint8_t opcode = hdr[0] & 0x0f;
    bool masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7f;

    if (payload_len == 126) {
        uint8_t ext[2];
        have = 0;
        while (have < 2) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t n = esp_tls_conn_read(tls, ext + have, 2 - have);
            if (n > 0) have += static_cast<size_t>(n);
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        have = 0;
        while (have < 8) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t n = esp_tls_conn_read(tls, ext + have, 8 - have);
            if (n > 0) have += static_cast<size_t>(n);
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    // Server frames should not be masked, but handle it if so.
    uint8_t mask_key[4] = {0};
    if (masked) {
        have = 0;
        while (have < 4) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t n = esp_tls_conn_read(tls, mask_key + have, 4 - have);
            if (n > 0) have += static_cast<size_t>(n);
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
    }

    if (payload_len > buf_cap) {
        ESP_LOGE(TAG, "WS frame too large: %llu > %u",
                 (unsigned long long)payload_len, (unsigned)buf_cap);
        return -1;
    }
    // RFC 6455 §5.5: control frames carry at most 125 bytes. A longer ping
    // can't be echoed in a pong's 7-bit length field, so fail the connection
    // instead of sending a malformed pong.
    if (opcode >= 0x8 && payload_len > 125) {
        ESP_LOGE(TAG, "WS control frame too large: %llu", (unsigned long long)payload_len);
        return -1;
    }

    // Read payload
    size_t plen = static_cast<size_t>(payload_len);
    have = 0;
    while (have < plen) {
        if (!s_running || esp_timer_get_time() >= deadline) return -1;
        ssize_t n = esp_tls_conn_read(tls, buf + have, plen - have);
        if (n > 0) {
            have += static_cast<size_t>(n);
        } else if (n == 0) {
            return 0;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            return -1;
        }
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            buf[i] ^= mask_key[i & 3];
        }
    }

    // Handle close frame
    if (opcode == 0x8) {
        ESP_LOGW(TAG, "WS close frame received");
        return -1;
    }

    // Handle ping → send pong echoing the payload (RFC 6455 §5.5.3)
    if (opcode == 0x9) {
        uint8_t pong_hdr[6];
        pong_hdr[0] = 0x8A; // FIN | pong
        pong_hdr[1] = 0x80 | static_cast<uint8_t>(plen); // masked, len (pings are ≤125)
        uint32_t mask_key = 0;
        memcpy(pong_hdr + 2, &mask_key, 4);
        write_all(tls, pong_hdr, 6);
        if (plen > 0) write_all(tls, buf, plen); // echo payload (unmasked, mask=0)
        continue; // loop to get next real frame
    }

    // Ignore pong
    if (opcode == 0xA) {
        continue;
    }

    (void)fin;
    return static_cast<ssize_t>(plen);
  } // for (;;)
}

// Non-blocking variant: returns -2 when no data is available.
static ssize_t ws_recv_frame_nonblock(esp_tls_t *tls, uint8_t *buf, size_t cap) {
    int64_t deadline = esp_timer_get_time() + WS_FRAME_TIMEOUT_US;
    if (!s_running) return -1;
    // Read the first header byte (opcode/FIN) non-blocking to check if data
    // is available. This consumes the byte — it's not a true peek.
    uint8_t peek;
    ssize_t n = esp_tls_conn_read(tls, &peek, 1);
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return -2; // no data available
    }
    if (n <= 0) {
        return n == 0 ? 0 : -1;
    }

    // We consumed byte 0 of the WS header (opcode/FIN); now read byte 1
    // (mask/length) to complete the 2-byte header.
    uint8_t hdr1;
    size_t have = 0;
    while (have < 1) {
        if (!s_running || esp_timer_get_time() >= deadline) return -1;
        ssize_t r = esp_tls_conn_read(tls, &hdr1, 1);
        if (r > 0) { have = 1; break; }
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) { vTaskDelay(1); continue; }
        return -1;
    }

    bool fin = (peek & 0x80) != 0;
    uint8_t opcode = peek & 0x0f;
    bool masked = (hdr1 & 0x80) != 0;
    uint64_t payload_len = hdr1 & 0x7f;

    if (payload_len == 126) {
        uint8_t ext[2]; have = 0;
        while (have < 2) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t r = esp_tls_conn_read(tls, ext + have, 2 - have);
            if (r > 0) have += static_cast<size_t>(r);
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8]; have = 0;
        while (have < 8) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t r = esp_tls_conn_read(tls, ext + have, 8 - have);
            if (r > 0) have += static_cast<size_t>(r);
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        have = 0;
        while (have < 4) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t r = esp_tls_conn_read(tls, mask_key + have, 4 - have);
            if (r > 0) have += static_cast<size_t>(r);
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
            else return -1;
        }
    }

    if (payload_len > cap) {
        ESP_LOGE(TAG, "WS frame too large: %llu", (unsigned long long)payload_len);
        s_req_too_large = true;
        return -1;
    }
    if (opcode >= 0x8 && payload_len > 125) { // RFC 6455 §5.5, as above
        ESP_LOGE(TAG, "WS control frame too large: %llu", (unsigned long long)payload_len);
        return -1;
    }

    size_t plen = static_cast<size_t>(payload_len);
    have = 0;
    while (have < plen) {
        if (!s_running || esp_timer_get_time() >= deadline) return -1;
        ssize_t r = esp_tls_conn_read(tls, buf + have, plen - have);
        if (r > 0) have += static_cast<size_t>(r);
        else if (r == 0) return 0;
        else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(1);
        else return -1;
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++) buf[i] ^= mask_key[i & 3];
    }

    if (opcode == 0x8) return -1;
    if (opcode == 0x9) {
        // Echo ping payload in pong (RFC 6455 §5.5.3)
        uint8_t pong_hdr[6];
        pong_hdr[0] = 0x8A;
        pong_hdr[1] = 0x80 | static_cast<uint8_t>(plen);
        memset(pong_hdr + 2, 0, 4); // mask key = 0
        write_all(tls, pong_hdr, 6);
        if (plen > 0) write_all(tls, buf, plen);
        return -3; // control frame: no app data, but the peer is alive
    }
    if (opcode == 0xA) return -3; // pong: reply to our keepalive ping

    (void)fin;
    return static_cast<ssize_t>(plen);
}

// ---- WebSocket upgrade ------------------------------------------------------

// Enough bytes for a status line, and it looks like one. Shares the parser
// with the classification below so the log and the reconnect decision can
// never disagree about whether a status was readable.
static bool upgrade_status_readable(const char *hdr, size_t have) {
    return noise_upgrade_status_code(hdr, have) >= 0;
}

// The two upgrade failure paths. Neither puts response bytes in the log beyond
// the 3-digit status: logs are captured into bug reports now, and the headers
// can carry tokens or edge internals. The status alone still separates "the
// backend said no" from "that wasn't HTTP".
//
// Each message is spelled out in full rather than composed from a shared
// format — tests/test_link_diagnostic_log.py greps this file for the exact
// strings, and a "%s: ..." prefix would hide them from that check.
static void log_upgrade_rejected(const char *hdr, size_t have) {
    if (upgrade_status_readable(hdr, have)) {
        ESP_LOGE(TAG, "upgrade rejected: status %.3s", hdr + 9);
    } else {
        ESP_LOGE(TAG, "upgrade rejected: malformed response");
    }
}

static void log_upgrade_overflow(const char *hdr, size_t have) {
    if (upgrade_status_readable(hdr, have)) {
        ESP_LOGE(TAG, "upgrade response too large: status %.3s", hdr + 9);
    } else {
        ESP_LOGE(TAG, "upgrade response too large: malformed response");
    }
}

static noise_upgrade_result_t ws_upgrade(esp_tls_t *tls) {
    // Request construction lives in noise_upgrade.h so the host tests can run
    // it: escaping vm_id, keeping the bearer in a header and verbatim, and
    // rejecting CR/LF are all decisions worth asserting on real bytes.
    // Auth tokens can be ~1-2 KB, so the buffer is heap, not the 12 KB
    // noise_ctrl task stack.
    size_t req_cap = 512 + NOISE_UPGRADE_ESCAPED_CAP(sizeof(s_vm_id))
                     + (s_auth_token ? strlen(s_auth_token) : 0);
    char *req = static_cast<char *>(malloc(req_cap));
    if (!req) {
        ESP_LOGE(TAG, "upgrade req malloc failed");
        return NOISE_UPGRADE_FAILED;
    }
    int req_len = noise_upgrade_build_request(
        NOISE_PATH, s_vm_id, s_noise_host,
        s_auth_token ? s_auth_token : "", req, req_cap);

    if (req_len <= 0) {
        ESP_LOGE(TAG, "upgrade request rejected before send");
        free(req);
        return NOISE_UPGRADE_FAILED;
    }

    if (write_all(tls, req, req_len) != req_len) {
        ESP_LOGE(TAG, "upgrade write failed");
        free(req);
        return NOISE_UPGRADE_FAILED;
    }
    free(req);

    // The configured edge attaches an RFC 9209
    // `Proxy-Status` header to error responses — measured at ~1.4 KB on its
    // own, so a bare 401 totals ~1.5 KB. The old 1 KB buffer could not hold
    // even that, which meant every edge-level rejection surfaced as "response
    // too large" and the real status was never readable. Sized for that header
    // with headroom, and heap-allocated for the same reason the request above
    // is: 4 KB has no business on the 12 KB noise_ctrl task stack.
    constexpr size_t kHdrCap = 4096;
    char *hdr = static_cast<char *>(malloc(kHdrCap));
    if (!hdr) {
        ESP_LOGE(TAG, "upgrade response malloc failed");
        return NOISE_UPGRADE_FAILED;
    }

    size_t have = 0;
    size_t scanned = 0;  // no header terminator begins before this offset
    noise_upgrade_result_t result = NOISE_UPGRADE_FAILED;
    int64_t deadline = esp_timer_get_time() + WS_FRAME_TIMEOUT_US;
    while (s_running && esp_timer_get_time() < deadline) {
        if (have >= kHdrCap) {
            log_upgrade_overflow(hdr, have);
            result = noise_upgrade_classify(hdr, have, /*overflowed=*/true);
            break;
        }
        ssize_t n = esp_tls_conn_read(tls, hdr + have, kHdrCap - have);
        if (n > 0) {
            have += static_cast<size_t>(n);
            bool terminated = false;
            for (size_t i = scanned; i + 3 < have; i++) {
                if (hdr[i] == '\r' && hdr[i+1] == '\n'
                    && hdr[i+2] == '\r' && hdr[i+3] == '\n') {
                    result = noise_upgrade_classify(hdr, have,
                                                    /*overflowed=*/false);
                    if (result != NOISE_UPGRADE_OK) {
                        log_upgrade_rejected(hdr, have);
                    } else {
                        size_t boundary = i + 4;
                        if (have > boundary) {
                            ESP_LOGW(TAG, "%u leftover bytes after upgrade — discarding",
                                     (unsigned)(have - boundary));
                        }
                    }
                    terminated = true;
                    break;
                }
            }
            if (terminated) break;
            // Re-scan only the tail: a terminator can straddle two reads, so
            // step back 3 bytes rather than rescanning from the start.
            scanned = (have >= 3) ? have - 3 : 0;
        } else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(1);
        } else {
            ESP_LOGE(TAG, "upgrade read err %d", (int)n);
            break;
        }
    }

    free(hdr);
    return result;
}

// ---- Noise handshake --------------------------------------------------------

static bool noise_handshake(esp_tls_t *tls, ClientSession &session,
                            uint8_t *scratch, size_t scratch_len) {
    // msg1: initiator → responder
    ByteSpan out_span(scratch, scratch_len);
    auto msg1 = session.WriteHandshakeMessage1(out_span);
    if (!msg1.ok()) {
        ESP_LOGE(TAG, "WriteHandshakeMessage1 failed: %s", msg1.status().str());
        return false;
    }
    if (!ws_send_binary(tls, scratch, msg1.size())) {
        ESP_LOGE(TAG, "failed to send handshake msg1");
        return false;
    }
    ESP_LOGI(TAG, "handshake msg1 sent (%u bytes)", (unsigned)msg1.size());

    // msg2: responder → initiator
    ssize_t msg2_len = ws_recv_frame(tls, scratch, scratch_len);
    if (msg2_len <= 0) {
        ESP_LOGE(TAG, "failed to receive handshake msg2: %d", (int)msg2_len);
        return false;
    }
    ESP_LOGI(TAG, "handshake msg2 received (%d bytes)", (int)msg2_len);

    uint8_t peer_extra[256];
    size_t peer_extra_written = 0;
    auto status2 = session.ReadHandshakeMessage2(
        ConstByteSpan(scratch, static_cast<size_t>(msg2_len)),
        ByteSpan(peer_extra, sizeof(peer_extra)),
        peer_extra_written);
    if (!status2.ok()) {
        ESP_LOGE(TAG, "ReadHandshakeMessage2 failed: %s", status2.str());
        return false;
    }

    // msg3: initiator → responder.
    //
    // The payload is the server's protobuf Noise handshake message 3.
    // The rev-proxy runs it through `classify_message3_payload`, which rejects
    // anything that is not valid protobuf encoding. Sending the bare token
    // string made it drop the session immediately AFTER the crypto handshake
    // completed, so the device saw "session established", sent link.register,
    // and got a close ~60 ms later with nothing logged daemon-side.
    //
    // No field of that message carries a bearer token: `notary_token` is a
    // separate credential the proxy validates against the notary, `kr` is the
    // RV key exchange, and `shared_agent_ticket` is rejected outright on the
    // owner handshake path. Our `vm_auth_token` authenticates through the
    // Authorization bearer header on the /v1/noise upgrade above, which is
    // what the route's WsBearer mechanism reads. The correct owner payload is
    // therefore an EMPTY NoiseHandshakeMessage3 — which encodes to zero bytes,
    // so the device needs no protobuf writer.
    ConstByteSpan auth_payload;
    auto msg3 = session.WriteHandshakeMessage3(auth_payload, out_span);
    if (!msg3.ok()) {
        ESP_LOGE(TAG, "WriteHandshakeMessage3 failed: %s", msg3.status().str());
        return false;
    }
    if (!ws_send_binary(tls, scratch, msg3.size())) {
        ESP_LOGE(TAG, "failed to send handshake msg3");
        return false;
    }
    ESP_LOGI(TAG, "handshake msg3 sent (%u bytes), session established",
             (unsigned)msg3.size());
    return session.isEstablished();
}

// ---- Session: send Noise-encrypted WS frames --------------------------------

static bool flush_outbound(esp_tls_t *tls, ClientSession &session,
                           uint8_t *ws_buf) {
    while (session.HasOutboundWebSocketPayload()) {
        ByteSpan ws_span(ws_buf, WS_BUF_SIZE);
        auto sws = session.WriteNextOutboundWebSocketPayload(ws_span);
        if (!sws.ok()) {
            ESP_LOGE(TAG, "WriteNextOutboundWebSocketPayload failed: %s",
                     sws.status().str());
            return false;
        }
        if (!ws_send_binary(tls, ws_buf, sws.size())) {
            ESP_LOGE(TAG, "ws_send_binary failed");
            return false;
        }
    }
    return true;
}

// Owned by the session task. Registration, results and heartbeats share one
// slot, so another u32-LE message never interleaves with a paused message.
struct pending_json_body {
    char *json = nullptr;
    size_t json_len = 0;
    size_t json_offset = 0;  // JSON bytes sent, excluding the length prefix
    bool prefix_sent = false;
    noise_ctrl_session_generation_t session_generation = 0;
    bool is_registration = false;
    bool is_heartbeat = false;
};

static void clear_pending_json_body(pending_json_body &body) {
    free(body.json);
    body = {};
}

enum class json_send_status { Pending, Complete, Failed };

// Send at most one complete BodyChunk per loop turn, yielding to RX, tunnel
// traffic and WS keepalives between chunks even when DMA headroom stays high.
// Pending preserves ownership and framing state; only an attempted send failure
// is fatal. Never retry that failure: sealing can irrecoverably poison Noise.
static json_send_status send_json_body_chunk(esp_tls_t *tls, ClientSession &session,
                                 pending_json_body &body,
                                 uint8_t *svc_scratch, uint8_t *env_scratch,
                                 uint8_t *ws_buf, bool end_body = false) {
    if (!body.json || body.json_len > UINT32_MAX
        || body.json_offset > body.json_len) return json_send_status::Failed;
    if (body.prefix_sent && body.json_offset == body.json_len) {
        return json_send_status::Complete;
    }
    // Recheck both budgets before EVERY chunk, including retained registration:
    // the queue-boundary sample cannot cover later turns of a paused message.
    if (!noise_tx_has_dma_headroom(nullptr)
        || !noise_tx_has_contiguous_dma_headroom()) {
        return json_send_status::Pending;
    }

    size_t prefix_len = body.prefix_sent ? 0 : 4;
    size_t n = body.json_len - body.json_offset;
    if (n > CTRL_BODY_CHUNK_MAX - prefix_len) {
        n = CTRL_BODY_CHUNK_MAX - prefix_len;
    }
    const uint8_t *data = reinterpret_cast<const uint8_t *>(body.json)
        + body.json_offset;
    if (prefix_len) {
        // Reuse the WS output scratch for just the first chunk. StartOutbound
        // copies the view into svc/env scratch before flush_outbound overwrites
        // ws_buf. Later chunks point directly into the retained JSON buffer.
        static_assert(WS_BUF_SIZE >= CTRL_BODY_CHUNK_MAX,
                      "first chunk must fit WS scratch");
        uint32_t le_len = static_cast<uint32_t>(body.json_len);
        ws_buf[0] = static_cast<uint8_t>(le_len & 0xff);
        ws_buf[1] = static_cast<uint8_t>((le_len >> 8) & 0xff);
        ws_buf[2] = static_cast<uint8_t>((le_len >> 16) & 0xff);
        ws_buf[3] = static_cast<uint8_t>((le_len >> 24) & 0xff);
        memcpy(ws_buf + prefix_len, data, n);
        data = ws_buf;
    }

    bool last = body.json_offset + n == body.json_len;
    BodyChunkView chunk;
    chunk.data = ConstByteSpan(data, prefix_len + n);
    chunk.end_body = last && end_body;
    auto sws = session.StartOutboundBodyChunk(
        ServiceType::Daemon, CTRL_STREAM_ID, chunk,
        ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGE(TAG, "StartOutboundBodyChunk failed at JSON byte %u/%u: %s",
                 (unsigned)body.json_offset, (unsigned)body.json_len,
                 sws.status().str());
        return json_send_status::Failed;
    }
    if (!flush_outbound(tls, session, ws_buf)) return json_send_status::Failed;

    body.json_offset += n;
    body.prefix_sent = true;
    return last ? json_send_status::Complete : json_send_status::Pending;
}

// ---- Tunnel stream multiplexing --------------------------------------------

// Bundles the session-task-owned state the tunnel emit callbacks need. Lives
// on the run_session() stack; valid only while the session is up.
struct tunnel_emit_ctx {
    esp_tls_t *tls;
    ClientSession *session;
    uint8_t *svc_scratch;
    uint8_t *env_scratch;
    uint8_t *ws_buf;
};

// Open the streaming POST /link-tunnel on TUNNEL_STREAM_ID.
static bool tunnel_open_stream(void *vctx) {
    auto *c = static_cast<tunnel_emit_ctx *>(vctx);
    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView("/link-tunnel");
    req.end_body = false;
    auto sws = c->session->StartOutboundApplicationRequest(
        ServiceType::Daemon, TUNNEL_STREAM_ID, req,
        ByteSpan(c->svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(c->env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGE(TAG, "tunnel StartOutboundApplicationRequest failed: %s",
                 sws.status().str());
        return false;
    }
    return flush_outbound(c->tls, *c->session, c->ws_buf);
}

// Send an IP-packet batch as a BodyChunk on the tunnel stream. The batch is
// tunnel_netif's [u16-LE len][pkt]... framing; Noise message boundaries
// delimit batches, so no outer u32 envelope is added.
static bool tunnel_send_body(void *vctx, const uint8_t *data, size_t len) {
    auto *c = static_cast<tunnel_emit_ctx *>(vctx);
    BodyChunkView chunk;
    chunk.data = ConstByteSpan(data, len);
    chunk.end_body = false;
    auto sws = c->session->StartOutboundBodyChunk(
        ServiceType::Daemon, TUNNEL_STREAM_ID, chunk,
        ByteSpan(c->svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(c->env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGE(TAG, "tunnel StartOutboundBodyChunk failed: %s",
                 sws.status().str());
        return false;
    }
    return flush_outbound(c->tls, *c->session, c->ws_buf);
}

// ---- Extra daemon requests (noise_ctrl_req_*) --------------------------------

// Only Muse opens these; Link-only gateways get the empty stubs below.
#if CONFIG_MUSE_ENABLED

// Stream ids from here up belong to these requests; lower ids are Link's own.
#define REQ_STREAM_BASE 16
#define REQ_MAX_STREAMS 3
#define REQ_QUEUE_LEN   6
#define REQ_MAX_HEADERS 6
// Body bytes queued at once. Without PSRAM a larger backlog leaves no block big
// enough for mbedTLS's per-write record buffer, and the session drops.
#define REQ_QUEUE_BYTES (SMALL_CONTROL_SESSION ? 4096 : 16384)
// Largest free block a request send needs: the TLS record plus overhead.
#define REQ_TLS_BLOCK   (CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN + 512)

enum class req_op_kind : uint8_t { Open, Body, Cancel };

// Queued by any task for the session task. Open packs
// "verb\0path\0name\0value\0...\0" into data; Body carries the bytes.
struct req_op {
    req_op_kind kind;
    bool end_body;
    int64_t id;
    uint8_t *data;
    size_t len;
    noise_ctrl_req_cb cb;
    void *ctx;
};

struct req_stream {
    int64_t id;  // 0: free
    noise_ctrl_req_cb cb;
    void *ctx;
};

static QueueHandle_t s_req_q = nullptr;
static std::atomic<uint32_t> s_req_next_id{REQ_STREAM_BASE};
// Payload bytes waiting in s_req_q. Sending frees them, so the DMA gate counts
// them as free: otherwise a burst of queued chunks can hold free RAM under the
// reserve and stall the very sends that would release it.
static std::atomic<int32_t> s_req_queued_bytes{0};

static bool req_take(req_op *op) {
    if (xQueueReceive(s_req_q, op, 0) != pdTRUE) return false;
    s_req_queued_bytes -= (int32_t)op->len;
    return true;
}

// Queues op, counting its payload first so a quick dequeue can't go negative.
static bool req_put(const req_op &op, TickType_t wait) {
    s_req_queued_bytes += (int32_t)op.len;
    if (xQueueSend(s_req_q, &op, wait) == pdTRUE) return true;
    s_req_queued_bytes -= (int32_t)op.len;
    return false;
}
// Session-task-owned.
static req_stream s_req_streams[REQ_MAX_STREAMS];

static req_stream *req_find(int64_t id) {
    for (auto &s : s_req_streams) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

static void req_drop(req_op &op) {
    if (op.kind == req_op_kind::Open) op.cb(op.ctx, -1, nullptr, 0, true);
    free(op.data);
}

static void req_fail(req_stream *s, int status = -1) {
    req_stream gone = *s;
    *s = {};
    gone.cb(gone.ctx, status, nullptr, 0, true);
}

// The session is gone, and every request with it: NOISE_CTRL_REQ_TOO_LARGE
// when a frame too big for this board's buffers ended it.
static void req_end_all(void) {
    req_op op;
    while (req_take(&op)) req_drop(op);
    int status = s_req_too_large ? NOISE_CTRL_REQ_TOO_LARGE : -1;
    s_req_too_large = false;
    for (auto &s : s_req_streams) {
        if (s.id) req_fail(&s, status);
    }
}

static bool req_open(esp_tls_t *tls, ClientSession &session, req_op &op,
                     uint8_t *svc_scratch, uint8_t *env_scratch, uint8_t *ws_buf) {
    req_stream *slot = req_find(0);
    if (!slot) {
        ESP_LOGW(TAG, "request %lld: no free stream", (long long)op.id);
        req_drop(op);
        return true;
    }
    const char *verb = reinterpret_cast<const char *>(op.data);
    const char *path = verb + strlen(verb) + 1;
    HeaderView hdrs[REQ_MAX_HEADERS];
    size_t nh = 0;
    for (const char *p = path + strlen(path) + 1; *p && nh < REQ_MAX_HEADERS;) {
        const char *v = p + strlen(p) + 1;
        hdrs[nh++] = {StringView(p), StringView(v)};
        p = v + strlen(v) + 1;
    }
    ApplicationRequestView req;
    req.verb = StringView(verb);
    req.path = StringView(path);
    req.headers = Span<const HeaderView>(hdrs, nh);
    req.end_body = op.end_body;
    auto sws = session.StartOutboundApplicationRequest(
        ServiceType::Daemon, op.id, req,
        ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGW(TAG, "request %lld: %s %s failed: %s", (long long)op.id, verb, path,
                 sws.status().str());
        req_drop(op);
        return true;
    }
    ESP_LOGD(TAG, "request %lld: %s %s", (long long)op.id, verb, path);
    *slot = {op.id, op.cb, op.ctx};
    free(op.data);
    return flush_outbound(tls, session, ws_buf);
}

// Sends at most one queued request operation per loop turn. False only if the
// transport failed.
static bool req_pump_tx(esp_tls_t *tls, ClientSession &session,
                        uint8_t *svc_scratch, uint8_t *env_scratch,
                        uint8_t *ws_buf, bool *did) {
    // The loop turns every tick; the heap checks below walk the heap, so only
    // when there is something to send.
    if (!uxQueueMessagesWaiting(s_req_q)) return true;
    int32_t queued = s_req_queued_bytes;
    if (!noise_tx_has_dma_headroom_reclaiming(queued > 0 ? (size_t)queued : 0)
        || !noise_tx_has_contiguous_dma_headroom()) return true;
    if (SMALL_CONTROL_SESSION
        && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < REQ_TLS_BLOCK) return true;
    req_op op;
    if (!req_take(&op)) return true;
    *did = true;
    if (op.kind == req_op_kind::Open) {
        return req_open(tls, session, op, svc_scratch, env_scratch, ws_buf);
    }
    req_stream *s = req_find(op.id);
    bool ok = true;
    if (s && op.kind == req_op_kind::Body) {
        BodyChunkView chunk;
        chunk.data = ConstByteSpan(op.data, op.len);
        chunk.end_body = op.end_body;
        auto sws = session.StartOutboundBodyChunk(
            ServiceType::Daemon, op.id, chunk,
            ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
            ByteSpan(env_scratch, OUT_ENV_SCRATCH));
        if (sws.ok()) {
            ok = flush_outbound(tls, session, ws_buf);
        } else {
            ESP_LOGW(TAG, "request %lld: body chunk failed: %s", (long long)op.id,
                     sws.status().str());
            req_fail(s);
        }
    } else if (s) {
        *s = {};
        ResetView rv{ResetCode::Cancelled, StringView("cancelled")};
        auto sws = session.StartOutboundReset(
            ServiceType::Daemon, op.id, rv,
            ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
            ByteSpan(env_scratch, OUT_ENV_SCRATCH));
        if (sws.ok()) ok = flush_outbound(tls, session, ws_buf);
    }
    free(op.data);
    return ok;
}

// Request stream frames never affect the control session. Frames for a
// cancelled request are dropped. False if the frame isn't on a request stream.
static bool req_on_frame(const DecodedServiceFrame &frame) {
    if (frame.stream_id < REQ_STREAM_BASE) return false;
    req_stream *s = req_find(frame.stream_id);
    if (!s) return true;
    req_stream cur = *s;
    if (frame.kind == ServiceFrameKind::Response) {
        if (frame.response.end_body) *s = {};
        cur.cb(cur.ctx, frame.response.status, frame.response.body.data(),
               frame.response.body.size(), frame.response.end_body);
    } else if (frame.kind == ServiceFrameKind::BodyChunk) {
        if (frame.body_chunk.end_body) *s = {};
        cur.cb(cur.ctx, 0, frame.body_chunk.data.data(),
               frame.body_chunk.data.size(), frame.body_chunk.end_body);
    } else if (frame.kind == ServiceFrameKind::Reset) {
        ESP_LOGW(TAG, "server reset request %lld", (long long)frame.stream_id);
        req_fail(s);
    }
    return true;
}

static void req_init(void) {
    s_req_q = xQueueCreate(REQ_QUEUE_LEN, sizeof(req_op));
}
#else
static void req_init(void) {}
static void req_end_all(void) {}
static bool req_on_frame(const DecodedServiceFrame &) { return false; }
#endif  // CONFIG_MUSE_ENABLED

// ---- Agent identity ----------------------------------------------------------

// Reply to GET /identity, collected across the Response and BodyChunk frames.
struct identity_fetch {
    bool requested;
    bool done;
    char *body;
    size_t len;
};

// Ask the daemon for the agent identity. Returns false only if the transport
// failed; a refused request just leaves the name unknown.
static bool identity_request(esp_tls_t *tls, ClientSession &session,
                             uint8_t *svc_scratch, uint8_t *env_scratch,
                             uint8_t *ws_buf) {
    ApplicationRequestView req;
    req.verb = StringView("GET");
    req.path = StringView("/identity");
    req.end_body = true;
    auto sws = session.StartOutboundApplicationRequest(
        ServiceType::Daemon, IDENTITY_STREAM_ID, req,
        ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGW(TAG, "identity StartOutboundApplicationRequest failed: %s",
                 sws.status().str());
        return true;
    }
    return flush_outbound(tls, session, ws_buf);
}

static void identity_finish(identity_fetch &f) {
    f.done = true;
    // The daemon wraps the identity: {"ok":true,"result":{"name":...}}.
    cJSON *root = f.body ? cJSON_ParseWithLength(f.body, f.len) : nullptr;
    cJSON *result = root ? cJSON_GetObjectItem(root, "result") : nullptr;
    cJSON *name = cJSON_GetObjectItem(result, "name");
    if (cJSON_IsString(name) && name->valuestring && *name->valuestring) {
        ESP_LOGI(TAG, "agent name: %s", name->valuestring);
        if (s_agent_name_cb) s_agent_name_cb(name->valuestring);
    } else {
        ESP_LOGW(TAG, "identity reply has no name");
    }
    cJSON_Delete(root);
    free(f.body);
    f.body = nullptr;
}

static void identity_append(identity_fetch &f, ConstByteSpan data, bool end_body) {
    if (f.done) return;
    if (data.size() > 0) {
        if (!f.body) f.body = static_cast<char *>(malloc(IDENTITY_BODY_MAX));
        if (!f.body || f.len + data.size() > IDENTITY_BODY_MAX) {
            ESP_LOGW(TAG, "identity reply too large or out of memory");
            free(f.body);
            f.body = nullptr;
            f.done = true;
            return;
        }
        memcpy(f.body + f.len, data.data(), data.size());
        f.len += data.size();
    }
    if (end_body) identity_finish(f);
}

// Identity stream frames never affect the control session.
static void identity_on_frame(identity_fetch &f, const DecodedServiceFrame &frame) {
    if (f.done) return;
    if (frame.kind == ServiceFrameKind::Response) {
        if (frame.response.status != 200) {
            ESP_LOGW(TAG, "server rejected /identity: %d", frame.response.status);
            f.done = true;
            return;
        }
        identity_append(f, frame.response.body, frame.response.end_body);
    } else if (frame.kind == ServiceFrameKind::BodyChunk) {
        identity_append(f, frame.body_chunk.data, frame.body_chunk.end_body);
    } else if (frame.kind == ServiceFrameKind::Reset) {
        ESP_LOGW(TAG, "server reset identity stream");
        free(f.body);
        f.body = nullptr;
        f.done = true;
    }
}

// ---- JSON message builders -------------------------------------------------

static cJSON *string_param(const char *description) {
    cJSON *param = cJSON_CreateObject();
    cJSON_AddStringToObject(param, "type", "string");
    if (description) {
        cJSON_AddStringToObject(param, "description", description);
    }
    return param;
}

static void add_command(cJSON *commands, const char *name,
                        const char *description,
                        cJSON *required, cJSON *optional) {
    cJSON *command = cJSON_CreateObject();
    cJSON_AddStringToObject(command, "description", description);
    cJSON_AddItemToObject(command, "required",
                          required ? required : cJSON_CreateObject());
    cJSON_AddItemToObject(command, "optional",
                          optional ? optional : cJSON_CreateObject());
    cJSON_AddItemToObject(commands, name, command);
}

static void add_register_metadata_string(cJSON **metadata, cJSON *params,
                                         const char *key,
                                         const char *value) {
    if (!value || !value[0]) return;
    if (!*metadata) {
        *metadata = cJSON_CreateObject();
        if (!*metadata) return;
        cJSON_AddItemToObject(params, "metadata", *metadata);
    }
    cJSON_AddStringToObject(*metadata, key, value);
}

static char *build_register_json(void) {
    char wifi_ssid[sizeof(s_wifi_ssid)];
    copy_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "req");
    char uuid[40];
    make_uuid(uuid, sizeof(uuid));
    cJSON_AddStringToObject(root, "id", uuid);
    // Remember this id so the session loop can detect the register reply and
    // only then open the tunnel stream (see s_register_req_id).
    strncpy(s_register_req_id, uuid, sizeof(s_register_req_id) - 1);
    s_register_req_id[sizeof(s_register_req_id) - 1] = '\0';
    cJSON_AddStringToObject(root, "method", "link.register");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "node_id", s_node_id);
    cJSON_AddStringToObject(params, "display_name", s_display_name);
    cJSON_AddStringToObject(params, "platform", "esp32");
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(params, "version", desc ? desc->version : "unknown");
    cJSON_AddStringToObject(params, "device_family", "link");
    cJSON_AddStringToObject(params, "model_id", "esp-link");
    cJSON_AddBoolToObject(params, "is_wakeup_supported", false);
    cJSON *metadata = nullptr;
    add_register_metadata_string(&metadata, params, "network_ssid", wifi_ssid);

    cJSON *commands = cJSON_CreateObject();
    add_command(commands, "device.health",
                "Report basic Link health, including battery level (percent), "
                "voltage, and whether it is charging or on USB power; each is "
                "null when the device has no battery or can't measure it. "
                "Boards with an SHT40 sensor also report temp_c (Celsius) and "
                "humidity_pct (null when unavailable)",
                nullptr, nullptr);
#if CONFIG_HOMEHUB_TUNNEL
    add_command(commands, "device.discover",
                "Scan local network for devices (ARP, mDNS, SSDP, SNMP, NetBIOS)",
                nullptr, nullptr);
    cJSON_AddNumberToObject(
        cJSON_GetObjectItem(commands, "device.discover"), "timeout_ms", 90000);
#endif

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    int disp_w, disp_h;
    if (led_status_display_info(&disp_w, &disp_h)) {
        // The agent prepares images from this, so it states the bit depth
        // and, for e-paper, the exact inks.
        int bits = led_status_display_bits();
        bool mono = bits == 1;
        bool epaper = bits == 1 || bits == 4;
        char desc[1536];
        snprintf(desc, sizeof(desc),
                 "Download an image and draw it on the %dx%d %s. Takes a "
                 "baseline "
                 "(not progressive) JPEG, shrunk 1/2, 1/4 or 1/8 on the "
                 "device if needed and centred across, or raw RGB565 (high "
                 "byte first, %d bytes per row).%s Plain http:// uses the least "
                 "device memory. Replies when the image is drawn. Hides the "
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
                 "avatar until display.show_animation, a tap or the talk button. "
                 "Some Muse screens are round and cut off the corners, so keep "
                 "the subject in the middle. A JPEG as large as the screen "
                 "looks best.",
#else
                 "%s until display.show_animation.",
#endif
                 disp_w, disp_h,
                 mono ? "black and white e-paper screen, 1 bit per pixel"
                 : bits == 4 ? "six-colour e-paper screen (E Ink Spectra 6), "
                               "4 bits per pixel"
                             : "colour screen, 16 bits per pixel (RGB565)",
                 disp_w * 2,
                 mono ? " The screen has no gray or colour: the device "
                        "dithers them to black and white dots, so photos "
                        "look grainy, and text and line art are sharpest "
                        "in pure black and white (#000000 and #ffffff are "
                        "shown exactly). It refreshes once the whole image "
                        "is in, taking about 2 s and flashing, so send one "
                        "whole screen rather than several parts. The image "
                        "stays on screen without power."
                 : bits == 4 ? " The screen has only six inks: black #000000, "
                               "white #ffffff, yellow #ffff00, red #ff0000, "
                               "blue #0000ff and green #00ff00, shown exactly. "
                               "The device dithers every other colour and gray "
                               "to dots of these, so photos look grainy and "
                               "muted, and text, charts and flat artwork are "
                               "sharpest drawn in those six colours only. It "
                               "refreshes once the whole image is in, taking "
                               "about 30 s and flashing, so send one whole "
                               "screen rather than several parts, at most "
                               "about once a minute. The image stays on "
                               "screen without power."
                             : ""
#if !CONFIG_HOMEHUB_LED_BACKEND_MUSE
                 , epaper ? "status screen" : "animation"
#endif
                 );
        cJSON *url_required = cJSON_CreateObject();
        cJSON_AddItemToObject(url_required, "url",
                              string_param("http:// or https:// URL of the image."));
        cJSON *url_optional = cJSON_CreateObject();
        cJSON *top_param = cJSON_CreateObject();
        cJSON_AddStringToObject(top_param, "type", "integer");
        cJSON_AddStringToObject(top_param, "description",
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
                                "Row to draw the top of the image at; by default a JPEG "
                                "is centred down the screen and raw data starts at 0.");
#else
                                "Row to draw the top of the image at; default 0.");
#endif
        cJSON_AddItemToObject(url_optional, "row", top_param);
        add_command(commands, "display.draw_url", desc, url_required, url_optional);
        // The six-colour e-paper may first finish a status screen refresh,
        // then takes as long again for the image.
        cJSON_AddNumberToObject(
            cJSON_GetObjectItem(commands, "display.draw_url"), "timeout_ms",
            bits == 4 ? 120000 : 60000);
        add_command(commands, "display.show_animation",
                    epaper ? "Clear the image and bring back the status screen "
                           "and the agent's name."
                         : "Clear the image and bring back the animation and "
                           "the agent's name.",
                    nullptr, nullptr);
    }
#endif

#if CONFIG_HOMEHUB_VOICE
    cJSON *volume_optional = cJSON_CreateObject();
    cJSON *volume_param = cJSON_CreateObject();
    cJSON_AddStringToObject(volume_param, "type", "integer");
    cJSON_AddStringToObject(volume_param, "description",
                            "Speaker volume, 0 to 100; kept across restarts.");
    cJSON_AddItemToObject(volume_optional, "volume", volume_param);
    add_command(commands, "voice.configure",
                "Set the speaker volume. Without volume, "
                "reports the current one.",
                nullptr, volume_optional);
#endif

#if CONFIG_HOMEHUB_SENSECAP_SENSORS
    add_command(commands, "sensors.read",
                "Read the air sensors: CO2 in ppm, the tVOC index (1-500, 100 "
                "is typical), temperature in degrees Celsius and relative "
                "humidity in percent. Each reading has its age in seconds; a "
                "sensor with no recent reading is null. Temperature and "
                "humidity need the Grove AHT20 plugged in.",
                nullptr, nullptr);
#endif
#if CONFIG_HOMEHUB_RETERMINAL_SHT4X
    add_command(commands, "sensors.read",
                "Read the onboard air sensor: temperature in degrees Celsius "
                "and relative humidity in percent. Each reading has its age "
                "in seconds; a sensor with no recent reading is null.",
                nullptr, nullptr);
#endif

#if CONFIG_MUSE_WATCHER_CAMERA
    add_command(commands, "camera.capture",
                "Capture one still JPEG frame from the SenseCAP Watcher camera. "
                "The frame is returned as base64 only when this command is explicitly invoked.",
                nullptr, nullptr);
    cJSON_AddNumberToObject(
        cJSON_GetObjectItem(commands, "camera.capture"), "timeout_ms", 30000);
#endif

    if (ota_is_enabled()) {
        cJSON *ota_required = cJSON_CreateObject();
        cJSON_AddItemToObject(ota_required, "url",
                              string_param("HTTPS URL of the .bin firmware image to flash."));
        cJSON *ota_optional = cJSON_CreateObject();
        cJSON *force_param = cJSON_CreateObject();
        cJSON_AddStringToObject(force_param, "type", "boolean");
        cJSON_AddStringToObject(force_param, "description",
                                "Install even if the image version is not newer.");
        cJSON_AddItemToObject(ota_optional, "force", force_param);
        add_command(commands, "device.ota",
                    "Download a firmware image from `url` and apply it "
                    "(esp_https_ota), then reboot.",
                    ota_required, ota_optional);
    }

    cJSON_AddItemToObject(params, "commands_v2", commands);
    cJSON_AddItemToObject(root, "params", params);

    // cJSON_PrintUnformatted grows its buffer by doubling, holding the old
    // one each time, so ~2 KB of JSON briefly needs ~6 KB of byte-addressable
    // heap. Right after the handshake there is often not that much, so print
    // into one buffer sized to fit instead.
    char *json = nullptr;
    for (int size = 2048; size <= 8192 && !json; size += 512) {
        json = (char *)malloc(size);
        if (!json) break;
        if (!cJSON_PrintPreallocated(root, json, size, false)) {
            free(json);
            json = nullptr;
        }
    }
    cJSON_Delete(root);
    return json;
}

// ---- OTA callback ----------------------------------------------------------

struct ota_ctx {
    noise_ctrl_session_generation_t session_generation;
    char request_id[64];
};

// Queued result from OTA or other async operations.
static void queue_result(noise_ctrl_session_generation_t session_generation,
                         const char *request_id, cJSON *result) {
    if (!session_generation || !s_result_q || !request_id) {
        cJSON_Delete(result);
        return;
    }
    pending_result pr;
    pr.session_generation = session_generation;
    pr.request_id = strdup(request_id);
    pr.result = result;
    if (pr.request_id == nullptr) {
        cJSON_Delete(result);
        return;
    }
    if (xQueueSend(s_result_q, &pr, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(pr.request_id);
        cJSON_Delete(result);
        ESP_LOGW(TAG, "result queue full, dropping result for %s", request_id);
    }
}

static void noise_ota_status(const ota_event_t *ev, void *user) {
    auto *ctx = static_cast<ota_ctx *>(user);
    cJSON *result = cJSON_CreateObject();

    switch (ev->result) {
        case OTA_RESULT_APPLIED: {
            cJSON_AddBoolToObject(result, "ok", true);
            char payload[192];
            snprintf(payload, sizeof(payload),
                     "{\"status\":\"applied\",\"version\":\"%s\",\"rebooting\":true}",
                     ev->new_version ? ev->new_version : "");
            cJSON_AddStringToObject(result, "payload_json", payload);
            break;
        }
        case OTA_RESULT_SKIPPED: {
            cJSON_AddBoolToObject(result, "ok", true);
            char payload[192];
            snprintf(payload, sizeof(payload),
                     "{\"status\":\"skipped\",\"version\":\"%s\",\"running\":\"%s\"}",
                     ev->new_version ? ev->new_version : "",
                     ev->running_version ? ev->running_version : "");
            cJSON_AddStringToObject(result, "payload_json", payload);
            break;
        }
        default: {
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "code", "ota_failed");
            cJSON_AddStringToObject(error, "message",
                                    ev->detail ? ev->detail : "ota failed");
            cJSON_AddItemToObject(result, "error", error);
            break;
        }
    }

    queue_result(ctx->session_generation, ctx->request_id, result);
    free(ctx);
}

// ---- Inbound message dispatch -----------------------------------------------

static void send_device_health(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id) {
    char wifi_ssid[sizeof(s_wifi_ssid)];
    copy_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));

    cJSON *health = cJSON_CreateObject();
    cJSON_AddStringToObject(health, "overall", "ok");
    cJSON_AddItemToObject(health, "levels", cJSON_CreateObject());
    cJSON_AddItemToObject(health, "critical", cJSON_CreateArray());
    cJSON_AddItemToObject(health, "warnings", cJSON_CreateArray());

    cJSON *metrics = cJSON_CreateObject();
    cJSON_AddNumberToObject(metrics, "uptime_seconds",
                            esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(metrics, "heap_free_internal",
                            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(metrics, "heap_free_psram",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON *battery_pct = nullptr, *battery_mv = nullptr;
    cJSON *charging = nullptr, *usb_power = nullptr;
#if CONFIG_MUSE_ENABLED
    muse_power_t power = muse_state_power();
    if (power.battery_pct >= 0) {
        battery_pct = cJSON_CreateNumber(power.battery_pct);
        if (power.battery_mv > 0) {
            battery_mv = cJSON_CreateNumber(power.battery_mv);
        }
        charging = cJSON_CreateBool(power.charging);
    }
    usb_power = cJSON_CreateBool(power.usb);
#endif
#if CONFIG_HOMEHUB_BATTERY_ADC_ENABLED
    int batt_mv = 0, batt_pct = 0;
    if (battery_adc_read(&batt_mv, &batt_pct)) {
        if (battery_pct) {
            cJSON_Delete(battery_pct);
            battery_pct = nullptr;
        }
        if (battery_mv) {
            cJSON_Delete(battery_mv);
            battery_mv = nullptr;
        }
        battery_pct = cJSON_CreateNumber(batt_pct);
        battery_mv = cJSON_CreateNumber(batt_mv);
    }
#endif
    cJSON_AddItemToObject(metrics, "battery_pct",
                          battery_pct ? battery_pct : cJSON_CreateNull());
    cJSON_AddItemToObject(metrics, "battery_mv",
                          battery_mv ? battery_mv : cJSON_CreateNull());
    cJSON_AddItemToObject(metrics, "charging",
                          charging ? charging : cJSON_CreateNull());
    cJSON_AddItemToObject(metrics, "usb_power",
                          usb_power ? usb_power : cJSON_CreateNull());
#if CONFIG_HOMEHUB_SHT40_ENABLED
    float sht_temp_c = 0, sht_humidity_pct = 0;
    if (sht40_read(&sht_temp_c, &sht_humidity_pct)) {
        cJSON_AddNumberToObject(metrics, "temp_c", sht_temp_c);
        cJSON_AddNumberToObject(metrics, "humidity_pct", sht_humidity_pct);
    } else {
        cJSON_AddNullToObject(metrics, "temp_c");
        cJSON_AddNullToObject(metrics, "humidity_pct");
    }
#endif
    cJSON_AddItemToObject(health, "metrics", metrics);
    if (wifi_ssid[0]) {
        cJSON_AddStringToObject(health, "network_ssid", wifi_ssid);
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(health, "version",
                            desc ? desc->version : "unknown");

    char *payload = cJSON_PrintUnformatted(health);
    cJSON_Delete(health);
    if (!payload) return;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "payload_json", payload);
    free(payload);
    queue_result(session_generation, request_id, result);
}

static void handle_invoke_request(
    const char *data, size_t len,
    noise_ctrl_session_generation_t session_generation) {
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        if (cJSON_IsString(event)
            && (strcmp(event->valuestring, "link.unpaired") == 0
                || strcmp(event->valuestring, "node.unpaired") == 0)) {
            ESP_LOGW(TAG, "received link.unpaired event from server");
            notify_status("ws_unpaired");
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || !method->valuestring
        || strcmp(method->valuestring, "link.invoke") != 0) {
        cJSON_Delete(root);
        return;
    }

    cJSON *id_j = cJSON_GetObjectItem(root, "id");
    cJSON *cmd_j = cJSON_GetObjectItem(root, "command");
    cJSON *cmd_params = cJSON_GetObjectItem(root, "params");
    const char *request_id = (cJSON_IsString(id_j) && id_j->valuestring)
                             ? id_j->valuestring : nullptr;
    const char *command = (cJSON_IsString(cmd_j) && cmd_j->valuestring)
                          ? cmd_j->valuestring : "";
    if (!cJSON_IsObject(cmd_params)) cmd_params = nullptr;

    if (!request_id) {
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "invoke request: command=%s", command);

    if (strcmp(command, "device.ota") == 0) {
        cJSON *url = cmd_params ? cJSON_GetObjectItem(cmd_params, "url") : nullptr;
        cJSON *force = cmd_params ? cJSON_GetObjectItem(cmd_params, "force") : nullptr;
        if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            auto *ctx = static_cast<ota_ctx *>(malloc(sizeof(ota_ctx)));
            if (ctx) {
                ctx->session_generation = session_generation;
                strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);
                ctx->request_id[sizeof(ctx->request_id) - 1] = '\0';
                ota_start(url->valuestring, cJSON_IsTrue(force),
                          noise_ota_status, ctx);
            } else {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", false);
                cJSON *error = cJSON_CreateObject();
                cJSON_AddStringToObject(error, "code", "out_of_memory");
                cJSON_AddStringToObject(error, "message",
                                        "failed to allocate OTA context");
                cJSON_AddItemToObject(result, "error", error);
                queue_result(session_generation, request_id, result);
            }
        } else {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddBoolToObject(result, "ok", false);
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "code", "invalid_params");
            cJSON_AddStringToObject(error, "message",
                                    "device.ota requires a non-empty url");
            cJSON_AddItemToObject(result, "error", error);
            queue_result(session_generation, request_id, result);
        }
    } else if (strcmp(command, "device.health") == 0) {
        send_device_health(session_generation, request_id);
    } else if (s_command_cb) {
        cJSON *result = s_command_cb(command, cmd_params, request_id,
                                     session_generation);
        if (result) {
            cJSON *async = cJSON_GetObjectItem(result, "_async");
            if (cJSON_IsTrue(async)) {
                cJSON_Delete(result);
            } else {
                queue_result(session_generation, request_id, result);
            }
        } else {
            cJSON *err_result = cJSON_CreateObject();
            cJSON_AddBoolToObject(err_result, "ok", false);
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "code", "handler_failed");
            cJSON_AddStringToObject(error, "message",
                                    "command handler did not return a result");
            cJSON_AddItemToObject(err_result, "error", error);
            queue_result(session_generation, request_id, err_result);
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "unsupported command: %s", command);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "unsupported");
        cJSON_AddStringToObject(error, "message", msg);
        cJSON_AddItemToObject(result, "error", error);
        queue_result(session_generation, request_id, result);
    }
    cJSON_Delete(root);
}

// ---- result → JSON envelope -------------------------------------------------

static char *wrap_result_json(const char *request_id, cJSON *result) {
    cJSON *root = cJSON_CreateObject();
    auto fail = [&]() -> char * {
        cJSON_Delete(root);
        cJSON_Delete(result);
        return nullptr;
    };
    if (!root || !cJSON_AddStringToObject(root, "method", "link.result")
        || !cJSON_AddStringToObject(root, "id", request_id)) return fail();

    cJSON *ok_j = cJSON_DetachItemFromObject(result, "ok");
    if (ok_j && !cJSON_AddItemToObject(root, "ok", ok_j)) {
        cJSON_Delete(ok_j);
        return fail();
    }

    cJSON *payload = cJSON_DetachItemFromObject(result, "payload");
    if (!payload) {
        cJSON *payload_json = cJSON_GetObjectItem(result, "payload_json");
        if (cJSON_IsString(payload_json) && payload_json->valuestring)
            payload = cJSON_Parse(payload_json->valuestring);
    }
    if (payload && !cJSON_AddItemToObject(root, "payload", payload)) {
        cJSON_Delete(payload);
        return fail();
    }

    cJSON *error = cJSON_DetachItemFromObject(result, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            if (!cJSON_AddStringToObject(root, "error", msg->valuestring)) {
                cJSON_Delete(error);
                return fail();
            }
        }
        cJSON_Delete(error);
    }

    cJSON_Delete(result);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// ---- Reconnect policy (mirrors raw_tunnel) ----------------------------------

enum session_result_t {
    SESSION_OK,
    SESSION_FAILED,
    SESSION_AUTH_FAILED,
};

static bool update_reconnect_policy(session_result_t result,
                                    uint64_t up_ms,
                                    uint32_t *backoff_ms,
                                    uint8_t *short_failures) {
    if (result == SESSION_AUTH_FAILED) {
        *short_failures = 0;
        *backoff_ms = RECONNECT_AUTH_FAILED_MS;
        return false;
    }
    if (up_ms > STABLE_SESSION_MS) {
        *short_failures = 0;
        *backoff_ms = RECONNECT_MIN_MS;
        return false;
    }
    if (*short_failures < UINT8_MAX) (*short_failures)++;
    *backoff_ms *= 2;
    if (*backoff_ms > RECONNECT_MAX_MS) *backoff_ms = RECONNECT_MAX_MS;
    if (*short_failures >= VM_REFRESH_FAILURES) {
        *short_failures = 0;
        return true;
    }
    return false;
}

static void reconnect_delay(uint32_t delay_ms) {
    while (s_running && delay_ms > 0) {
        uint32_t step = delay_ms < RECONNECT_DELAY_STEP_MS
                        ? delay_ms : RECONNECT_DELAY_STEP_MS;
        vTaskDelay(pdMS_TO_TICKS(step));
        delay_ms -= step;
    }
}

// ---- Session loop -----------------------------------------------------------

// Process one inbound RX body-chunk message from the server. The Noise layer
// gives us reassembled service frames; inside is a stream-1 BodyChunk whose
// data is one or more u32-LE prefixed JSON messages.
static void process_inbound_body_chunk(
    ConstByteSpan data,
    noise_ctrl_session_generation_t session_generation) {
    const uint8_t *p = data.data();
    size_t remaining = data.size();

    while (remaining >= 4) {
        uint32_t msg_len = static_cast<uint32_t>(p[0])
                         | (static_cast<uint32_t>(p[1]) << 8)
                         | (static_cast<uint32_t>(p[2]) << 16)
                         | (static_cast<uint32_t>(p[3]) << 24);
        p += 4;
        remaining -= 4;
        if (msg_len > remaining) {
            ESP_LOGW(TAG, "truncated inbound message: want %u, have %u",
                     (unsigned)msg_len, (unsigned)remaining);
            break;
        }
        if (msg_len > 0) {
            // Detect the link.register reply (id matches our request) so the
            // caller can open the tunnel stream only once the server has
            // recorded this connection's device registration.
            if (!s_register_acked && s_register_req_id[0]) {
                cJSON *msg = cJSON_ParseWithLength(
                    reinterpret_cast<const char *>(p), msg_len);
                if (msg) {
                    cJSON *id = cJSON_GetObjectItem(msg, "id");
                    if (cJSON_IsString(id) && id->valuestring
                        && strcmp(id->valuestring, s_register_req_id) == 0) {
                        cJSON *failure = cJSON_GetObjectItem(msg, "error");
                        if (failure && !cJSON_IsNull(failure) && !cJSON_IsFalse(failure)) {
                            // Not registered, so the server would refuse the
                            // tunnel and the Muse can't invoke this device.
                            cJSON *reason = cJSON_IsObject(failure)
                                ? cJSON_GetObjectItem(failure, "message") : failure;
                            ESP_LOGE(TAG, "link.register rejected: %s",
                                     cJSON_IsString(reason) && reason->valuestring
                                         ? reason->valuestring : "no reason given");
                        } else {
                            s_register_acked = true;
                            ESP_LOGI(TAG, "link.register acked; tunnel may open");
                            cJSON *type = cJSON_GetObjectItem(msg, "type");
                            cJSON *reply = cJSON_GetObjectItem(msg, "result");
                            cJSON *status = cJSON_GetObjectItem(reply, "status");
                            s_heartbeat_registered = cJSON_IsString(type)
                                && strcmp(type->valuestring, "res") == 0
                                && cJSON_IsString(status)
                                && strcmp(status->valuestring, "registered") == 0
                                && !failure;
                        }
                    }
                    cJSON_Delete(msg);
                }
            }
            handle_invoke_request(reinterpret_cast<const char *>(p), msg_len,
                                  session_generation);
        }
        p += msg_len;
        remaining -= msg_len;
    }
}

// The session loop's pause after a turn with nothing to do: a tick while
// traffic is recent, then until the server sends something, for at most
// IDLE_POLL_MS (POWER_SAVE_POLL_MS in power save). What the session sends
// comes from queues that can't wake the wait, hence the cap. Polling every
// tick when quiet cost about a tenth of a core, taken from drawing the avatar.
static void idle_wait(esp_tls_t *tls, uint64_t last_busy_us) {
    uint64_t quiet_us = esp_timer_get_time() - last_busy_us;
    int fd = -1;
    if (quiet_us < IDLE_QUIET_MS * 1000ULL || esp_tls_get_bytes_avail(tls) > 0
        || esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
        vTaskDelay(1);
        return;
    }
    int ms = s_power_save && quiet_us > POWER_SAVE_QUIET_MS * 1000ULL ? POWER_SAVE_POLL_MS : IDLE_POLL_MS;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    lwip_select(fd + 1, &rd, nullptr, nullptr, &tv);
}

static session_result_t run_session(stack_monitor_t *stack) {
    session_result_t result = SESSION_FAILED;
    uint8_t *ws_buf = nullptr, *rx_buf = nullptr;
    uint8_t *svc_scratch = nullptr, *env_scratch = nullptr;
    uint8_t *tf_scratch = nullptr, *sr_scratch = nullptr;
    noise_ctrl_session_generation_t session_generation = 0;
    pending_json_body control_tx;
    identity_fetch identity = {};

    // 1. TLS connect
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls_init failed");
        return SESSION_FAILED;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;

    int r = esp_tls_conn_new_sync(s_noise_host, static_cast<int>(strlen(s_noise_host)),
                                  NOISE_PORT, &cfg, tls);
    if (r != 1) {
        ESP_LOGE(TAG, "TLS connect failed: %d", r);
        esp_tls_conn_destroy(tls);
        return SESSION_FAILED;
    }
    ESP_LOGI(TAG, "TLS connected to %s:%d (vm_id=%s)", s_noise_host, NOISE_PORT, s_vm_id);

    // 2. WebSocket upgrade
    noise_upgrade_result_t upgrade = ws_upgrade(tls);
    if (upgrade != NOISE_UPGRADE_OK) {
        esp_tls_conn_destroy(tls);
        // The edge refusing our bearer is an auth failure, not a network
        // blip. Saying so takes the 60s auth backoff instead of the 15s
        // network cap, and raises ws_auth_failed so app.c re-fetches the
        // token from fetch_vms — the only thing that can actually fix it.
        return upgrade == NOISE_UPGRADE_AUTH_REJECTED ? SESSION_AUTH_FAILED
                                                     : SESSION_FAILED;
    }
    ESP_LOGI(TAG, "WebSocket upgraded to %s", NOISE_PATH);

    // Allocate session-scoped buffers
    PsaCryptoBackend crypto;
    ClientSession session(crypto);

    size_t ws_buf_size = WS_BUF_SIZE;
    ws_buf = static_cast<uint8_t *>(
        heap_caps_malloc(ws_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ws_buf) ws_buf = static_cast<uint8_t *>(malloc(ws_buf_size));

    rx_buf = static_cast<uint8_t *>(
        heap_caps_malloc(WS_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rx_buf) rx_buf = static_cast<uint8_t *>(malloc(WS_RX_BUF_SIZE));

    // Crypto/framing scratch. PSRAM-preferred (with a heap fallback) so the
    // 12 KB buffers don't land in internal SRAM — anything under
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL would otherwise be served from the
    // scarce internal pool that WiFi/TLS/lwIP also need.
    svc_scratch = static_cast<uint8_t *>(
        heap_caps_malloc(OUT_SVC_SCRATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!svc_scratch) svc_scratch = static_cast<uint8_t *>(malloc(OUT_SVC_SCRATCH));
    env_scratch = static_cast<uint8_t *>(
        heap_caps_malloc(OUT_ENV_SCRATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!env_scratch) env_scratch = static_cast<uint8_t *>(malloc(OUT_ENV_SCRATCH));
    tf_scratch = static_cast<uint8_t *>(
        heap_caps_malloc(SVC_FRAME_SCRATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!tf_scratch) tf_scratch = static_cast<uint8_t *>(malloc(SVC_FRAME_SCRATCH));
    sr_scratch = static_cast<uint8_t *>(
        heap_caps_malloc(SVC_FRAME_SCRATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!sr_scratch) sr_scratch = static_cast<uint8_t *>(malloc(SVC_FRAME_SCRATCH));

    if (!ws_buf || !rx_buf || !svc_scratch || !env_scratch
        || !tf_scratch || !sr_scratch) {
        ESP_LOGE(TAG, "session buffer alloc failed");
        goto cleanup;
    }

    {  // Scoped block for event loop — avoids goto crossing variable init.

    // 3. Noise handshake
    if (!noise_handshake(tls, session, ws_buf, ws_buf_size)) {
        ESP_LOGE(TAG, "Noise handshake failed");
        result = SESSION_AUTH_FAILED;
        goto cleanup;
    }
    session_generation = noise_ctrl_next_session_generation(
        s_session_generation_counter);
    s_session_generation_counter = session_generation;

    // 4. Open POST /link-control stream
    {
        ApplicationRequestView req;
        req.verb = StringView("POST");
        req.path = StringView("/link-control");
        req.end_body = false;

        auto open_sws = session.StartOutboundApplicationRequest(
            ServiceType::Daemon, CTRL_STREAM_ID, req,
            ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
            ByteSpan(env_scratch, OUT_ENV_SCRATCH));
        if (!open_sws.ok()) {
            ESP_LOGE(TAG, "StartOutboundApplicationRequest failed: %s",
                     open_sws.status().str());
            goto cleanup;
        }
        if (!flush_outbound(tls, session, ws_buf)) goto cleanup;
        ESP_LOGI(TAG, "opened POST /link-control stream");
    }

    // Drain any stale results from previous sessions, including retained JSON.
    clear_pending_json_body(control_tx);
    pending_result stale;
    while (xQueueReceive(s_result_q, &stale, 0) == pdTRUE) {
        free(stale.request_id);
        cJSON_Delete(stale.result);
    }

    req_end_all();

    // 5. Queue link.register in the same slot as results so it too can pause
    // under DMA pressure while the event loop services RX and WS keepalives.
    {
        s_register_acked = false;
        s_heartbeat_registered = false;
        char *reg_json = build_register_json();
        if (!reg_json) {
            ESP_LOGE(TAG, "could not build link.register: int=%u/%u dma=%u/%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            goto cleanup;
        }
        control_tx = {reg_json, strlen(reg_json), 0, false, session_generation};
        control_tx.is_registration = true;
    }

    // Switch to non-blocking for the event loop
    {
        int sockfd = -1;
        if (esp_tls_get_conn_sockfd(tls, &sockfd) != ESP_OK || sockfd < 0) {
            ESP_LOGE(TAG, "get_conn_sockfd failed");
            goto cleanup;
        }
        int flags = lwip_fcntl(sockfd, F_GETFL, 0);
        lwip_fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    s_connected = true;
    notify_status("ws_connected");

    HeaderView hdr_storage[16];
    bool error = false;
    uint64_t last_activity_us = esp_timer_get_time();
    // last_rx_us tracks inbound liveness (app frames + keepalive pongs),
    // distinct from last_activity_us which also counts our TX.
    uint64_t last_rx_us = last_activity_us;
    bool heartbeat_sent = false;
    uint64_t last_heartbeat_us = 0;

    // Tunnel data-plane stream, multiplexed on this session. Deferred: it is
    // opened from inside the loop only after the link.register reply arrives
    // (s_register_acked), because the server authorizes /link-tunnel by checking
    // this connection has a registered device. Opening before the ack races the
    // registration and takes a 403 + slow reopen.
    tunnel_emit_ctx tun_ctx = {tls, &session, svc_scratch, env_scratch, ws_buf};
    noise_tunnel_emit_t tun_emit = {&tun_ctx, tunnel_open_stream, tunnel_send_body};
    bool tunnel_opened = false;
    uint64_t last_busy_us = esp_timer_get_time();

    while (!error && s_running) {
        bool did = false;

        // Ask for the agent's name (once) after the register ack, so the
        // request never races registration traffic.
        if (SHOW_AGENT_NAME && !identity.requested && s_register_acked) {
            identity.requested = true;
            if (!identity_request(tls, session, svc_scratch, env_scratch, ws_buf)) {
                error = true;
                break;
            }
        }

        // Once registration is acked, bring up the tunnel stream (once).
#if CONFIG_HOMEHUB_TUNNEL
        if (!tunnel_opened && s_register_acked) {
            tunnel_opened = true;
            if (!noise_tunnel_on_session_up(&tun_emit)) {
                ESP_LOGW(TAG, "tunnel stream open failed; checking Noise session health below");
            }
        }
#endif

        // RX: try to receive a WS frame (non-blocking)
        ssize_t n = ws_recv_frame_nonblock(tls, rx_buf, WS_RX_BUF_SIZE);
        if (n > 0) {
            did = true;
            last_activity_us = esp_timer_get_time();
            last_rx_us = last_activity_us;

            auto inbound = session.ProcessInboundWebSocketPayload(
                ConstByteSpan(rx_buf, static_cast<size_t>(n)),
                ByteSpan(tf_scratch, SVC_FRAME_SCRATCH),
                ByteSpan(sr_scratch, SVC_FRAME_SCRATCH),
                Span<HeaderView>(hdr_storage, 16));

            if (!inbound.ok()) {
                ESP_LOGW(TAG, "ProcessInbound failed: %s", inbound.status.str());
                // Too large only if the frame's plaintext can't fit the scratch:
                // a PSA allocation failure is ResourceExhausted too, and isn't.
                if (inbound.status.IsResourceExhausted() &&
                    static_cast<size_t>(n) > SVC_FRAME_SCRATCH + CryptoBackend::kAes256GcmTagSize) {
                    s_req_too_large = true;
                }
                error = true;
                break;
            }
            if (inbound.frame_status == InboundFrameStatus::Complete) {
                auto &frame = inbound.frame;
                bool is_tunnel = (frame.stream_id == TUNNEL_STREAM_ID);
                if (req_on_frame(frame)) {
                    // One of Muse's extra requests.
                } else if (frame.stream_id == IDENTITY_STREAM_ID) {
                    identity_on_frame(identity, frame);
                } else if (frame.kind == ServiceFrameKind::BodyChunk) {
                    if (is_tunnel) {
                        noise_tunnel_on_inbound(frame.body_chunk.data.data(),
                                                frame.body_chunk.data.size());
                    } else {
                        process_inbound_body_chunk(frame.body_chunk.data,
                                                   session_generation);
                    }
                } else if (frame.kind == ServiceFrameKind::Reset) {
                    if (is_tunnel) {
                        // Tunnel stream reset is non-fatal for the control
                        // session; drop the tunnel and keep control alive.
                        ESP_LOGW(TAG, "server reset tunnel stream");
                        noise_tunnel_on_session_down();
                    } else {
                        ESP_LOGW(TAG, "server reset stream %lld",
                                 (long long)frame.stream_id);
                        error = true;
                        break;
                    }
                } else if (frame.kind == ServiceFrameKind::Response) {
                    ESP_LOGI(TAG, "got response status=%d on stream %lld",
                             frame.response.status, (long long)frame.stream_id);
                    if (frame.response.status != 200) {
                        if (is_tunnel) {
                            ESP_LOGW(TAG, "server rejected /link-tunnel: %d",
                                     frame.response.status);
                            noise_tunnel_on_session_down();
                        } else {
                            ESP_LOGE(TAG, "server rejected /link-control: %d",
                                     frame.response.status);
                            error = true;
                            break;
                        }
                    }
                }
            }
        } else if (n == 0) {
            ESP_LOGI(TAG, "peer closed");
            error = true;
            break;
        } else if (n == -1) {
            ESP_LOGW(TAG, "rx error");
            error = true;
            break;
        } else if (n == -3) {
            // Control frame (ping/pong) — no app data, but the peer answered,
            // so the connection is bidirectionally alive. Reset RX liveness.
            last_rx_us = esp_timer_get_time();
            last_activity_us = last_rx_us;
        }
        // n == -2: no data available, continue

        // TX: keep exactly one control message in flight across loop turns.
        // A paused slot prevents dequeue/serialization of the next result.
        // Unreachable today: both generations come from the same run_session
        // local, so they cannot diverge. Kept deliberately — if generation
        // ownership ever moves, its absence would send a stale result on a
        // new session, silently and without a test to catch it.
        if (control_tx.json && !noise_ctrl_session_is_current(
                control_tx.session_generation, session_generation)) {
            clear_pending_json_body(control_tx);
        }
        if (!control_tx.json &&
            noise_ctrl_heartbeat_due(s_heartbeat_registered, heartbeat_sent,
                                     esp_timer_get_time(), last_heartbeat_us)) {
            char *json = strdup("{\"method\":\"link.heartbeat\"}");
            if (json) {
                control_tx = {json, strlen(json), 0, false, session_generation};
                control_tx.is_heartbeat = true;
            }
        }
        pending_result pr;
        while (!control_tx.json && noise_tx_has_dma_headroom(nullptr)
               && xQueueReceive(s_result_q, &pr, 0) == pdTRUE) {
            if (!noise_ctrl_session_is_current(
                    pr.session_generation, session_generation)) {
                ESP_LOGW(TAG, "dropping invoke result from stale control session");
                free(pr.request_id);
                cJSON_Delete(pr.result);
                continue;
            }
            // wrap_result_json consumes the result tree. Retain its output
            // directly; there is no second whole-message allocation/copy.
            char *json = wrap_result_json(pr.request_id, pr.result);
            free(pr.request_id);
            if (json) {
                control_tx = {json, strlen(json), 0, false, session_generation};
            }
        }
        if (control_tx.json) {
            size_t old_offset = control_tx.json_offset;
            bool old_prefix_sent = control_tx.prefix_sent;
            auto status = send_json_body_chunk(
                tls, session, control_tx, svc_scratch, env_scratch, ws_buf);
            if (status == json_send_status::Failed) {
                if (control_tx.is_heartbeat) {
                    ESP_LOGW(TAG, "heartbeat send failed");
                }
                error = true;
                break;
            }
            if (!control_tx.is_heartbeat &&
                (control_tx.json_offset != old_offset
                 || control_tx.prefix_sent != old_prefix_sent)) {
                did = true;
                last_activity_us = esp_timer_get_time();
            }
            if (status == json_send_status::Complete) {
                if (control_tx.is_heartbeat) {
                    heartbeat_sent = true;
                    last_heartbeat_us = esp_timer_get_time();
                }
                ESP_LOGI(TAG, "sent %s (%u bytes)",
                         control_tx.is_registration ? "link.register" :
                         control_tx.is_heartbeat ? "link.heartbeat" : "control message",
                         (unsigned)control_tx.json_len);
                clear_pending_json_body(control_tx);
            }
        }

#if CONFIG_MUSE_ENABLED
        {
            bool sent = false;
            if (!req_pump_tx(tls, session, svc_scratch, env_scratch, ws_buf, &sent)) {
                error = true;
                break;
            }
            if (sent) {
                did = true;
                last_activity_us = esp_timer_get_time();
            }
        }
#endif

        // Retry opening the tunnel stream if it went down while control is up
        // (e.g. transient backend 503). Rate-limited internally. Only after the
        // initial post-register open, so we never race registration.
        if (tunnel_opened) {
            noise_tunnel_maybe_reopen(&tun_emit);
        }

        // TX: drain the tunnel batch queue (multiplexed on this session).
        int tun_sent = noise_tunnel_pump_tx(&tun_emit);
        if (tun_sent > 0) {
            did = true;
            last_activity_us = esp_timer_get_time();
        } else if (tun_sent < 0) {
            error = true;
            break;
        }

        // Tunnel keepalive: ping the VM backend when the tunnel is idle and drop
        // it if pongs stop, so a half-open tunnel (WS alive, backend gone) is
        // detected and reopened by maybe_reopen above. Cheap and rate-limited
        // internally, so it's safe to call every loop iteration.
        if (tunnel_opened) {
            noise_tunnel_tick(&tun_emit);
        }

        // A seal failure permanently poisons Noise, including failures from
        // tunnel open/reopen or the void keepalive hook. Check once per turn:
        // WS pings can still succeed and otherwise keep a dead session alive.
        if (!session.isEstablished()) {
            ESP_LOGW(TAG, "Noise session no longer established; reconnecting");
            error = true;
            break;
        }

        // Keepalive: a WebSocket ping. The peer answers with a pong, so both
        // directions see traffic and middlebox idle timers (which drop the
        // idle server→device path otherwise) stay reset. This keeps the whole
        // WebSocket transport alive; Noise session health is checked above.
        if (!did) {
            uint64_t idle_us = esp_timer_get_time() - last_activity_us;
            if (idle_us >= static_cast<uint64_t>(KEEPALIVE_INTERVAL_MS) * 1000) {
                if (!ws_send_ping(tls)) {
                    ESP_LOGW(TAG, "keepalive ping failed");
                    error = true;
                    break;
                }
                ESP_LOGD(TAG, "keepalive ping sent");
                last_activity_us = esp_timer_get_time();
                did = true;
            }
        }

        stack_monitor_poll(stack);
        if (did) {
            last_busy_us = esp_timer_get_time();
        } else {
            idle_wait(tls, last_busy_us);
        }
    }

    s_connected = false;
    req_end_all();
    noise_tunnel_on_session_down();
    ESP_LOGI(TAG, "session ending (rx idle %llus at teardown)",
             (unsigned long long)((esp_timer_get_time() - last_rx_us) / 1000000));
    notify_status("ws_disconnected");
    // Clean exit (s_running went false) — no backoff needed.
    if (!error) result = SESSION_OK;
    }  // end event loop scope

cleanup:
    clear_pending_json_body(control_tx);
    free(ws_buf);
    free(identity.body);
    free(rx_buf);
    free(svc_scratch);
    free(env_scratch);
    free(tf_scratch);
    free(sr_scratch);
    esp_tls_conn_destroy(tls);
    return result;
}

static void noise_ctrl_task(void *arg) {
    (void)arg;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    uint32_t backoff = RECONNECT_MIN_MS;
    uint8_t short_failures = 0;
    while (s_running) {
        if (s_vm_id[0] == '\0') {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        uint64_t t0 = esp_timer_get_time();
        session_result_t result = run_session(&stack);
        uint64_t up_ms = (esp_timer_get_time() - t0) / 1000;
        if (update_reconnect_policy(result, up_ms, &backoff, &short_failures)) {
            notify_status("ws_refresh_needed");
        }
        if (result == SESSION_AUTH_FAILED) {
            notify_status("ws_auth_failed");
        }
        ESP_LOGI(TAG, "reconnect in %u ms (last session %llu ms)",
                 (unsigned)backoff, (unsigned long long)up_ms);
        stack_monitor_record(&stack);
        reconnect_delay(backoff);
    }
    stack_monitor_record(NULL);
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ---- Public API (extern "C") ------------------------------------------------

extern "C" void noise_ctrl_init(const char *node_id, const char *display_name,
                                noise_ctrl_status_cb on_status) {
    strncpy(s_node_id, node_id, sizeof(s_node_id) - 1);
    strncpy(s_display_name, display_name, sizeof(s_display_name) - 1);
    s_status_cb = on_status;
    s_connect_mutex = xSemaphoreCreateMutex();
    s_result_q = xQueueCreate(8, sizeof(pending_result));
    req_init();
    reserve_stack();
}

extern "C" void noise_ctrl_set_agent_name_cb(noise_ctrl_agent_name_cb cb) {
    s_agent_name_cb = cb;
}

extern "C" void noise_ctrl_set_command_cb(noise_ctrl_command_cb cb) {
    s_command_cb = cb;
}

extern "C" void noise_ctrl_set_power_save(bool on) {
    s_power_save = on;
}

extern "C" void noise_ctrl_set_host(const char *host) {
    snprintf(s_noise_host, sizeof(s_noise_host), "%s",
             host && *host ? host : NOISE_DEFAULT_HOST);
}

extern "C" bool noise_ctrl_connect(const char *vm_id, const char *auth_token,
                                   const char *network_ssid) {
    if (!vm_id || !*vm_id || !auth_token || !*auth_token || !s_connect_mutex) {
        return false;
    }

    xSemaphoreTake(s_connect_mutex, portMAX_DELAY);

    if (s_task) {
        ESP_LOGI(TAG, "stopping existing session before reconnect");
        s_running = false;
        while (s_task) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    strncpy(s_vm_id, vm_id, sizeof(s_vm_id) - 1);
    s_vm_id[sizeof(s_vm_id) - 1] = '\0';
    store_wifi_ssid(network_ssid);
    free(s_auth_token);
    s_auth_token = strdup(auth_token);

    s_running = true;
    release_stack();
    if (xTaskCreate(noise_ctrl_task, "noise_ctrl", NOISE_CTRL_STACK, nullptr, 5, &s_task) != pdPASS) {
        s_running = false;
        s_task = nullptr;
        reserve_stack();
        ESP_LOGE(TAG, "failed to start noise control task");
        xSemaphoreGive(s_connect_mutex);
        return false;
    }

    xSemaphoreGive(s_connect_mutex);
    return true;
}

extern "C" bool noise_ctrl_reconnect(const char *network_ssid) {
    if (!s_connect_mutex) return false;
    char vm_id[sizeof(s_vm_id)];
    xSemaphoreTake(s_connect_mutex, portMAX_DELAY);
    strlcpy(vm_id, s_vm_id, sizeof(vm_id));
    char *token = s_auth_token ? strdup(s_auth_token) : nullptr;
    xSemaphoreGive(s_connect_mutex);
    bool ok = token && noise_ctrl_connect(vm_id, token, network_ssid);
    free(token);
    return ok;
}

extern "C" void noise_ctrl_disconnect(void) {
    if (!s_connect_mutex) return;
    xSemaphoreTake(s_connect_mutex, portMAX_DELAY);
    s_running = false;
    // Wait for the task to exit, then drain any orphaned queue items
    // so their request_id strings and cJSON trees are freed.
    while (s_task != nullptr) {
        xSemaphoreGive(s_connect_mutex);
        vTaskDelay(pdMS_TO_TICKS(50));
        xSemaphoreTake(s_connect_mutex, portMAX_DELAY);
    }
    reserve_stack();
    if (s_result_q) {
        pending_result stale;
        while (xQueueReceive(s_result_q, &stale, 0) == pdTRUE) {
            free(stale.request_id);
            cJSON_Delete(stale.result);
        }
    }
    xSemaphoreGive(s_connect_mutex);
}

extern "C" bool noise_ctrl_is_connected(void) {
    return s_connected;
}

extern "C" bool noise_ctrl_is_running(void) {
    return s_task != nullptr;
}

extern "C" void noise_ctrl_send_command_result(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id, cJSON *result) {
    if (!session_generation || !request_id || !result) {
        if (result) cJSON_Delete(result);
        return;
    }
    queue_result(session_generation, request_id, result);
}

#if CONFIG_MUSE_ENABLED
extern "C" int64_t noise_ctrl_req_open(const char *verb, const char *path,
                                       const char *const *headers, bool end_body,
                                       noise_ctrl_req_cb cb, void *ctx) {
    if (!s_req_q || !s_connected || !verb || !path || !cb) return 0;
    size_t n = 0, len = strlen(verb) + strlen(path) + 3;
    for (const char *const *h = headers; h && *h; h++, n++) len += strlen(*h) + 1;
    if (n % 2 || n / 2 > REQ_MAX_HEADERS) return 0;
    char *blob = static_cast<char *>(malloc(len));
    if (!blob) return 0;
    char *p = blob;
    const char *first[] = {verb, path};
    for (const char *str : first) {
        size_t l = strlen(str) + 1;
        memcpy(p, str, l);
        p += l;
    }
    for (const char *const *h = headers; h && *h; h++) {
        size_t l = strlen(*h) + 1;
        memcpy(p, *h, l);
        p += l;
    }
    *p = '\0';
    int64_t id = s_req_next_id.fetch_add(1);
    req_op op = {req_op_kind::Open, end_body, id,
                 reinterpret_cast<uint8_t *>(blob), len, cb, ctx};
    if (!req_put(op, 0)) {
        free(blob);
        return 0;
    }
    return id;
}

extern "C" bool noise_ctrl_req_send(int64_t id, const void *data, size_t len,
                                    bool end_body, int wait_ms) {
    if (!s_req_q || id < REQ_STREAM_BASE) return false;
    uint8_t *copy = static_cast<uint8_t *>(malloc(len ? len : 1));
    if (!copy) return false;
    if (len) memcpy(copy, data, len);
    req_op op = {req_op_kind::Body, end_body, id, copy, len, nullptr, nullptr};
    int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (s_req_queued_bytes > 0
           && s_req_queued_bytes + (int32_t)len > REQ_QUEUE_BYTES) {
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            free(copy);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1);
    }
    int64_t left = deadline - esp_timer_get_time();
    if (!req_put(op, left > 0 ? pdMS_TO_TICKS(left / 1000) : 0)) {
        free(copy);
        return false;
    }
    return true;
}

extern "C" void noise_ctrl_req_cancel(int64_t id) {
    if (!s_req_q || id < REQ_STREAM_BASE) return;
    req_op op = {req_op_kind::Cancel, false, id, nullptr, 0, nullptr, nullptr};
    req_put(op, pdMS_TO_TICKS(100));
}
#endif  // CONFIG_MUSE_ENABLED
