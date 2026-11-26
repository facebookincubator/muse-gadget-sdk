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

#include "ble_commands.h"
#include "noise_control.h"
#if CONFIG_MUSE_WASM_APPS
#include "muse_apps.h"
#endif
#include "stack_monitor.h"
#include "muse_wdt.h"
#include "muse_stack.h"
#include "noise_upgrade.h"
#include "muse_server_hosts.h"
#include "noise_tunnel.h"
#include "noise_tunnel_internal.h"
#include "link_events.h"
#include "link_probe.h"
#include "tunnel_relay.h"
#include "ws_mask.h"
#if CONFIG_HOMEHUB_FAST_GCM
#include "fast_gcm.h"
#endif
#if CONFIG_HOMEHUB_ERROR_REPORT
#include "error_report.h"
#endif

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>

#include "esp_log.h"
#include "register_family.h"   // after sdkconfig.h, which esp_log.h brings
#include "esp_timer.h"
#include "esp_tls.h"
#include "network_proxy_tls.h"
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
#include "led_status.h"
#include "muse_bootguard.h"
#include "session_image_core.h"
#if CONFIG_MUSE_AVATAR_FACE
#include "mg_face.h"
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 || \
    CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 || \
    CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
#include "reterminal_battery.h"
#endif
#include "tunnel_netif.h"
#if CONFIG_MUSE_ENABLED
extern "C" {
#include "muse_state.h"
}
#endif
}

#ifdef PRODUCT_CONTROL_EXTENSION_HEADER
#include PRODUCT_CONTROL_EXTENSION_HEADER
#endif

#include <xplat/noise/core/ClientSession.h>
#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/ServiceCodec.h>

#if CONFIG_MUSE_GADGET_BLE_SESSION
#include "mg_ble.h"
#endif

using namespace musegadgets::noise::core;

static const char *TAG = "link.noise_ctrl";

#define NOISE_DEFAULT_HOST "hatch.metaaivm.com"
static char s_noise_host[256] = NOISE_DEFAULT_HOST;
#define NOISE_PATH       "/v1/noise"
#define NOISE_PORT       443
#define CTRL_STREAM_ID   1
// One-shot GET /api/gadget/me for the agent's name (TUNNEL_STREAM_ID is 2).
#define IDENTITY_STREAM_ID 3
// The reply is a few dozen bytes of JSON. Held only until it is parsed.
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
// A half-open TCP connection (the WAN drops, then comes back with the NAT
// mapping gone) keeps accepting our writes, so a send failure can take lwIP's
// whole retransmit budget (15+ minutes) to surface. Only inbound traffic proves
// the VM can still reach us: give up after two unanswered keepalive pings.
#define RX_IDLE_TIMEOUT_MS        150000
#define POWER_SAVE_QUIET_MS       1000    // noise_ctrl_set_power_save(): quiet this long,
#define POWER_SAVE_POLL_MS        100     // then poll this often
#define IDLE_QUIET_MS             50      // otherwise quiet this long,
#define IDLE_POLL_MS              10      // then wait on the socket at most this long
// Inbound frames the session reads per loop turn: everything waiting, up to
// these bounds (always at least one frame). Reading one frame per turn let
// small inbound frames (acknowledgements, requests to home devices) queue
// behind a turn's tunnel sends during a download. The byte bound keeps large
// frames (an upload's data) from taking a whole turn, so queued tunnel batches
// still go out often. Only tunnel data keeps a turn reading: a control-stream
// frame (a command, a daemon response) ends the turn's reads, as before, since
// a command's result waits in a short queue the turn drains one at a time.
#define SESSION_RX_FRAMES_PER_TURN 32
#define SESSION_RX_BYTES_PER_TURN  (16 * 1024)
#define STABLE_SESSION_MS         30000
#define VM_REFRESH_FAILURES       3

// On a board with the full UI but no PSRAM or tunnel, only control JSON and Muse's
// voice notes and live chat replies cross this session, and the whole session has
// to fit in what internal RAM Wi-Fi, TLS and the display leave, so the buffers
// below are smaller. The CYD's status screen leaves too little internal RAM for
// the usual Link buffers, so it takes the smaller ones too.
#if (CONFIG_MUSE_ENABLED && !CONFIG_SPIRAM && !CONFIG_HOMEHUB_TUNNEL) || \
    CONFIG_HOMEHUB_LED_BACKEND_CYD_ILI9341
#define SMALL_CONTROL_SESSION 1
#else
#define SMALL_CONTROL_SESSION 0
#endif

// Max inbound service frame scratch. Daemon control responses are modest JSON,
// but the tunnel stream (multiplexed on this session) carries ~8 KB IP-packet
// batches, so scratch must fit a full batch plus ServiceFrame/envelope overhead.
// Chat subscriptions deliver 16 KB body chunks plus framing, even on boards
// without PSRAM. Reserve enough inbound space for those frames. The ESP32-C3
// AI Passport and AtomS3 take 12 KB: larger buffers leave too little RAM for
// the session to stay up beside Wi-Fi and the UI.
// With PSRAM and extra daemon requests, use the protocol's largest frame, as
// Muse's chat session does, so display.draw_file can take large replies in few
// frames. Control and tunnel traffic alone need only 12 KB: the two buffers save
// 104 KB of PSRAM. WebSocket and handshake buffers remain independent of this
// service-frame scratch.
// Chat events arrive one per frame; a frame over that ends the session (it
// can't be skipped: Noise's nonces and the ordered framer would desync), the
// request in flight ends with NOISE_CTRL_REQ_TOO_LARGE, and the session
// reconnects.
#if !CONFIG_HOMEHUB_DAEMON_REQUESTS
#define SVC_FRAME_SCRATCH (12 * 1024)
#elif SMALL_CONTROL_SESSION && (CONFIG_MUSE_BOARD_AI_PASSPORT || \
                               CONFIG_MUSE_BOARD_M5STACK_ATOMS3)
#define SVC_FRAME_SCRATCH (12 * 1024)
#elif CONFIG_SPIRAM
#define SVC_FRAME_SCRATCH (64 * 1024)
#else
#define SVC_FRAME_SCRATCH (SMALL_CONTROL_SESSION ? 17 * 1024 : 12288)
#endif

// The smallest buffers: the boards that cannot allocate the session with the
// larger inbound buffers and the usual outbound buffers together (the
// Cardputer ADV, the ESP32-C3 AI Passport, the AtomS3).
// Keep this reduction local to the profiles that need it.
#if SMALL_CONTROL_SESSION && (CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV || \
                              CONFIG_MUSE_BOARD_AI_PASSPORT || \
                              CONFIG_MUSE_BOARD_M5STACK_ATOMS3)
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
// Free bytes kept in front of the outbound WebSocket payload in ws_buf, so the
// frame header (at most 14 bytes) can go in front of it and the frame leave in
// one TLS write. 32 also keeps the payload, which Noise seals in place,
// aligned to a cache line for the AES DMA.
#define WS_HEADROOM 32
#if CONFIG_SPIRAM
#define WS_BUF_SIZE ClientSession::kMaxOutboundWebSocketPayloadSize
#elif CARDPUTER_CONTROL_SESSION
#define WS_BUF_SIZE (4 * 1024)
#else
#define WS_BUF_SIZE (SMALL_CONTROL_SESSION ? 7 * 1024 : 16 * 1024)
#endif
// Inbound WebSocket frames: SVC_FRAME_SCRATCH plus framing and the AEAD tag.
#if SMALL_CONTROL_SESSION && (CONFIG_MUSE_BOARD_AI_PASSPORT || \
                             CONFIG_MUSE_BOARD_M5STACK_ATOMS3)
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
// because the server authorizes /api/gadget/tunnel by checking that a device has
// registered on this same Noise connection (checked by the server's ingress
// proxy). Opening earlier races the registration and
// draws a 403 that would otherwise wait out the tunnel reopen interval (~5s+).
// Session-task-owned: written before the event loop, read/reset within it.
static char s_register_req_id[40] = {0};
static bool s_register_acked = false;
static bool s_heartbeat_registered = false;
// The VM answered link.register with an error: not registered (no tunnel, no invokes), but no longer
// waiting either, so the session can close when idle. A new session registers again.
static bool s_register_rejected = false;
#if CONFIG_MUSE_GADGET_BLE_SESSION
// The device session's own lines (what came in that it doesn't handle, what
// it refused, link.register and its reply, session.ping) go to mg.session,
// which the Nordic UART mirrors (CONFIG_MUSE_GADGET_BLE_NUS_LOG_TAGS takes
// tags starting with "mg"), so a phone-side tester reads them with
// mg_ble_client.py log. Types and sizes only: never a token, a payload or a
// server's message text.
static const char *const MG_SESSION_TAG = "mg.session";
// When link.register finished going out on the device session
// (esp_timer_get_time(); 0: not yet). Session-task-owned.
static int64_t s_register_sent_us = 0;
#endif
static TaskHandle_t s_task = nullptr;
static muse_task_slot_t s_task_slot;   // muse_stack.h: the session task, started again on each connect

#define NOISE_CTRL_STACK 12288
#if CONFIG_MUSE_ENABLED && CONFIG_SPIRAM && !CONFIG_MUSE_STACKS_IN_PSRAM
// Muse's display and audio leave internal RAM fragmented by the time a session
// starts, often without a 12 KB block for this stack. Hold one from boot and
// hand it over just before the task is created. The idle task frees a
// deleted session task's stack, so let it run before either step. With
// CONFIG_MUSE_STACKS_IN_PSRAM the stack is in PSRAM (muse_stack.h) and
// nothing is held.
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
    int64_t queued_us;   // when it was queued (a BLE session's next one takes it within 10 s)
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

// Longest server Reset reason kept. Enough for every reason the server sends
// ("chunk for stream with completed body" is 36) while leaving the
// "server reset stream, code=N: " prefix room in a TUNNEL_FAILURE_MAX detail.
static constexpr size_t RESET_REASON_MAX = 40;

// Copy a Reset reason off the wire for logs and the connectivity history:
// printable ASCII only (anything else becomes '?'), cut at RESET_REASON_MAX.
static void copy_reset_reason(StringView reason, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    size_t len = std::min(reason.size(), out_len - 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)reason[i];
        out[i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    out[len] = '\0';
}

static constexpr int64_t WS_FRAME_TIMEOUT_US = 15000000;

// A TLS read or write that would block (the session's socket is non-blocking
// once it's up): wait a tick and try again. Every loop that calls this is
// bounded by its own deadline (WS_FRAME_TIMEOUT_US, 15 s) and gives up after
// it, so the Link session's watchdog (muse_wdt.h, 30 s) is fed here: a
// Wi-Fi stall the deadline will end can't trip it, while loop work that hangs
// still does.
static void io_retry_wait(void) {
    muse_wdt_feed(MUSE_WDT_LINK_SESSION);
    vTaskDelay(1);
}

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
            io_retry_wait();
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
// masking, and receive binary frames. The server expects a standard
// WS upgrade and then passes raw binary payloads to its ingress proxy.

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

    // Mask into a stack chunk, the header riding in the first one, so a short
    // frame (the handshake) is one TLS record. Frames from ws_buf take
    // ws_send_frame_inplace() instead.
    const uint8_t *mk = reinterpret_cast<const uint8_t *>(&mask_key);
    uint8_t chunk[512];
    memcpy(chunk, hdr, hdr_len);
    size_t fill = hdr_len;
    size_t off = 0;
    do {
        size_t n = len - off;
        if (n > sizeof(chunk) - fill) n = sizeof(chunk) - fill;
        for (size_t i = 0; i < n; i++) {
            chunk[fill + i] = payload[off + i] ^ mk[(off + i) & 3];
        }
        fill += n;
        if (write_all(tls, chunk, fill) != static_cast<ssize_t>(fill)) {
            return false;
        }
        off += n;
        fill = 0;
    } while (off < len);
    return true;
}

// Send a masked binary WS frame whose payload sits at `payload` with at least
// WS_HEADROOM writable bytes in front of it. The payload is masked in place
// and the header written in front of it, so the frame leaves in ONE TLS write:
// one record (up to CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN) instead of one per
// 512-byte chunk, and one AES-GCM pass. The payload is scratch afterwards.
static bool ws_send_frame_inplace(esp_tls_t *tls, uint8_t *payload, size_t len) {
    uint8_t hdr[14];
    size_t hdr_len;
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
    uint32_t mask_key = esp_random();
    memcpy(hdr + hdr_len, &mask_key, 4);
    hdr_len += 4;

    ws_mask_inplace(payload, len, reinterpret_cast<const uint8_t *>(&mask_key));

    uint8_t *frame = payload - hdr_len;
    memcpy(frame, hdr, hdr_len);
    size_t total = hdr_len + len;
    return write_all(tls, frame, total) == static_cast<ssize_t>(total);
}

#if CONFIG_HOMEHUB_PIPELINE_BENCH
// Bench builds drive the production frame sender over their own TLS link.
extern "C" bool noise_bench_ws_send_inplace(esp_tls_t *tls, uint8_t *payload, size_t len) {
    s_running = true;
    return ws_send_frame_inplace(tls, payload, len);
}
#endif

// Send a zero-length masked WebSocket ping (opcode 0x9). The peer (the load
// balancer or ingress proxy) replies with a pong, so this generates traffic in BOTH
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
// server's ingress proxy controls framing and won't fragment).
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
            io_retry_wait();
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
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
            else if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
            io_retry_wait();
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
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) { io_retry_wait(); continue; }
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
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
            else return -1;
        }
        payload_len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8]; have = 0;
        while (have < 8) {
            if (!s_running || esp_timer_get_time() >= deadline) return -1;
            ssize_t r = esp_tls_conn_read(tls, ext + have, 8 - have);
            if (r > 0) have += static_cast<size_t>(r);
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
            else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
        else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) io_retry_wait();
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
            io_retry_wait();
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
    // The server's ingress proxy rejects
    // anything that is not valid protobuf encoding. Sending the bare token
    // string made it drop the session immediately AFTER the crypto handshake
    // completed, so the device saw "session established", sent link.register,
    // and got a close ~60 ms later with nothing logged daemon-side.
    //
    // No field of that message carries a bearer token: its fields hold other
    // credentials and key-exchange data that the server validates separately
    // or refuses outright on the device's handshake path.
    // Our `vm_auth_token` authenticates through the
    // Authorization bearer header on the /v1/noise upgrade above, which is
    // what the server's bearer check reads. The correct owner payload is
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
    // The payload goes WS_HEADROOM into ws_buf so its frame header fits in
    // front of it (ws_send_frame_inplace).
    uint8_t *payload = ws_buf + WS_HEADROOM;
    while (session.HasOutboundWebSocketPayload()) {
        ByteSpan ws_span(payload, WS_BUF_SIZE);
        auto sws = session.WriteNextOutboundWebSocketPayload(ws_span);
        if (!sws.ok()) {
            ESP_LOGE(TAG, "WriteNextOutboundWebSocketPayload failed: %s",
                     sws.status().str());
            return false;
        }
        if (!ws_send_frame_inplace(tls, payload, sws.size())) {
            ESP_LOGE(TAG, "ws_send_binary failed");
            return false;
        }
    }
    return true;
}

// The largest control BodyChunk the session's transport takes. The device
// session over BLE (below) may take less: the client's max_message.
static size_t body_chunk_max(const ClientSession &) {
    return CTRL_BODY_CHUNK_MAX;
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
    bool is_error_report = false;
};

static void clear_pending_json_body(pending_json_body &body) {
    free(body.json);
    body = {};
}

static constexpr ServiceType GADGET_SERVICE = ServiceType::Gadget;
#define GADGET_CONTROL_PATH  "/api/gadget/control"
#define GADGET_TUNNEL_PATH   "/api/gadget/tunnel"
#define GADGET_IDENTITY_PATH "/api/gadget/me"

enum class json_send_status { Pending, Complete, Failed };

// Send at most one complete BodyChunk per loop turn, yielding to RX, tunnel
// traffic and WS keepalives between chunks even when DMA headroom stays high.
// Pending preserves ownership and framing state; only an attempted send failure
// is fatal. Never retry that failure: sealing can irrecoverably poison Noise.
// Session is ClientSession, or the device session over BLE (BleSessionOut).
// Whether a session's sends need the TLS path's internal DMA reserve
// (noise_tx_has_dma_headroom(): esp_tls writes from internal DMA memory, so
// Wi-Fi parks a send until 16 KB is free). The device session over BLE
// (BleSessionOut) doesn't: its messages are copied PSRAM first and leave as
// NimBLE notifications, so it never waits for internal RAM. On the
// Watcher's BLE-only build, which idles near 12 KB internal, that wait
// would park link.register forever, silently, right after POST /link-control.
template <typename Session>
struct tx_needs_dma_reserve {
    static constexpr bool value = true;
};

// The service a session's control stream is on: the VM's Gadget API over
// Wi-Fi (POST /api/gadget/control); the device session over BLE
// (BleSessionOut) keeps POST /link-control on the daemon, the route the
// phone's relay carries.
template <typename Session>
struct control_service {
    static constexpr ServiceType value = GADGET_SERVICE;
};

template <typename Session>
static json_send_status send_json_body_chunk(esp_tls_t *tls, Session &session,
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
    if (tx_needs_dma_reserve<Session>::value
        && (!noise_tx_has_dma_headroom(nullptr) || !noise_tx_has_contiguous_dma_headroom())) {
        return json_send_status::Pending;
    }

    size_t prefix_len = body.prefix_sent ? 0 : 4;
    size_t n = body.json_len - body.json_offset;
    size_t chunk_max = body_chunk_max(session);
    if (n > chunk_max - prefix_len) {
        n = chunk_max - prefix_len;
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
        control_service<Session>::value, CTRL_STREAM_ID, chunk,
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

// Open the streaming POST /api/gadget/tunnel on TUNNEL_STREAM_ID.
static bool tunnel_open_stream(void *vctx) {
    auto *c = static_cast<tunnel_emit_ctx *>(vctx);
    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView(GADGET_TUNNEL_PATH);
    req.end_body = false;
    auto sws = c->session->StartOutboundApplicationRequest(
        GADGET_SERVICE, TUNNEL_STREAM_ID, req,
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
        GADGET_SERVICE, TUNNEL_STREAM_ID, chunk,
        ByteSpan(c->svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(c->env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGE(TAG, "tunnel StartOutboundBodyChunk failed: %s",
                 sws.status().str());
        return false;
    }
    return flush_outbound(c->tls, *c->session, c->ws_buf);
}

// The same, encoding and encrypting the batch where it sits in its tunnel
// slot (noise_tunnel.cpp): no copy into the service, envelope and WebSocket
// buffers. It fails, like the copying path, when the session isn't
// established or another outbound message is mid-flight: every send here
// flushes its frames before returning, so the latter means the session is
// broken, and the caller drops the batch and rechecks the session.
static bool tunnel_send_body_inplace(void *vctx, uint8_t *buf, size_t headroom,
                                     size_t len, size_t tailroom) {
    auto *c = static_cast<tunnel_emit_ctx *>(vctx);
    auto r = c->session->SealBodyChunkInPlace(
        GADGET_SERVICE, TUNNEL_STREAM_ID,
        ByteSpan(buf, headroom + len + tailroom), headroom, len);
    if (!r.status.ok() || r.offset < 14) {
        ESP_LOGE(TAG, "tunnel SealBodyChunkInPlace failed: %s (offset %u)",
                 r.status.str(), (unsigned)r.offset);
        return false;
    }
    return ws_send_frame_inplace(c->tls, buf + r.offset, r.size);
}

// Reset TUNNEL_STREAM_ID so the server drops its record of the old stream
// before noise_tunnel_maybe_reopen reuses the id.
static bool tunnel_reset_stream(void *vctx) {
    auto *c = static_cast<tunnel_emit_ctx *>(vctx);
    ResetView reset;
    reset.code = ResetCode::Cancelled;
    reset.reason = StringView("tunnel reopen");
    auto sws = c->session->StartOutboundReset(
        GADGET_SERVICE, TUNNEL_STREAM_ID, reset,
        ByteSpan(c->svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(c->env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok()) {
        ESP_LOGE(TAG, "tunnel StartOutboundReset failed: %s",
                 sws.status().str());
        return false;
    }
    return flush_outbound(c->tls, *c->session, c->ws_buf);
}

// ---- Extra daemon requests (noise_ctrl_req_*) --------------------------------

// Opened by Muse and by an app's own code (CONFIG_HOMEHUB_DAEMON_REQUESTS);
// builds without it get the empty stubs below.
#if CONFIG_HOMEHUB_DAEMON_REQUESTS

// Stream ids from here up belong to these requests; lower ids are Link's own.
#define REQ_STREAM_BASE 16
#define REQ_MAX_STREAMS 3
#define REQ_QUEUE_LEN   6
#define REQ_MAX_HEADERS 6
// Body bytes queued at once. Without PSRAM a larger backlog leaves no block big
// enough for mbedTLS's per-write record buffer, and the session drops: on any
// board without it, Muse or not.
#if SMALL_CONTROL_SESSION || !CONFIG_SPIRAM
#define REQ_LOW_RAM 1
#else
#define REQ_LOW_RAM 0
#endif
#define REQ_QUEUE_BYTES (REQ_LOW_RAM ? 4096 : 16384)
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
    int64_t opened_us;   // Open: when noise_ctrl_req_open() made it (the 10 s wait counts from there)
    bool resent;         // Open: re-sent after a session's unasked end (once only)
    noise_ctrl_req_hdr_cb hdr_cb;   // Open: the response head's headers (nullptr: not wanted)
};

// The BLE device session keeps what a GET or HEAD request sent (its Open and
// body) until the first response bytes, so one in flight when the session ends
// unasked can go again in the next (mgcommands.h, the relay). Any other method
// fails with the session and its caller retries: nothing whose effect must
// happen at most once goes again blind.
#define REQ_REPLAY_MAX 2048
static bool req_resendable(const char *verb) {
    return !strcmp(verb, "GET") || !strcmp(verb, "HEAD");
}
static bool s_req_replay_on;   // set by the BLE device session's first begin (never on Wi-Fi)
static const char *const REQ_BLE_TAG = "mg.session";   // the BLE session's re-sends (the Nordic UART mirror shows mg.*)

struct req_stream {
    int64_t id;  // 0: free
    noise_ctrl_req_cb cb;
    void *ctx;
    uint8_t *replay;     // the Open's packed data, then the body sent so far (nullptr: none kept)
    size_t open_len, replay_len;
    bool open_end, body_end;   // the Open ended the body / the body's last chunk went
    bool resent;         // this is the re-send: a second unasked end fails it
    int64_t opened_us;
    noise_ctrl_req_hdr_cb hdr_cb;   // the response head's headers, before cb (nullptr: none)
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

static void req_drop(req_op &op, int status = -1) {
    if (op.kind == req_op_kind::Open) op.cb(op.ctx, status, nullptr, 0, true);
    free(op.data);
}

// Frees a stream's slot (and what it kept to re-send).
static void req_clear(req_stream *s) {
    free(s->replay);
    *s = {};
}

static void req_fail(req_stream *s, int status = -1) {
    req_stream gone = *s;
    req_clear(s);
    gone.cb(gone.ctx, status, nullptr, 0, true);
}

// The session is gone, and the requests it carried (their streams).
static void req_streams_end(void) {
    int status = s_req_too_large ? NOISE_CTRL_REQ_TOO_LARGE : -1;
    s_req_too_large = false;
    for (auto &s : s_req_streams) {
        if (s.id) req_fail(&s, status);
    }
}

// The session is gone, and every request with it: NOISE_CTRL_REQ_TOO_LARGE
// when a frame too big for this board's buffers ended it.
static void req_end_all(void) {
    req_op op;
    while (req_take(&op)) req_drop(op);
    req_streams_end();
}

#if CONFIG_MUSE_GADGET_BLE_SESSION
// The session ended with a state the gadget didn't ask for (closed,
// no_network): a request in flight that got no response bytes yet goes back
// to the front of the queue once, in its order, to be re-sent after the
// reopen and the link.register ack, its 10 s still counted from its open.
// One that got bytes, or was already re-sent, fails as before; what's queued
// waits for the next session. Returns the requests re-queued.
static int req_streams_requeue(int64_t *oldest_us) {
    req_stream *mine[REQ_MAX_STREAMS];
    int n = 0;
    for (auto &s : s_req_streams) {
        if (s.id && s.replay && !s.resent) mine[n++] = &s;
    }
    // Oldest id first in the queue: pushed to the front newest first, each Open ahead of its body.
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (mine[j]->id > mine[i]->id) {
                req_stream *t = mine[i];
                mine[i] = mine[j];
                mine[j] = t;
            }
        }
    }
    int requeued = 0;
    for (int i = 0; i < n; i++) {
        req_stream *s = mine[i];
        size_t body_len = s->replay_len - s->open_len;
        uint8_t *open = static_cast<uint8_t *>(malloc(s->open_len));
        uint8_t *body = body_len ? static_cast<uint8_t *>(malloc(body_len)) : nullptr;
        if (!open || (body_len && !body)) {
            free(open);
            free(body);
            continue;   // fails below with the rest
        }
        memcpy(open, s->replay, s->open_len);
        if (body_len) memcpy(body, s->replay + s->open_len, body_len);
        bool ok = true;
        if (body_len || (s->body_end && !s->open_end)) {
            req_op b = {req_op_kind::Body, s->body_end, s->id, body, body_len, nullptr, nullptr, 0, false, nullptr};
            s_req_queued_bytes += (int32_t)body_len;
            ok = xQueueSendToFront(s_req_q, &b, 0) == pdTRUE;
            if (!ok) {
                s_req_queued_bytes -= (int32_t)body_len;
                free(body);
            }
        }
        req_op o = {req_op_kind::Open, s->open_end, s->id, open, s->open_len, s->cb, s->ctx, s->opened_us, true, s->hdr_cb};
        s_req_queued_bytes += (int32_t)s->open_len;
        if (!ok || xQueueSendToFront(s_req_q, &o, 0) != pdTRUE) {
            s_req_queued_bytes -= (int32_t)s->open_len;
            free(open);
            continue;
        }
        ESP_LOGI(REQ_BLE_TAG, "request %lld was in flight with no reply when the session ended unasked: "
                                 "it goes again after the reopen", (long long)s->id);
        if (s->opened_us && (!*oldest_us || s->opened_us < *oldest_us)) *oldest_us = s->opened_us;
        free(s->replay);
        *s = {};
        requeued++;
    }
    return requeued;
}
#endif

template <typename Session>
static bool req_open(esp_tls_t *tls, Session &session, req_op &op,
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
    *slot = {};
    slot->id = op.id;
    slot->cb = op.cb;
    slot->ctx = op.ctx;
    slot->hdr_cb = op.hdr_cb;
    slot->resent = op.resent;
    slot->opened_us = op.opened_us;
    slot->open_end = op.end_body;
    if (op.resent) {
        ESP_LOGI(REQ_BLE_TAG, "request %lld re-sent after the session's unasked end", (long long)op.id);
    }
    if (s_req_replay_on && !op.resent && op.len <= REQ_REPLAY_MAX && req_resendable(verb)) {
        // Kept until the first response bytes: it may go again once.
        slot->replay = op.data;
        slot->open_len = slot->replay_len = op.len;
    } else {
        free(op.data);
    }
    return flush_outbound(tls, session, ws_buf);
}

// Sends at most one queued request operation per loop turn. False only if the
// transport failed.
template <typename Session>
static bool req_pump_tx(esp_tls_t *tls, Session &session,
                        uint8_t *svc_scratch, uint8_t *env_scratch,
                        uint8_t *ws_buf, bool *did) {
    // The loop turns every tick; the heap checks below walk the heap, so only
    // when there is something to send.
    if (!uxQueueMessagesWaiting(s_req_q)) return true;
    int32_t queued = s_req_queued_bytes;
    if (tx_needs_dma_reserve<Session>::value
        && (!noise_tx_has_dma_headroom_reclaiming(queued > 0 ? (size_t)queued : 0)
            || !noise_tx_has_contiguous_dma_headroom())) return true;
    if (tx_needs_dma_reserve<Session>::value && REQ_LOW_RAM
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
            if (s->replay) {
                // What a re-send would carry: the body so far, while it fits.
                uint8_t *grown = s->replay_len + op.len <= REQ_REPLAY_MAX
                                     ? static_cast<uint8_t *>(realloc(s->replay, s->replay_len + op.len))
                                     : nullptr;
                if (grown) {
                    memcpy(grown + s->replay_len, op.data, op.len);
                    s->replay = grown;
                    s->replay_len += op.len;
                    s->body_end = op.end_body;
                } else {
                    free(s->replay);
                    s->replay = nullptr;
                }
            }
            ok = flush_outbound(tls, session, ws_buf);
        } else {
            ESP_LOGW(TAG, "request %lld: body chunk failed: %s", (long long)op.id,
                     sws.status().str());
            req_fail(s);
        }
    } else if (s) {
        req_clear(s);
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
    if (frame.kind != ServiceFrameKind::Reset && s->replay) {
        free(s->replay);   // response bytes: it can't go again (it fails as before at a session's end)
        s->replay = nullptr;
        cur.replay = nullptr;
    }
    if (frame.kind == ServiceFrameKind::Response) {
        if (frame.response.end_body) *s = {};
        if (cur.hdr_cb) {
            for (size_t i = 0; i < frame.response.headers.size(); i++) {
                const HeaderView &h = frame.response.headers[i];
                cur.hdr_cb(cur.ctx, h.key.data(), h.key.size(), h.value.data(), h.value.size());
            }
        }
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
static void req_streams_end(void) {}
static bool req_on_frame(const DecodedServiceFrame &) { return false; }
#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS

// ---- Agent identity ----------------------------------------------------------

// Reply to GET /api/gadget/me, collected across the Response and BodyChunk frames.
struct identity_fetch {
    bool requested;
    bool done;
    char *body;
    size_t len;
};

// Ask the VM for the agent's name. Returns false only if the transport
// failed; a refused request just leaves the name unknown.
static bool identity_request(esp_tls_t *tls, ClientSession &session,
                             uint8_t *svc_scratch, uint8_t *env_scratch,
                             uint8_t *ws_buf) {
    ApplicationRequestView req;
    req.verb = StringView("GET");
    req.path = StringView(GADGET_IDENTITY_PATH);
    req.end_body = true;
    auto sws = session.StartOutboundApplicationRequest(
        GADGET_SERVICE, IDENTITY_STREAM_ID, req,
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
    // The VM wraps the payload: {"ok":true,"result":{"muse_name":...}}.
    cJSON *root = f.body ? cJSON_ParseWithLength(f.body, f.len) : nullptr;
    cJSON *result = root ? cJSON_GetObjectItem(root, "result") : nullptr;
    cJSON *name = cJSON_GetObjectItem(result, "muse_name");
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
            ESP_LOGW(TAG, "server rejected /api/gadget/me: %d", frame.response.status);
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

extern "C" const char muse_device_guide_md[];

// Unused on a build without the display, image or face commands.
[[maybe_unused]] static cJSON *string_param(const char *description) {
    cJSON *param = cJSON_CreateObject();
    cJSON_AddStringToObject(param, "type", "string");
    if (description) {
        cJSON_AddStringToObject(param, "description", description);
    }
    return param;
}

#if CONFIG_HOMEHUB_NET_FORWARD && CONFIG_HOMEHUB_TUNNEL && !CONFIG_MUSE_GADGET_BLE_STANDALONE
static cJSON *integer_param(void) {
    cJSON *param = cJSON_CreateObject();
    cJSON_AddStringToObject(param, "type", "integer");
    return param;
}
#endif

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS && !CONFIG_MUSE_SESSION_IMAGE
// The optional `row` of display.draw_url and display.draw_file (neither is
// registered beside display.show_image).
static cJSON *row_params(void) {
    cJSON *optional = cJSON_CreateObject();
    cJSON *top_param = cJSON_CreateObject();
    cJSON_AddStringToObject(top_param, "type", "integer");
    cJSON_AddStringToObject(top_param, "description",
#if CONFIG_HOMEHUB_LED_BACKEND_MUSE
                            "Row to draw the top of the image at; by default a JPEG "
                            "is centred down the screen and raw data starts at 0.");
#else
                            "Row to draw the top of the image at; default 0.");
#endif
    cJSON_AddItemToObject(optional, "row", top_param);
    return optional;
}
#endif

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
    // A control character escapes to six JSON bytes (\u00XX), which
    // link.register has no room for. Only an SSID, which may hold any byte,
    // can have one: leave it out rather than fail to register.
    for (const unsigned char *c = (const unsigned char *)value; *c; c++) {
        if (*c < 0x20) return;
    }
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
    // Every build registers as "gadget"; model_id is the board's overlay id
    // (register_family.h).
    cJSON_AddStringToObject(params, "device_family", register_device_family());
    cJSON_AddStringToObject(params, "model_id", register_model_id());
    cJSON_AddBoolToObject(params, "is_wakeup_supported", false);
    cJSON *metadata = nullptr;
    // metadata.model_name, once: the board's short product name where its
    // overlay sets one (StopWatch, Watcher, ...,
    // CONFIG_HOMEHUB_MODEL_NAME), so the agent names the gadget in hand; else
    // the model name every overlay sets (CONFIG_MUSE_DEVICE_MODEL_NAME).
    if (register_model_name()[0]) {
        add_register_metadata_string(&metadata, params, "model_name", register_model_name());
    } else {
        add_register_metadata_string(&metadata, params, "model_name",
                                     CONFIG_MUSE_DEVICE_MODEL_NAME);
    }
    add_register_metadata_string(&metadata, params, "network_ssid", wifi_ssid);
#if CONFIG_HOMEHUB_TUNNEL
    // The LAN the tunnel reaches, where LAN devices find a net.forward port. A
    // new station address aborts this session's TCP connection; the next
    // session sends it.
    char lan_ip[16], lan_prefix[4];
    int prefix = 0;
    if (tunnel_relay_lan_address(lan_ip, sizeof(lan_ip), &prefix)) {
        snprintf(lan_prefix, sizeof(lan_prefix), "%d", prefix);
        add_register_metadata_string(&metadata, params, "lan_ipv4", lan_ip);
        add_register_metadata_string(&metadata, params, "lan_prefix", lan_prefix);
    }
#endif

    cJSON *commands = cJSON_CreateObject();
    add_command(commands, "device.health",
                "Report Link health: battery level (percent), voltage, whether "
                "it is charging or on USB power (each null if not measurable), "
                "and connectivity_events, recent Wi-Fi, session and tunnel "
                "outages with causes",
                nullptr, nullptr);
    add_command(commands, "device.guide",
                "This device's usage guide (Markdown): what it is, its "
                "controls and limits."
#if CONFIG_MUSE_WASM_APPS
                " Also returns structured capabilities: board, devices and WASM API discovery."
#endif
                , nullptr, nullptr);
#if CONFIG_HOMEHUB_TUNNEL && !CONFIG_MUSE_GADGET_BLE_STANDALONE
    // Offered only where app.c handles it: a BLE-only gadget has no network to scan.
    add_command(commands, "device.discover",
                "Scan local network for devices (ARP, mDNS, SSDP, WSD, SNMP, NetBIOS, vendor)",
                nullptr, nullptr);
    cJSON_AddNumberToObject(
        cJSON_GetObjectItem(commands, "device.discover"), "timeout_ms", 90000);
#if CONFIG_HOMEHUB_NET_FORWARD
    // Terse: the VM's tunnel code calls it, and link.register has 8 KB in all
    // (docs/home-link-network.md has the rest).
    cJSON *forward_required = cJSON_CreateObject();
    cJSON_AddItemToObject(forward_required, "proto", string_param(nullptr));
    cJSON_AddItemToObject(forward_required, "port", integer_param());
    cJSON_AddItemToObject(forward_required, "ttl_s", integer_param());
    cJSON *forward_optional = cJSON_CreateObject();
    cJSON_AddItemToObject(forward_optional, "to_port", integer_param());
    add_command(commands, "net.forward",
                "LAN port to VM to_port (default port) for ttl_s s, 0 closes; udp|tcp",
                forward_required, forward_optional);
#endif
#endif

#if CONFIG_HOMEHUB_DISPLAY_COMMANDS
    int disp_w, disp_h;
    if (led_status_display_info(&disp_w, &disp_h)) {
        // The agent prepares images from this, so it states the bit depth
        // and, for e-paper, the exact inks.
        int bits = led_status_display_bits();
        char desc[1536];
        const char *restore;    // display.show_animation's description
#if CONFIG_HOMEHUB_LED_MATRIX
        restore = "Clear the image and bring back the status picture.";
        snprintf(desc, sizeof(desc),
                 "Download an image and draw it on the %dx%d LED matrix, "
                 "1 bit per pixel: each pixel is lit or dark. Takes a baseline "
                 "(not progressive) JPEG, shrunk 1/2, 1/4 or 1/8 on the device "
                 "if needed and centred across, or raw RGB565 (high byte first, "
                 "%d bytes per row). Pixels brighter than mid-gray light up and "
                 "the rest stay dark, with no dithering, so draw it in white "
                 "#ffffff (lit) on black #000000 (dark) at exactly %dx%d: "
                 "icons, symbols and simple shapes work, photos and words "
                 "don't. Plain http:// uses the least device memory. Replies "
                 "when the image is drawn. Hides the status picture until "
                 "display.show_animation.",
                 disp_w, disp_h, disp_w * 2, disp_w, disp_h);
#else
        bool mono = bits == 1;
        bool epaper = bits == 1 || bits == 2 || bits == 4;
        restore = epaper ? "Clear the image and bring back the status screen "
                           "and the agent's name."
                         : "Clear the image and bring back the animation and "
                           "the agent's name.";
        // 2 bits are four inks on the Waveshare 1.54G, four grays elsewhere.
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
        bool inks4 = bits == 2;
#else
        bool inks4 = false;
#endif
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
                 : inks4 ? "four-colour e-paper screen, 2 bits per pixel"
                 : bits == 4 ? "six-colour e-paper screen (E Ink Spectra 6), "
                               "4 bits per pixel"
                             : bits == 2 ? "four-level grayscale e-paper screen, 2 bits per pixel"
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
                 : inks4 ? " The screen has only four inks: black #000000, "
                               "white #ffffff, yellow #ffff00 and red #ff0000, "
                               "shown exactly. There is no green or blue ink: "
                               "saturated greens and blues come out nearly "
                               "black. The device dithers every other "
                               "colour and gray to dots of these, so photos "
                               "look grainy and muted, and text, charts and "
                               "flat artwork are sharpest drawn in those four "
                               "colours only. It refreshes once the whole image "
                               "is in, taking 9 to 20 s and flashing, so send "
                               "one whole screen rather than several parts. "
                               "The image stays on screen without power."
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
                             : bits == 2 ? " Images are quantized to four gray levels. "
                                           "The panel refreshes the whole image at once and retains "
                                           "it without power; send one whole screen rather than "
                                           "several parts."
                                         : ""
#if !CONFIG_HOMEHUB_LED_BACKEND_MUSE
                 , epaper ? "status screen" : "animation"
#endif
                 );
#endif
#if !CONFIG_MUSE_GADGET_BLE_STANDALONE
        // A BLE-only gadget has no network of its own (image_fetch.c refuses
        // a URL there): only display.draw_file, from the VM.
        cJSON *url_required = cJSON_CreateObject();
        cJSON_AddItemToObject(url_required, "url",
                              string_param("http:// or https:// URL of the image."));
        add_command(commands, "display.draw_url", desc, url_required, row_params());
        // The six-colour e-paper may first finish a status screen refresh,
        // then takes as long again for the image.
        cJSON_AddNumberToObject(
            cJSON_GetObjectItem(commands, "display.draw_url"), "timeout_ms",
            bits == 4 ? 120000 : 60000);
#endif
#if CONFIG_MUSE_SESSION_IMAGE
        // display.show_image (below) shows a file from the VM, for a while
        // or until dismissed: one way to show a picture, so Muse never picks
        // the older one (with both registered, Muse kept using
        // draw_file, which worked earlier in the same chat, and timed the
        // 5 s itself with display.show_animation). draw_file still answers.
#else
        cJSON *path_required = cJSON_CreateObject();
        cJSON_AddItemToObject(path_required, "path",
                              string_param("Path of the image file on this VM, relative, "
                                           "as the VM's file API takes it, e.g. "
                                           "workspace/picture.jpg."));
#if CONFIG_MUSE_GADGET_BLE_STANDALONE
        // display.draw_url isn't offered here, so this one says it all
        // (desc's room, not another buffer: this runs on the session task).
        size_t used = strlen(desc);
        snprintf(desc + used, sizeof(desc) - used, " Takes a path on this VM: the device "
                 "reads the file over its connection to the VM.");
        add_command(commands, "display.draw_file", desc, path_required, row_params());
#else
        add_command(commands, "display.draw_file",
                    "Draw an image file from this VM on the screen. The same as "
                    "display.draw_url, with the same image formats and screen, "
                    "but takes a path on the VM instead of a URL: the device "
                    "reads the file over its connection to the VM, so the file "
                    "need not be on the internet. Replies when the image is "
                    "drawn.",
                    path_required, row_params());
#endif
        cJSON_AddNumberToObject(
            cJSON_GetObjectItem(commands, "display.draw_file"), "timeout_ms",
            bits == 4 ? 120000 : 60000);
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179
        const char *gray_command = "display.draw_gray4_url";
        const char *gray_desc = "Draw an image with four gray levels at 800 by 480.";
#elif CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
        const char *gray_command = "display.draw_gray16_url";
        const char *gray_desc = "Draw an image with 16 gray levels at 1872 by 1404.";
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 || CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
        char gray_help[256];
        snprintf(gray_help, sizeof(gray_help),
                 "%s Same JPEG and raw RGB565 formats as display.draw_url. "
                 "Clears the canvas before drawing and replies after one full refresh.", gray_desc);
        cJSON *gray_required = cJSON_CreateObject();
        cJSON_AddItemToObject(gray_required, "url",
                              string_param("HTTP or HTTPS URL of the image."));
        add_command(commands, gray_command, gray_help, gray_required, row_params());
        cJSON_AddNumberToObject(cJSON_GetObjectItem(commands, gray_command), "timeout_ms", 150000);
#endif
#endif
        add_command(commands, "display.show_animation", restore, nullptr, nullptr);
#if CONFIG_MUSE_SESSION_IMAGE
        // A use case taught to Muse by this description alone (session_image.h):
        // what the command is for, what the screen takes, and how to get there.
        // In desc's room again (add_command copied the last one).
        snprintf(desc, sizeof(desc),
                 "Show a picture on this gadget's screen for duration_ms, then go "
                 "back to what it was showing. This is how to show any picture "
                 "here, including one you make for the user: make or find it "
                 "first (generate one if they ask for a new or random picture), "
                 "save it on this VM as a baseline (not progressive) JPEG of "
                 "%dx%d pixels, quality about 70 and under 30 KB (it travels over "
                 "Bluetooth: smaller shows sooner), and pass its path. The screen "
                 "is %s%dx%d: keep the subject in the middle. A larger JPEG is only "
                 "shrunk by 1/2, 1/4 or 1/8, so resize it yourself. The gadget "
                 "downloads the file over its own connection and replies once the "
                 "picture is on screen, with its size and how long the download "
                 "took; an error says what to fix.",
                 disp_w, disp_h, led_status_display_round() ? "round (the corners are cut off), " : "",
                 disp_w, disp_h);
        cJSON *show_required = cJSON_CreateObject();
        cJSON_AddItemToObject(show_required, "path",
                              string_param("Path of the JPEG on this VM, relative to the "
                                           "home folder, e.g. workspace/picture.jpg."));
        cJSON *show_optional = cJSON_CreateObject();
        cJSON *ms_param = cJSON_CreateObject();
        cJSON_AddStringToObject(ms_param, "type", "integer");
        cJSON_AddStringToObject(ms_param, "description",
                                "How long to show it, in ms: 1000 to 60000, default 5000; "
                                "0 keeps it until a tap or display.show_animation.");
        cJSON_AddItemToObject(show_optional, "duration_ms", ms_param);
        add_command(commands, "display.show_image", desc, show_required, show_optional);
        cJSON_AddNumberToObject(cJSON_GetObjectItem(commands, "display.show_image"),
                                "timeout_ms", SIMG_COMMAND_TIMEOUT_MS);
#endif
    }
#endif

#if CONFIG_MUSE_AVATAR_FACE
    {
        // The avatar face (mg_face.h): Muse tells the gadget its face
        // picture is ready; the gadget reads it itself (GET /fs/raw ranges).
        char face_desc[480];
        snprintf(face_desc, sizeof(face_desc),
                 "Tell this gadget that its watch-face picture, the user's dark avatar, is ready "
                 "at %s: a baseline (not progressive) JPEG of at most %u bytes, black background. "
                 "The gadget reads the file over its own connection, stores it and shows its home "
                 "face over it, then replies with the size it took; an error says what to fix. "
                 "Call it after writing or updating that file.",
                 mg_face_own_path(), (unsigned)mg_face_max_bytes());
        cJSON *face_required = cJSON_CreateObject();
        cJSON_AddItemToObject(face_required, "path",
                              string_param("The JPEG's path on this VM: exactly the path above."));
        cJSON *len_param = cJSON_CreateObject();
        cJSON_AddStringToObject(len_param, "type", "integer");
        cJSON_AddStringToObject(len_param, "description", "The file's size in bytes.");
        cJSON_AddItemToObject(face_required, "byte_len", len_param);
        cJSON *face_optional = cJSON_CreateObject();
        cJSON_AddItemToObject(face_optional, "etag", string_param("The file's ETag, if known."));
        add_command(commands, "face.set", face_desc, face_required, face_optional);
        cJSON_AddNumberToObject(cJSON_GetObjectItem(commands, "face.set"), "timeout_ms", 120000);
    }
#endif

#if CONFIG_MUSE_WASM_APPS
    muse_apps_advertise(commands);   // app.run and app.stop
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
#if CONFIG_HOMEHUB_WAVESHARE_154G_SHTC3
    add_command(commands, "sensors.read",
                "Read the onboard air sensor: temperature in degrees Celsius "
                "and relative humidity in percent. Each reading has its age "
                "in seconds; a sensor with no recent reading is null.",
                nullptr, nullptr);
#endif

#if CONFIG_MUSE_WAKEWORD_ENABLE
    add_command(commands, "wakeword.disable",
                "Turn off hands-free \"Hey Muse\" listening at once, ending any "
                "hands-free voice note in progress. Only someone at the device "
                "can turn it back on. Kept across restarts. Returns enabled and "
                "listening (both false).",
                nullptr, nullptr);
#endif

#if CONFIG_MUSE_WATCHER_CAMERA || CONFIG_MUSE_P4_CSI_CAMERA || CONFIG_MUSE_MOSAICO_CAMERA
    add_command(commands, "camera.capture",
#if CONFIG_MUSE_WATCHER_CAMERA
                "Capture one still JPEG frame from the SenseCAP Watcher camera. "
#elif CONFIG_MUSE_P4_CSI_CAMERA
                "Capture one still 1280x960 JPEG frame from the device camera. "
#else
                "Capture one still JPEG frame from the device camera. "
#endif
                "The frame is returned as base64 only when this command is explicitly invoked.",
                nullptr, nullptr);
    cJSON_AddNumberToObject(
        cJSON_GetObjectItem(commands, "camera.capture"), "timeout_ms", 30000);
#endif

#if CONFIG_HOMEHUB_BLE_COMMANDS
    ble_commands_register(commands);
#endif
#if CONFIG_MUSE_BOOTGUARD_SAFE_LINK_QUIET
    // SAFE mode on a build without musegadgets: link.register lists
    // only what says how the device is.
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        cJSON *c = commands->child;
        while (c) {
            cJSON *next = c->next;
            if (strcmp(c->string, "device.health") != 0) {
                cJSON_Delete(cJSON_DetachItemViaPointer(commands, c));
            }
            c = next;
        }
    }
#endif

#ifdef PRODUCT_CONTROL_REGISTER_COMMANDS
    PRODUCT_CONTROL_REGISTER_COMMANDS(commands);
#endif

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

// ---- Async results ---------------------------------------------------------

// Queued result from an asynchronous command.
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
    pr.queued_us = esp_timer_get_time();
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

#ifdef PRODUCT_CONTROL_EXTENSION_IMPL
#include PRODUCT_CONTROL_EXTENSION_IMPL
#endif

// ---- Inbound message dispatch -----------------------------------------------

extern "C" cJSON *noise_ctrl_device_health_json(bool for_bug_report) {
    char wifi_ssid[sizeof(s_wifi_ssid)];
    copy_wifi_ssid(wifi_ssid, sizeof(wifi_ssid));

    cJSON *health = cJSON_CreateObject();
    cJSON_AddStringToObject(health, "overall", "ok");
#if CONFIG_MUSE_BOOTGUARD_SAFE_LINK_QUIET
    if (muse_bootguard_mode() == MUSE_BOOT_SAFE) {
        cJSON_AddBoolToObject(health, "safe", true);   // SAFE mode: the Link's commands are off
    }
#endif
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
#elif CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 || \
    CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 || \
    CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951
    // The status-screen boards do not populate muse_state_power().
    // Only voltage and estimated charge are measured; other fields stay null.
    int rt_mv, rt_pct;
    if (reterminal_battery_read(&rt_mv, &rt_pct)) {
        battery_pct = cJSON_CreateNumber(rt_pct);
        battery_mv = cJSON_CreateNumber(rt_mv);
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
    cJSON_AddItemToObject(health, "metrics", metrics);
#if CONFIG_HOMEHUB_TUNNEL
    cJSON_AddBoolToObject(health, "tunnel_connected", noise_tunnel_is_connected());
    // tunnel_connected only means the stream is open. Idle time and counters
    // show whether it is passing traffic. No control-side idle time: this
    // request itself just arrived on control.
    tunnel_stats_t tun_stats;
    tunnel_netif_get_stats(&tun_stats);
    cJSON *tunnel = cJSON_CreateObject();
    uint32_t tun_idle_s = noise_tunnel_rx_idle_s();
    if (tun_idle_s != UINT32_MAX) {
        cJSON_AddNumberToObject(tunnel, "rx_idle_seconds", tun_idle_s);
    }
    cJSON_AddNumberToObject(tunnel, "rx_packets", tun_stats.rx_pkts);
    cJSON_AddNumberToObject(tunnel, "rx_bytes", tun_stats.rx_bytes);
    cJSON_AddNumberToObject(tunnel, "tx_packets", tun_stats.tx_pkts);
    cJSON_AddNumberToObject(tunnel, "tx_bytes", tun_stats.tx_bytes);
    cJSON_AddNumberToObject(tunnel, "tx_dropped", tun_stats.tx_dropped);
    cJSON_AddItemToObject(health, "tunnel", tunnel);
#endif
    link_events_add_json(health, "connectivity_events",
                         for_bug_report ? LINK_EVENTS_CAPACITY
                                        : LINK_EVENTS_HEALTH_MAX);
    if (!for_bug_report && wifi_ssid[0]) {
        cJSON_AddStringToObject(health, "network_ssid", wifi_ssid);
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(health, "version",
                            desc ? desc->version : "unknown");
    return health;
}

static void send_device_health(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id) {
    cJSON *health = noise_ctrl_device_health_json(false);
    if (!health) return;
    char *payload = cJSON_PrintUnformatted(health);
    cJSON_Delete(health);
    if (!payload) return;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddStringToObject(result, "payload_json", payload);
    free(payload);
    queue_result(session_generation, request_id, result);
}

static void send_device_guide(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id) {
    cJSON *result = cJSON_CreateObject();
    cJSON *payload = cJSON_AddObjectToObject(result, "payload");
    cJSON *guide = cJSON_CreateStringReference(muse_device_guide_md);
    if (!cJSON_AddBoolToObject(result, "ok", true) || !payload
        || !cJSON_AddStringToObject(payload, "format", "markdown")
        || !cJSON_AddItemToObject(payload, "guide", guide)
#if CONFIG_MUSE_WASM_APPS
        /* Once guide is attached, result owns it, including on a later failure. */
        || !(guide = nullptr, muse_apps_add_capabilities(payload))
#endif
        ) {
        cJSON_Delete(guide);
        cJSON_Delete(result);
        result = cJSON_CreateObject();
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(error, "code", "out_of_memory");
        cJSON_AddStringToObject(error, "message", "could not build the device guide");
        if (!cJSON_AddItemToObject(result, "error", error)) cJSON_Delete(error);
    }
    queue_result(session_generation, request_id, result);
}

#if CONFIG_MUSE_GADGET_BLE_SESSION
// A name from a message for the log (its type, method, event or error
// code): at most 32 characters of [A-Za-z0-9._:-], "-" when it isn't a
// string, "?" when it holds anything else. Never more of the message.
static const char *log_name(const cJSON *j, char out[34]) {
    if (!cJSON_IsString(j) || !j->valuestring) return "-";
    const char *s = j->valuestring;
    size_t n = strlen(s);
    if (!n || n > 32) return "?";
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return "?";
    }
    memcpy(out, s, n + 1);
    return out;
}

// A control message on the device session that nothing here handles (a
// reply nobody waits for, an event or a method this gadget doesn't know):
// the placeholder for what the server sends back through the phone. Its
// type and size are logged, never its content, and it's accepted: its bytes
// were credited back when the session took it, and the session stays open.
static void log_control_unhandled(const cJSON *root, size_t len) {
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(MG_SESSION_TAG, "refused a control message that isn't a JSON object (%u bytes)", (unsigned)len);
        return;
    }
    char type[34], method[34], event[34];
    ESP_LOGI(MG_SESSION_TAG, "unhandled control message: type %s, method %s, event %s (%u bytes): accepted",
             log_name(cJSON_GetObjectItem(root, "type"), type),
             log_name(cJSON_GetObjectItem(root, "method"), method),
             log_name(cJSON_GetObjectItem(root, "event"), event), (unsigned)len);
}
#endif

static void handle_invoke_request(
    const char *data, size_t len,
    noise_ctrl_session_generation_t session_generation) {
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
#if CONFIG_MUSE_GADGET_BLE_SESSION
        ESP_LOGW(MG_SESSION_TAG, "refused a control message that isn't JSON (%u bytes)", (unsigned)len);
#endif
        return;
    }

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type_j) && strcmp(type_j->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(root, "event");
        if (cJSON_IsString(event)
            && (strcmp(event->valuestring, "link.unpaired") == 0
                || strcmp(event->valuestring, "node.unpaired") == 0)) {
            ESP_LOGW(TAG, "received link.unpaired event from server");
            notify_status("ws_unpaired");
        }
#if CONFIG_MUSE_GADGET_BLE_SESSION
        else {
            log_control_unhandled(root, len);
        }
#endif
        cJSON_Delete(root);
        return;
    }

    cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || !method->valuestring
        || strcmp(method->valuestring, "link.invoke") != 0) {
#if CONFIG_MUSE_GADGET_BLE_SESSION
        log_control_unhandled(root, len);
#endif
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
#if CONFIG_MUSE_GADGET_BLE_SESSION
        ESP_LOGW(MG_SESSION_TAG, "refused a link.invoke without an id (%u bytes)", (unsigned)len);
#endif
        cJSON_Delete(root);
        return;
    }

    ESP_LOGI(TAG, "invoke request: command=%s", command);

#ifdef PRODUCT_CONTROL_INVOKE
    if (PRODUCT_CONTROL_INVOKE(command, cmd_params, session_generation, request_id)) {
        cJSON_Delete(root);
        return;
    }
#endif
    if (strcmp(command, "device.health") == 0) {
        send_device_health(session_generation, request_id);
    } else if (strcmp(command, "device.guide") == 0) {
        send_device_guide(session_generation, request_id);
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

// A control-stream message cut across BodyChunks, held for its session until
// the rest arrives. Bytes of one message held at most: one longer is skipped
// whole (logged), so the stream stays in step.
static constexpr uint32_t kCtrlRxMaxHeld = 32 * 1024;
static struct {
    noise_ctrl_session_generation_t generation;
    uint8_t head[4];       // a length prefix cut short
    size_t head_len;
    uint8_t *body;         // a message cut short: body_have of body_len bytes
    uint32_t body_len, body_have;
    uint32_t skip;         // bytes left of a message too long to hold
} s_ctrl_rx;

// The session ended: free a message it left cut short.
static void drop_held_control_message(void) {
    free(s_ctrl_rx.body);
    s_ctrl_rx = {};
}

// Process one inbound RX body-chunk message from the server. The Noise layer
// gives us reassembled service frames; inside is a stream-1 BodyChunk whose
// data carries u32-LE prefixed JSON messages. The stream is bytes: a message
// may start in one BodyChunk and end in a later one (the Linux client buffers
// it the same way), so a partial one waits for the rest, for this session
// only.
static void process_inbound_body_chunk(
    ConstByteSpan data,
    noise_ctrl_session_generation_t session_generation) {
    auto &rx = s_ctrl_rx;
    if (rx.generation != session_generation) {
        drop_held_control_message();
        rx.generation = session_generation;
    }
    auto handle = [&](const uint8_t *p, uint32_t msg_len) {
        // Detect the link.register reply (id matches our request) so the
        // caller can open the tunnel stream only once the server has
        // recorded this connection's device registration.
#if CONFIG_MUSE_GADGET_BLE_SESSION
        bool register_reply = false;
        long long since_ms = s_register_sent_us ? (long long)((esp_timer_get_time() - s_register_sent_us) / 1000) : -1;
#endif
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
                        s_register_rejected = true;   /* answered: nothing waits for it any more */
                        ESP_LOGE(TAG, "link.register rejected: %s",
                                 cJSON_IsString(reason) && reason->valuestring
                                     ? reason->valuestring : "no reason given");
#if CONFIG_MUSE_GADGET_BLE_SESSION
                        char code[34];
                        ESP_LOGW(MG_SESSION_TAG, "link.register reply: rejected, code %s (%u bytes, %lld ms after it went)",
                                 log_name(cJSON_IsObject(failure) ? cJSON_GetObjectItem(failure, "code") : nullptr, code),
                                 (unsigned)msg_len, since_ms);
#endif
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
#if CONFIG_MUSE_GADGET_BLE_SESSION
                        char st[34];
                        ESP_LOGI(MG_SESSION_TAG, "link.register reply: %s, status %s (%u bytes, %lld ms after it went)",
                                 s_heartbeat_registered ? "registered" : "accepted, not registered",
                                 log_name(status, st), (unsigned)msg_len, since_ms);
#endif
                    }
#if CONFIG_MUSE_GADGET_BLE_SESSION
                    register_reply = true;
#endif
                }
                cJSON_Delete(msg);
            }
        }
#if CONFIG_MUSE_GADGET_BLE_SESSION
        // Handled: handle_invoke_request would only drop it, and log it as
        // unhandled.
        if (register_reply) return;
#endif
        handle_invoke_request(reinterpret_cast<const char *>(p), msg_len,
                              session_generation);
    };
    // A message of msg_len bytes whose first bytes come next: held, or
    // skipped if it can't be.
    auto start_partial = [&](uint32_t msg_len) {
        rx.body = msg_len <= kCtrlRxMaxHeld ? static_cast<uint8_t *>(malloc(msg_len)) : nullptr;
        if (!rx.body) {
            ESP_LOGW(TAG, "inbound control message of %u bytes too long to hold; skipping it",
                     (unsigned)msg_len);
            rx.skip = msg_len;
            return;
        }
        rx.body_len = msg_len;
        rx.body_have = 0;
    };
    const uint8_t *p = data.data();
    size_t remaining = data.size();

    while (remaining > 0) {
        if (rx.skip) {
            size_t n = remaining < rx.skip ? remaining : rx.skip;
            rx.skip -= static_cast<uint32_t>(n);
            p += n;
            remaining -= n;
            continue;
        }
        if (rx.body) {
            size_t n = rx.body_len - rx.body_have;
            if (n > remaining) n = remaining;
            memcpy(rx.body + rx.body_have, p, n);
            rx.body_have += static_cast<uint32_t>(n);
            p += n;
            remaining -= n;
            if (rx.body_have == rx.body_len) {
                handle(rx.body, rx.body_len);
                free(rx.body);
                rx.body = nullptr;
            }
            continue;
        }
        uint32_t msg_len;
        if (rx.head_len || remaining < 4) {
            size_t n = 4 - rx.head_len;
            if (n > remaining) n = remaining;
            memcpy(rx.head + rx.head_len, p, n);
            rx.head_len += n;
            p += n;
            remaining -= n;
            if (rx.head_len < 4) break;
            rx.head_len = 0;
            msg_len = static_cast<uint32_t>(rx.head[0])
                    | (static_cast<uint32_t>(rx.head[1]) << 8)
                    | (static_cast<uint32_t>(rx.head[2]) << 16)
                    | (static_cast<uint32_t>(rx.head[3]) << 24);
        } else {
            msg_len = static_cast<uint32_t>(p[0])
                    | (static_cast<uint32_t>(p[1]) << 8)
                    | (static_cast<uint32_t>(p[2]) << 16)
                    | (static_cast<uint32_t>(p[3]) << 24);
            p += 4;
            remaining -= 4;
        }
        if (msg_len > remaining) {
            start_partial(msg_len);
            continue;
        }
        if (msg_len > 0) handle(p, msg_len);
        p += msg_len;
        remaining -= msg_len;
    }
}

// One turn of the control stream's sender, the same over either transport.
// It keeps exactly one control message in flight across loop turns: a paused
// slot prevents dequeue/serialization of the next result. A stale message is
// dropped first. Unreachable today: the generations come from the same
// session local, so they cannot diverge. Kept deliberately — if generation
// ownership ever moves, its absence would send a stale result on a new
// session, silently and without a test to catch it. Then the heartbeat when
// it's due, the next result, and at most one BodyChunk. False if a send
// failed: the session is over.
template <typename Session>
static bool control_tx_step(esp_tls_t *tls, Session &session, pending_json_body &control_tx,
                            noise_ctrl_session_generation_t session_generation,
                            bool &heartbeat_sent, uint64_t &last_heartbeat_us,
                            uint8_t *svc_scratch, uint8_t *env_scratch, uint8_t *ws_buf,
                            bool *did, uint64_t *last_activity_us) {
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
#if CONFIG_HOMEHUB_ERROR_REPORT
    // Same gate as heartbeats: only a VM that confirmed this registration
    // gets the report, and it stays queued until a send completes. Over
    // either transport: a BLE-only gadget reports its crashes too.
    if (!control_tx.json && s_heartbeat_registered) {
        char *json = error_report_pending_json();
        if (json) {
            control_tx = {json, strlen(json), 0, false, session_generation};
            control_tx.is_error_report = true;
        }
    }
#endif
    pending_result pr;
    while (!control_tx.json && (!tx_needs_dma_reserve<Session>::value || noise_tx_has_dma_headroom(nullptr))
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
            return false;
        }
        if (!control_tx.is_heartbeat &&
            (control_tx.json_offset != old_offset
             || control_tx.prefix_sent != old_prefix_sent)) {
            *did = true;
            *last_activity_us = esp_timer_get_time();
        }
        if (status == json_send_status::Complete) {
            if (control_tx.is_heartbeat) {
                heartbeat_sent = true;
                last_heartbeat_us = esp_timer_get_time();
            }
#if CONFIG_HOMEHUB_ERROR_REPORT
            if (control_tx.is_error_report) error_report_mark_sent();
#endif
            ESP_LOGI(TAG, "sent %s (%u bytes)",
                     control_tx.is_registration ? "link.register" :
                     control_tx.is_heartbeat ? "link.heartbeat" :
                     control_tx.is_error_report ? "link.error_report" :
                     "control message",
                     (unsigned)control_tx.json_len);
            clear_pending_json_body(control_tx);
        }
    }
    return true;
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

// Link's control stream ends (end_body on its Response or a BodyChunk) only
// when the VM closes this device's control channel. The device is then no
// longer registered, and nothing else would notice: WebSocket pongs keep the
// connection looking alive. End the session so it reconnects and registers
// again.
static bool control_stream_ended(const DecodedServiceFrame &frame) {
    if (frame.stream_id != CTRL_STREAM_ID) return false;
    if (frame.kind == ServiceFrameKind::BodyChunk) return frame.body_chunk.end_body;
    if (frame.kind == ServiceFrameKind::Response) return frame.response.end_body;
    return false;
}

// One whole service frame from the VM, the same over either transport: the
// extra daemon requests, the identity reply, the tunnel's stream and the
// control stream. False if it ends the session (the control stream reset or
// refused), with *end_reason (when given) saying why for the connectivity
// history.
static bool dispatch_inbound_frame(const DecodedServiceFrame &frame, identity_fetch &identity,
                                   noise_ctrl_session_generation_t session_generation,
                                   const char **end_reason = nullptr) {
    bool is_tunnel = (frame.stream_id == TUNNEL_STREAM_ID);
    if (req_on_frame(frame)) {
        // One of the extra daemon requests.
    } else if (frame.stream_id == IDENTITY_STREAM_ID) {
        identity_on_frame(identity, frame);
    } else if (frame.kind == ServiceFrameKind::BodyChunk) {
        if (is_tunnel) {
            noise_tunnel_on_inbound(frame.body_chunk.data.data(),
                                    frame.body_chunk.data.size());
        } else {
            process_inbound_body_chunk(frame.body_chunk.data,
                                       session_generation);
            if (frame.stream_id == CTRL_STREAM_ID && frame.body_chunk.end_body) {
                // The VM ended the control stream: nothing more can come on it, so the session ends (over the
                // phone's relay too, where no socket closes to say so).
                ESP_LOGW(TAG, "server ended the control stream");
                return false;
            }
        }
    } else if (frame.kind == ServiceFrameKind::Reset) {
        if (is_tunnel) {
            // Tunnel stream reset is non-fatal for the control
            // session; drop the tunnel and keep control alive.
            // The reason is bytes off the wire: printable ASCII
            // only, capped. The code alone doesn't say which of
            // the server's ProtocolError cases this was.
            char reason[RESET_REASON_MAX + 1];
            copy_reset_reason(frame.reset.reason, reason, sizeof(reason));
            ESP_LOGW(TAG, "server reset tunnel stream: code=%d reason=\"%s\"",
                     (int)frame.reset.code, reason);
            char why[TUNNEL_FAILURE_MAX];
            snprintf(why, sizeof(why), "server reset stream, code=%d%s%s",
                     (int)frame.reset.code, reason[0] ? ": " : "", reason);
            noise_tunnel_on_server_drop(why);
        } else {
            ESP_LOGW(TAG, "server reset stream %lld",
                     (long long)frame.stream_id);
            if (end_reason) *end_reason = "server reset control stream";
            return false;
        }
    } else if (frame.kind == ServiceFrameKind::Response) {
        ESP_LOGI(TAG, "got response status=%d on stream %lld",
                 frame.response.status, (long long)frame.stream_id);
        if (frame.response.status == 200 && frame.stream_id == CTRL_STREAM_ID && frame.response.end_body) {
            // Its inline messages first (a final node.unpaired among them), as a BodyChunk's above.
            process_inbound_body_chunk(frame.response.body, session_generation);
            ESP_LOGW(TAG, "server ended the control stream in its response");   // as a BodyChunk's end_body above
            return false;
        }
        if (frame.response.status != 200) {
            if (is_tunnel) {
                ESP_LOGW(TAG, "server rejected " GADGET_TUNNEL_PATH ": %d",
                         frame.response.status);
                char why[TUNNEL_FAILURE_MAX];
                snprintf(why, sizeof(why), "server rejected open: %d",
                         (int)frame.response.status);
                noise_tunnel_on_server_drop(why);
            } else {
                ESP_LOGE(TAG, "server rejected the control stream: %d",
                         frame.response.status);
                if (end_reason) *end_reason = "server rejected the control stream";
                return false;
            }
        } else if (frame.stream_id == CTRL_STREAM_ID) {
            // A Response's inline body carries messages too.
            process_inbound_body_chunk(frame.response.body, session_generation);
        }
    }
    return true;
}

// Connectivity history: failed attempts between sessions are summarized in
// the next session_up rather than recorded one by one. noise_ctrl_task-owned.
static uint32_t s_failed_attempts = 0;
static const char *s_last_attempt_failure = "";
static uint64_t s_session_down_us = 0;

static void note_attempt_failed(const char *reason) {
    ++s_failed_attempts;
    s_last_attempt_failure = reason;
}

#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
// TLS session resumption for the VM WebSocket (CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS):
// a reconnect offers the last session's id (or RFC 5077 ticket), and an edge
// that kept it resumes, skipping the certificate chain and the ECDHE. The
// server was verified against the CA bundle when the session was made; a
// server that doesn't resume falls back to a full handshake, verified against
// the same bundle (crt_bundle_attach stays set). Kept per host: a session is
// never offered to another one. Session-task-owned.
struct tls_resume_t {
    esp_tls_client_session_t *session;
    char *host;   // the host it was made with (heap copy)
};

static tls_resume_t s_tls_resume;

static void tls_resume_forget(tls_resume_t *t) {
    if (t->session) esp_tls_free_client_session(t->session);
    free(t->host);
    t->session = nullptr;
    t->host = nullptr;
}

static void tls_resume_offer(tls_resume_t *t, const char *host, esp_tls_cfg_t *cfg) {
    if (t->session && (!t->host || strcmp(t->host, host) != 0)) tls_resume_forget(t);
    cfg->client_session = t->session;
}

// After a successful handshake: keep this connection's session for the next.
static void tls_resume_keep(tls_resume_t *t, const char *host, esp_tls_t *tls) {
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_tls_client_session_t *fresh = esp_tls_get_client_session(tls);
    char *copy = fresh ? strdup(host) : nullptr;
    if (!copy) {   // no memory: no resumption next time, nothing else changes
        if (fresh) esp_tls_free_client_session(fresh);
        tls_resume_forget(t);
        return;
    }
    tls_resume_forget(t);
    t->session = fresh;
    t->host = copy;
    ESP_LOGI(TAG, "TLS session kept for resumption: %d B of internal RAM",
             (int)before - (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

// After a failed connect. One that never reached TLS (DNS, the TCP connect or
// a proxy failed, as while Wi-Fi is down) is still in ESP_TLS_INIT and says
// nothing about the session: keep it, so the reconnect still resumes. Once TLS
// began (a handshake or verification error, or a server refusing the offered
// session), the session may be what failed: never retry with it.
static void tls_resume_failed(tls_resume_t *t, esp_tls_t *tls) {
    esp_tls_conn_state_t state = ESP_TLS_FAIL;
    if (esp_tls_get_conn_state(tls, &state) != ESP_OK || state != ESP_TLS_INIT) tls_resume_forget(t);
}
#endif

static session_result_t run_session(stack_monitor_t *stack) {
    session_result_t result = SESSION_FAILED;
    const char *end_reason = "stopped";
    bool went_silent = false;  // torn down because nothing came back from the VM
    uint64_t session_up_us = 0;
    uint8_t *ws_buf = nullptr, *rx_buf = nullptr;
    uint8_t *svc_scratch = nullptr, *env_scratch = nullptr;
    uint8_t *tf_scratch = nullptr, *sr_scratch = nullptr;
    noise_ctrl_session_generation_t session_generation = 0;
    pending_json_body control_tx;
    identity_fetch identity = {};

    // 1. TLS connect. One copy of the host for the whole handshake: setup can
    // rewrite s_noise_host from another task (noise_ctrl_set_host()), and a
    // kept session must be filed under the host it was made with.
    char *tls_host = strdup(s_noise_host);
    esp_tls_t *tls = tls_host ? esp_tls_init() : nullptr;
    if (!tls) {
        free(tls_host);
        ESP_LOGE(TAG, "esp_tls_init failed");
        note_attempt_failed("TLS init failed");
        return SESSION_FAILED;
    }
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // a fallback full handshake verifies as always
    cfg.timeout_ms = 15000;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
    tls_resume_offer(&s_tls_resume, tls_host, &cfg);
#endif
    int64_t tls_t0 = esp_timer_get_time();

    int r = network_proxy_tls_connect(tls_host, static_cast<int>(strlen(tls_host)),
                                  NOISE_PORT, &cfg, tls);
    if (r != 1) {
        ESP_LOGE(TAG, "TLS connect failed: %d", r);
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        tls_resume_failed(&s_tls_resume, tls);
#endif
        esp_tls_conn_destroy(tls);
        free(tls_host);
        note_attempt_failed("TLS connect failed");
        return SESSION_FAILED;
    }
    {
        bool offered = false;
#if CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS
        offered = cfg.client_session != nullptr;
        tls_resume_keep(&s_tls_resume, tls_host, tls);
#endif
        ESP_LOGI(TAG, "TLS connected to %s:%d (vm_id=%s) in %lld ms%s", tls_host, NOISE_PORT, s_vm_id,
                 (long long)((esp_timer_get_time() - tls_t0) / 1000), offered ? ", last session offered" : "");
        free(tls_host);
    }
    // Send each write at once. With Nagle's algorithm on, a small write (a
    // tunnel batch holding a lone handshake packet or a short reply) waited
    // until everything sent before it was acknowledged, adding ~100 ms to
    // every exchange through the tunnel that sent two small writes in a row,
    // such as each new connection's handshake. Every write here is a whole
    // TLS record, so this adds no tiny segments.
    {
        int fd = -1, one = 1;
        if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0
            || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            ESP_LOGW(TAG, "TCP_NODELAY not set on the Muse connection");
        }
    }

    // 2. WebSocket upgrade
    noise_upgrade_result_t upgrade = ws_upgrade(tls);
    if (upgrade != NOISE_UPGRADE_OK) {
        esp_tls_conn_destroy(tls);
#if CONFIG_HOMEHUB_FAST_GCM
        fast_gcm_forget_all();
#endif
        // The edge refusing our bearer is an auth failure, not a network
        // blip. Saying so takes the 60s auth backoff instead of the 15s
        // network cap, and raises ws_auth_failed so app.c re-fetches the
        // token from fetch_vms — the only thing that can actually fix it.
        note_attempt_failed(upgrade == NOISE_UPGRADE_AUTH_REJECTED
                                ? "WebSocket upgrade auth rejected"
                                : "WebSocket upgrade failed");
        return upgrade == NOISE_UPGRADE_AUTH_REJECTED ? SESSION_AUTH_FAILED
                                                     : SESSION_FAILED;
    }
    ESP_LOGI(TAG, "WebSocket upgraded to %s", NOISE_PATH);

    // Allocate session-scoped buffers
    PsaCryptoBackend crypto;
    ClientSession session(crypto);

    // WS_HEADROOM in front of the payload for its frame header; the buffer is
    // cache-line aligned so the payload Noise seals in place is too.
    size_t ws_buf_size = WS_HEADROOM + WS_BUF_SIZE;
    ws_buf = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        WS_HEADROOM, ws_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ws_buf) ws_buf = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        WS_HEADROOM, ws_buf_size, MALLOC_CAP_8BIT));

    rx_buf = static_cast<uint8_t *>(
        heap_caps_malloc(WS_RX_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rx_buf) rx_buf = static_cast<uint8_t *>(malloc(WS_RX_BUF_SIZE));

    // Crypto/framing scratch. PSRAM-preferred (with a heap fallback) so the
    // buffers don't land in internal SRAM — anything under
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
        end_reason = "session buffer alloc failed";
        goto cleanup;
    }

    {  // Scoped block for event loop — avoids goto crossing variable init.

    // 3. Noise handshake
    if (!noise_handshake(tls, session, ws_buf, ws_buf_size)) {
        ESP_LOGE(TAG, "Noise handshake failed");
        result = SESSION_AUTH_FAILED;
        end_reason = "Noise handshake failed";
        goto cleanup;
    }
    session_generation = noise_ctrl_next_session_generation(
        s_session_generation_counter);
    s_session_generation_counter = session_generation;

    // 4. Open POST /api/gadget/control stream
    {
        ApplicationRequestView req;
        req.verb = StringView("POST");
        req.path = StringView(GADGET_CONTROL_PATH);
        req.end_body = false;

        auto open_sws = session.StartOutboundApplicationRequest(
            GADGET_SERVICE, CTRL_STREAM_ID, req,
            ByteSpan(svc_scratch, OUT_SVC_SCRATCH),
            ByteSpan(env_scratch, OUT_ENV_SCRATCH));
        if (!open_sws.ok()) {
            ESP_LOGE(TAG, "StartOutboundApplicationRequest failed: %s",
                     open_sws.status().str());
            end_reason = "control stream open failed";
            goto cleanup;
        }
        if (!flush_outbound(tls, session, ws_buf)) {
            end_reason = "control stream open failed";
            goto cleanup;
        }
        ESP_LOGI(TAG, "opened POST /api/gadget/control stream");
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
        s_register_rejected = false;
        s_heartbeat_registered = false;
        char *reg_json = build_register_json();
        if (!reg_json) {
            ESP_LOGE(TAG, "could not build link.register: int=%u/%u dma=%u/%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            end_reason = "link.register build failed";
            goto cleanup;
        }
#if CONFIG_MUSE_WASM_APPS
        // Apps add to it, and it must stay under 8 KB.
        ESP_LOGI(TAG, "link.register: %u bytes", (unsigned)strlen(reg_json));
#endif
        control_tx = {reg_json, strlen(reg_json), 0, false, session_generation};
        control_tx.is_registration = true;
    }

    // Switch to non-blocking for the event loop
    {
        int sockfd = -1;
        if (esp_tls_get_conn_sockfd(tls, &sockfd) != ESP_OK || sockfd < 0) {
            ESP_LOGE(TAG, "get_conn_sockfd failed");
            end_reason = "socket setup failed";
            goto cleanup;
        }
        int flags = lwip_fcntl(sockfd, F_GETFL, 0);
        lwip_fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    s_connected = true;
    notify_status("ws_connected");
    session_up_us = esp_timer_get_time();
    if (s_failed_attempts) {
        link_events_record(LINK_EVENT_SESSION_UP,
                           "established after %us: %u failed attempts, last: %s",
                           (unsigned)((session_up_us - s_session_down_us) / 1000000),
                           (unsigned)s_failed_attempts, s_last_attempt_failure);
    } else {
        link_events_record(LINK_EVENT_SESSION_UP, "established");
    }
    s_failed_attempts = 0;

    HeaderView hdr_storage[16];
    bool error = false;
    uint64_t last_activity_us = esp_timer_get_time();
    // last_rx_us tracks inbound liveness (app frames + keepalive pongs),
    // distinct from last_activity_us which also counts our TX.
    uint64_t last_rx_us = last_activity_us;
    uint64_t last_ws_ping_us = 0;
    bool heartbeat_sent = false;
    uint64_t last_heartbeat_us = 0;
    int64_t dma_held_since_us = 0, dma_held_warned_us = 0;

    // Tunnel data-plane stream, multiplexed on this session. Deferred: it is
    // opened from inside the loop only after the link.register reply arrives
    // (s_register_acked), because the server authorizes /api/gadget/tunnel by checking
    // this connection has a registered device. Opening before the ack races the
    // registration and takes a 403 + slow reopen.
    tunnel_emit_ctx tun_ctx = {tls, &session, svc_scratch, env_scratch, ws_buf};
    noise_tunnel_emit_t tun_emit = {&tun_ctx, tunnel_open_stream, tunnel_send_body,
                                    tunnel_send_body_inplace, tunnel_reset_stream};
    bool tunnel_opened = false;
    uint64_t last_busy_us = esp_timer_get_time();

    while (!error && s_running) {
        muse_wdt_feed(MUSE_WDT_LINK_SESSION);   // the session loop is watched (each wait in it is bounded)
        bool did = false;

        // Ask for the agent's name (once) after the register ack, so the
        // request never races registration traffic.
        if (SHOW_AGENT_NAME && !identity.requested && s_register_acked) {
            identity.requested = true;
            if (!identity_request(tls, session, svc_scratch, env_scratch, ws_buf)) {
                end_reason = "identity request failed";
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

        // RX: read the waiting inbound WS frames (non-blocking), up to
        // SESSION_RX_FRAMES_PER_TURN frames or SESSION_RX_BYTES_PER_TURN bytes.
        size_t rx_bytes = 0;
        bool rx_drain = true;  // cleared by a control-stream frame
        for (int rx_frame = 0; rx_drain && rx_frame < SESSION_RX_FRAMES_PER_TURN
                               && (rx_frame == 0 || rx_bytes < SESSION_RX_BYTES_PER_TURN);
             rx_frame++) {
            ssize_t n = ws_recv_frame_nonblock(tls, rx_buf, WS_RX_BUF_SIZE);
            if (n > 0) {
                rx_bytes += static_cast<size_t>(n);
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
                    end_reason = "inbound frame rejected";
                    error = true;
                    break;
                }
                if (inbound.frame_status == InboundFrameStatus::Complete) {
                    if (inbound.frame.stream_id != TUNNEL_STREAM_ID) rx_drain = false;
                    if (!dispatch_inbound_frame(inbound.frame, identity, session_generation, &end_reason)) {
                        error = true;
                        break;
                    }
                    if (control_stream_ended(inbound.frame)) {
                        ESP_LOGW(TAG, "server ended " GADGET_CONTROL_PATH);
                        end_reason = "server ended control stream";
                        error = true;
                        break;
                    }
                }
            } else if (n == 0) {
                ESP_LOGI(TAG, "peer closed");
                end_reason = "server closed connection";
                error = true;
                break;
            } else if (n == -1) {
                ESP_LOGW(TAG, "rx error");
                end_reason = "socket read error";
                error = true;
                break;
            } else if (n == -3) {
                // Control frame (ping/pong) — no app data, but the peer answered,
                // so the connection is bidirectionally alive. Reset RX liveness.
                last_rx_us = esp_timer_get_time();
                last_activity_us = last_rx_us;
            } else {
                break;  // n == -2: nothing more waiting
            }
        }
        if (error) break;

        // TX: keep exactly one control message in flight across loop turns
        // (control_tx_step, which the device session over BLE runs too).
        if (!control_tx_step(tls, session, control_tx, session_generation,
                             heartbeat_sent, last_heartbeat_us,
                             svc_scratch, env_scratch, ws_buf,
                             &did, &last_activity_us)) {
            end_reason = "control send failed";
            error = true;
            break;
        }
        // A control message Link holds back while DMA-capable RAM is under its
        // reserve (the registration, a command's result) otherwise waits
        // silently, and a held registration leaves the device offline in
        // Muse: say so every 5 s while it does. The heap is only sampled
        // while something waits.
        {
            size_t dma_free = 0;
            bool held = (control_tx.json || uxQueueMessagesWaiting(s_result_q))
                        && (!noise_tx_has_dma_headroom(&dma_free)
                            || !noise_tx_has_contiguous_dma_headroom());
            int64_t now = esp_timer_get_time();
            if (!held) {
                dma_held_since_us = 0;
            } else if (!dma_held_since_us) {
                dma_held_since_us = now;
                dma_held_warned_us = now;
            } else if (now - dma_held_warned_us >= 5000000) {
                dma_held_warned_us = now;
                ESP_LOGW(TAG, "%s held for %d s: DMA-capable RAM is short (%u bytes free; sending needs %u)",
                         !control_tx.json ? "a command result"
                         : control_tx.is_registration ? "link.register"
                         : control_tx.is_heartbeat ? "link.heartbeat" : "a control message",
                         (int)((now - dma_held_since_us) / 1000000), (unsigned)dma_free,
                         (unsigned)NOISE_TX_DMA_RESERVE_BYTES);
            }
        }

#if CONFIG_HOMEHUB_DAEMON_REQUESTS
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
        if (tunnel_opened && !noise_tunnel_maybe_reopen(&tun_emit)) {
            end_reason = "tunnel reopen failed";
            error = true;
            break;
        }

        // TX: drain the tunnel batch queue (multiplexed on this session).
        int tun_sent = noise_tunnel_pump_tx(&tun_emit);
        if (tun_sent > 0) {
            did = true;
            last_activity_us = esp_timer_get_time();
        } else if (tun_sent < 0) {
            end_reason = "tunnel TX failed or stalled";
            error = true;
            break;
        }

        // Tunnel keepalive: ping the VM backend when the tunnel is idle and drop
        // it if pongs stop, so a half-open tunnel (WS alive, backend gone) is
        // detected and reopened by maybe_reopen above. Cheap and rate-limited
        // internally, so it's safe to call every loop iteration.
        if (tunnel_opened) {
            noise_tunnel_tick(&tun_emit);
            // If nothing at all has arrived since the oldest unanswered tunnel
            // ping, not even the pong to the WebSocket ping sent after it, the
            // connection itself is dead (typically a WAN drop leaving TCP
            // half-open). Reopening the tunnel on it can't work, so reconnect
            // now instead of waiting out RX_IDLE_TIMEOUT_MS.
            const uint64_t unanswered_ping_us = noise_tunnel_take_unanswered_timeout();
            if (unanswered_ping_us && last_rx_us < unanswered_ping_us) {
                ESP_LOGW(TAG, "tunnel keepalive unanswered and nothing received since; reconnecting");
                end_reason = "no response from VM (tunnel keepalive)";
                went_silent = true;
                error = true;
                break;
            }
        }

        // A seal failure permanently poisons Noise, including failures from
        // tunnel open/reopen or the void keepalive hook. Check once per turn:
        // WS pings can still succeed and otherwise keep a dead session alive.
        if (!session.isEstablished()) {
            ESP_LOGW(TAG, "Noise session no longer established; reconnecting");
            end_reason = "Noise session broke";
            error = true;
            break;
        }

        // Keepalive: a WebSocket ping. The peer answers with a pong, so both
        // directions see traffic and middlebox idle timers (which drop the
        // idle server→device path otherwise) stay reset. This keeps the whole
        // WebSocket transport alive; Noise session health is checked above.
        // Ping when we've sent nothing for the interval, or when nothing has
        // come back for it even while we keep sending (pong proves the path).
        {
            const uint64_t now_us = esp_timer_get_time();
            const uint64_t interval_us = static_cast<uint64_t>(KEEPALIVE_INTERVAL_MS) * 1000;
            const uint64_t rx_idle_us = now_us - last_rx_us;
            if (rx_idle_us >= static_cast<uint64_t>(RX_IDLE_TIMEOUT_MS) * 1000) {
                ESP_LOGW(TAG, "nothing received for %llus; reconnecting",
                         (unsigned long long)(rx_idle_us / 1000000));
                end_reason = "no response from VM";
                went_silent = true;
                error = true;
                break;
            }
            bool tx_idle = !did && now_us - last_activity_us >= interval_us;
            bool rx_idle = rx_idle_us >= interval_us && now_us - last_ws_ping_us >= interval_us;
            // A tunnel ping unanswered with nothing received since: ping once
            // more, so a live connection answers before the tunnel keepalive
            // timeout above decides between a tunnel reopen and a reconnect.
            const uint64_t tun_ping_us = tunnel_opened ? noise_tunnel_unanswered_ping_us() : 0;
            bool tun_probe = tun_ping_us && now_us - tun_ping_us >= TUN_PING_PROBE_US
                             && last_rx_us < tun_ping_us && last_ws_ping_us < tun_ping_us;
            if (tx_idle || rx_idle || tun_probe) {
                if (!ws_send_ping(tls)) {
                    ESP_LOGW(TAG, "keepalive ping failed");
                    end_reason = "WebSocket ping failed";
                    error = true;
                    break;
                }
                ESP_LOGD(TAG, "keepalive ping sent");
                last_activity_us = last_ws_ping_us = now_us;
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
    s_session_down_us = esp_timer_get_time();
    link_events_record(LINK_EVENT_SESSION_DOWN, "%s after %us up, rx idle %us",
                       end_reason,
                       (unsigned)((s_session_down_us - session_up_us) / 1000000),
                       (unsigned)((s_session_down_us - last_rx_us) / 1000000));
    // Silence alone can't say which hop failed; check the internet while the
    // reconnect runs.
    if (went_silent) link_probe_on_stall();
    notify_status("ws_disconnected");
    // Clean exit (s_running went false) — no backoff needed.
    if (!error) result = SESSION_OK;
    }  // end event loop scope

cleanup:
    clear_pending_json_body(control_tx);
    drop_held_control_message();
    free(ws_buf);
    free(identity.body);
    free(rx_buf);
    free(svc_scratch);
    free(env_scratch);
    free(tf_scratch);
    free(sr_scratch);
    esp_tls_conn_destroy(tls);
#if CONFIG_HOMEHUB_FAST_GCM
    fast_gcm_forget_all();   // this session's TLS and Noise keys
#endif
    if (!session_up_us) note_attempt_failed(end_reason);
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
        // Connect, handshake and the reconnect wait aren't watched: each has
        // its own timeout (muse_wdt.h); the session loop is.
        muse_wdt_remove(MUSE_WDT_LINK_SESSION);
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
    MUSE_TASK_END();
}

// ---- Device session over BLE (mg_command_session) ---------------------------
//
// A BLE-only gadget reaches its VM the way a Wi-Fi gadget does, with the
// phone as its network (protocols/mgcommands.h, Device session): the phone
// runs TLS, the WebSocket and Noise as this gadget's node and carries that
// session's messages over BLE. What goes over it comes from the code
// run_session() runs: link.register on the control stream (POST /link-control
// on the daemon here, the route the phone's relay carries), the heartbeat,
// link.invoke and link.result (control_tx_step, dispatch_inbound_frame), and
// the extra daemon requests (req_pump_tx). Only the transport differs: each
// ServiceFrame goes out as one ServiceRequestEnvelope (mg_ble_session_send()),
// and each ServiceResponseEnvelope from the session is decoded and dispatched
// as run_session() dispatches a frame Noise opened. There's no tunnel, and no
// GET /identity (not among the gadget role's routes yet).
#if CONFIG_MUSE_GADGET_BLE_SESSION

#define BLE_SESSION_STACK_FULL 8192
#define BLE_SESSION_STACK BLE_SESSION_STACK_FULL
// What a BodyChunk frame and its envelope add to a control chunk's data.
#define BLE_CHUNK_OVERHEAD 32
// Messages from the VM are taken only while the result queue (8 deep) has
// fewer than this many waiting: each link.invoke queues a result, and this
// task is the queue's only reader, so a burst taken whole would block it
// and drop the results past the eighth. What it leaves stays in the device
// session, its credit not yet given back, so the client waits.
#define BLE_RX_RESULTS_MAX 4

// Stands in for ClientSession in the shared senders (send_json_body_chunk,
// control_tx_step, req_open, req_pump_tx): StartOutbound* encode the
// ServiceFrame and its ServiceRequestEnvelope as ClientSession does, and
// flush_outbound() hands the envelope to the device session where
// ClientSession would seal it for the WebSocket.
class BleSessionOut {
 public:
    // max_envelope: the client's max_message; session: the device session's
    // id (mg_ble_session_id()), so nothing goes into a later one.
    void Reset(size_t max_envelope, uint32_t session) {
        max_envelope_ = max_envelope;
        session_ = session;
        pending_size_ = 0;
        last_sent_us_ = esp_timer_get_time();
    }
    StatusWithSize StartOutboundApplicationRequest(
        ServiceType service, int64_t stream_id, ApplicationRequestView request,
        ByteSpan frame_scratch, ByteSpan envelope_scratch) noexcept {
        return Start(service,
                     ServiceFrameView{stream_id, ServiceFrameKind::Request, request, {}, {}, {}},
                     frame_scratch, envelope_scratch);
    }
    StatusWithSize StartOutboundBodyChunk(
        ServiceType service, int64_t stream_id, BodyChunkView chunk,
        ByteSpan frame_scratch, ByteSpan envelope_scratch) noexcept {
        return Start(service,
                     ServiceFrameView{stream_id, ServiceFrameKind::BodyChunk, {}, {}, chunk, {}},
                     frame_scratch, envelope_scratch);
    }
    StatusWithSize StartOutboundReset(
        ServiceType service, int64_t stream_id, ResetView reset,
        ByteSpan frame_scratch, ByteSpan envelope_scratch) noexcept {
        return Start(service,
                     ServiceFrameView{stream_id, ServiceFrameKind::Reset, {}, {}, {}, reset},
                     frame_scratch, envelope_scratch);
    }
    // Queues the envelope a Start left: every message this session sends
    // (POST /link-control, link.register, the heartbeat and session.ping,
    // results, request operations) goes out here, through
    // mg_ble_session_send(), which reaches mg_session_send(), where a sealed
    // session's data is sealed (mgcommands.h, Sealed data). False if the
    // device session didn't take it (it ended, or has no room): the session
    // is over.
    bool Flush() {
        if (!pending_size_) return true;
        bool ok = mg_ble_session_send(session_, pending_, pending_size_);
        if (ok) last_sent_us_ = esp_timer_get_time();
        if (!ok) {
            ESP_LOGW(MG_SESSION_TAG, "a message of %u bytes didn't go (room %u, the client takes %u): the session ends",
                     (unsigned)pending_size_, (unsigned)mg_ble_session_room(), (unsigned)mg_ble_session_client_max());
        }
        pending_size_ = 0;
        return ok;
    }
    size_t max_envelope() const { return max_envelope_; }
    // When the last message went (the keepalive's clock).
    int64_t last_sent_us() const { return last_sent_us_; }

 private:
    StatusWithSize Start(ServiceType service, ServiceFrameView frame,
                         ByteSpan frame_scratch, ByteSpan envelope_scratch) noexcept {
        if (pending_size_) return StatusWithSize(Status::FailedPrecondition(), 0);
        StatusWithSize encoded = EncodeServiceFrame(frame, frame_scratch);
        if (!encoded.ok()) return encoded;
        StatusWithSize envelope = EncodeServiceRequestEnvelope(
            service, frame_scratch.subspan(0, encoded.size()), envelope_scratch);
        if (!envelope.ok()) return envelope;
        // Larger than the client takes: refused here, so a request body that
        // can't go ends its request, not the session.
        if (envelope.size() > max_envelope_) return StatusWithSize(Status::ResourceExhausted(), 0);
        pending_ = envelope_scratch.data();
        pending_size_ = envelope.size();
        return envelope;
    }
    size_t max_envelope_ = 0;
    uint32_t session_ = 0;
    const uint8_t *pending_ = nullptr;
    size_t pending_size_ = 0;
    int64_t last_sent_us_ = 0;
};

template <>
struct tx_needs_dma_reserve<BleSessionOut> {
    static constexpr bool value = false;
};

template <>
struct control_service<BleSessionOut> {
    static constexpr ServiceType value = ServiceType::Daemon;
};

static bool flush_outbound(esp_tls_t *, BleSessionOut &out, uint8_t *) {
    return out.Flush();
}

static size_t body_chunk_max(const BleSessionOut &out) {
    size_t max = out.max_envelope() > BLE_CHUNK_OVERHEAD ? out.max_envelope() - BLE_CHUNK_OVERHEAD : 0;
    return max < CTRL_BODY_CHUNK_MAX ? max : CTRL_BODY_CHUNK_MAX;
}

// One VM session over BLE, from the client's open to the state after it.
// Owned by the BLE session task.
struct ble_link {
    bool active;
    uint32_t id;               // the device session it runs or last ran (mg_ble_session_id())
    int64_t began_us;
    BleSessionOut out;
    noise_ctrl_session_generation_t session_generation;
    uint32_t pairing_gen;      // s_ble_pairing_gen when it began
    pending_json_body control_tx;
    identity_fetch identity;   // never requested
    bool heartbeat_sent;
    uint64_t last_heartbeat_us;
    uint64_t last_activity_us;
    uint8_t *svc_scratch, *env_scratch;
    uint8_t *chunk_scratch;    // send_json_body_chunk's first chunk (ws_buf over Wi-Fi)
    uint8_t *rx;               // one message from the VM
    size_t rx_cap;
    // session.ping: asked for (waiting for the control slot), and in the slot.
    bool ping_pending;
    bool ping_in_slot;
};

// The client's state reports (mg_ble's session_state, on the BLE host or
// worker task): the latest state, and how many there have been.
static std::atomic<uint8_t> s_ble_state{mg_session_state_closed};
static std::atomic<uint32_t> s_ble_seq{0};
// A failure ended the session: once the client has closed it and the backoff
// has passed, ask it to open a new one (run_session()'s reconnect over Wi-Fi,
// with its policy, so a VM that keeps refusing isn't asked again at once).
// Task-owned.
static bool s_ble_reopen;          // asked the client to close after a failure
static bool s_ble_reopen_closed;   // ... and it has
static int64_t s_ble_reopen_at;    // not before this (esp_timer_get_time())
static uint32_t s_ble_backoff_ms = RECONNECT_MIN_MS;
static uint8_t s_ble_short_failures;
// The client reported a session state the gadget didn't ask for (no_network,
// or closed) while none was open: ask for one again with open_request [1] at
// once, then with backoff, only while the link stays connected. A failed open
// counts the same (mg-session-v1 exchange 14: connecting, then closed or
// no_network with no session ever open; mg_session.c reports every state):
// the open_request that went is followed by that kind's next step, so the
// gadget is never left with nothing armed. Two series,
// neither restarting the other: after closed (the server's end or refusal) 1, 2, 4 ... s capped
// at 60 s; after no_network (the phone's network, or an OS that killed the
// app's socket) 1, 2, 4 ... s capped at 15 s. A series starts over only
// after a session that stayed open 60 s (BLE_ASK_STABLE_MS: after a shorter
// one the same kind of end continues it, so a server that keeps ending at
// once is never asked faster); both start over at a hello answered (which drops an ask still
// waiting), at a credentials_update saved, which asks at once if the
// session is closed (since the hello the last state ended one: new
// credentials may fix what ended it, even auth_failed; that ask is the first
// of the ended kind's series; mg_ble_session_reask_marks(), as
// mg_session_ref.py and the Zephyr side), and at the link's end (a gadget
// that registered commands must stay reachable, and the client never reopens
// on its own; iOS suspends the Muse app in the background and ends its
// sockets at each Bluetooth wake, so the relay reports closed every ~30 s on
// the Watcher; a phone can answer no_network while the handed-off
// token refreshes, and never try again). Never after
// auth_failed, not_enrolled or too_large: those need something else than a
// retry, and one cancels an ask still waiting. Task-owned.
#define BLE_ASK_FIRST_MS 1000
#define BLE_ASK_STABLE_MS 60000   // a session open this long restarts the series at its end
enum { BLE_ASK_CLOSED, BLE_ASK_NO_NETWORK, BLE_ASK_KINDS };
static const uint32_t BLE_ASK_MAX_MS[BLE_ASK_KINDS] = {60000, 15000};
static uint32_t s_ble_ask_ms[BLE_ASK_KINDS];   // each series' last delay
static uint8_t s_ble_asks[BLE_ASK_KINDS];      // asks in each series: the first goes at once
static int64_t s_ble_ask_sent_us[BLE_ASK_KINDS];   // when each series' last ask went
static int64_t s_ble_opened_us;      // when the client's open session opened (0: none open)
static uint32_t s_ble_ask_hellos, s_ble_ask_saved;  // mg_ble_session_reask_marks() as last seen
// ...and as they were when the last state was reported (noise_ctrl_ble_state(),
// under the protocol lock), so a pass orders them around that state.
static std::atomic<uint32_t> s_ble_state_hellos{0}, s_ble_state_saved{0};
// The last state ended a session (closed, no_network, not_enrolled,
// auth_failed, too_large; not open or connecting) and no hello came since:
// a credentials_update saved now asks at once (mgcommands.h).
static bool s_ble_ended;
static int s_ble_ended_kind;   // ...and the series that end continues (closed's unless no_network)
static bool s_ble_asking;   // the reopen waiting is such an ask (not a failure's)
// ...set when the last ask went, the client silent since: counted only when
// it goes; a state the client reports replaces it.
static bool s_ble_silent;
static uint8_t s_ble_ask_kinds;   // ...for these series (1 << BLE_ASK_CLOSED, BLE_ASK_NO_NETWORK)
// The last session ended with a state the gadget didn't ask for (closed or
// no_network: iOS ending the relay at a wake): its invoke results go in the next.
static bool s_ble_carry;
// Bumped by noise_ctrl_ble_pairing_changed() (any task); the task's pass drops
// what came before it, and a session carries nothing across it.
static std::atomic<uint32_t> s_ble_pairing_gen{0};
static uint32_t s_ble_pairing_seen;
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
// Requests made while the session isn't ready (open, its link.register
// acknowledged) wait in s_req_q, in order, for NOISE_CTRL_REQ_WAIT_OPEN_MS at
// most (mgcommands.h; iOS ends the relay's sockets at each
// Bluetooth wake, and a picture test pressed in that gap failed with "press
// again"); the first asks the client for a session at once.
static std::atomic<bool> s_req_ready{false};       // the session is open and link.register acknowledged
static std::atomic<int64_t> s_req_wait_since{0};   // a request waits since (0: none)
static std::atomic<int> s_req_wait_opens{0};       // ...this many (at most REQ_MAX_STREAMS)
#endif
// noise_ctrl_ble_request_open(): ask now (the picture test's press with no
// session), from any task.
static std::atomic<bool> s_ble_ask_now{false};
static TaskHandle_t s_ble_task = nullptr;
// session.ping from the console or the Nordic UART (noise_ctrl_ble_ping()):
// the task takes it on its next pass.
static std::atomic<bool> s_ble_ping{false};
// The active task (noise_control.h): each kind's hold, until this
// time (esp_timer_get_time(); 0 none), any task. s_ble_task_on: the last
// pass's verdict (task-owned), so the start and the end are logged and acted
// on once.
static std::atomic<int64_t> s_task_until[NOISE_TASK_KINDS];
static bool s_ble_task_on;
// The gadget's own close of an open session with no task: due
// at this time (0: none armed), and whether this session already got it (a
// close goes once; the client's closed state follows).
static int64_t s_ble_idle_close_at;
static bool s_ble_idle_closed;
static const char *const TASK_NAMES[NOISE_TASK_KINDS] = {"user action", "face", "picture test"};

// What session.ping sends: the heartbeat a registered gadget already sends
// on its own (noise_ctrl_heartbeat_due()), byte for byte. The VM takes it
// from any registered gadget node and only marks the node alive: no id, no
// reply to wait for, nothing stored or run on either side.
#define BLE_PING_JSON "{\"method\":\"link.heartbeat\"}"

static uint8_t *ble_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return static_cast<uint8_t *>(p ? p : malloc(n));
}

#if CONFIG_SPIRAM
// cJSON's allocations on this task (link.register, link.invoke, results)
// PSRAM first, as ble_alloc: hundreds of small nodes that the malloc
// threshold would otherwise put in internal RAM. free() takes either heap.
// Only boards with PSRAM install it (noise_ctrl_ble_start()).
static void *ble_json_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
#endif

// The internal heap on mg.session at the session's milestones (open,
// link.register gone, end, a send that failed), so a capture shows where it
// went: free, largest block, lowest ever.
static void ble_heap_note(const char *when) {
    ESP_LOGI(MG_SESSION_TAG, "internal heap %s: %uK free, %uK largest, %uK lowest", when,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void ble_session_end(ble_link &link, bool too_large) {
    if (!link.active) return;
    link.active = false;
    s_connected = false;
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    // As over Wi-Fi: requests end with NOISE_CTRL_REQ_TOO_LARGE when a frame
    // too large for this gadget ended the session (the client's too_large).
    s_req_too_large = too_large;
    if (!s_req_ready.exchange(false)) {
        // Never ready (link.register not acknowledged): nothing went yet, so
        // what's queued keeps waiting for the next session, its 10 s counted
        // from here if it wasn't already.
        req_streams_end();
        int64_t none = 0;
        if (uxQueueMessagesWaiting(s_req_q)) s_req_wait_since.compare_exchange_strong(none, esp_timer_get_time());
    } else if (s_ble_carry && !too_large && mg_ble_connected()) {
        // Ended unasked (closed, no_network: iOS ends the relay at a wake) on
        // a link that's still up: a request in flight with no reply yet goes
        // again once, after the reopen; the rest of the queue waits for it
        // too. The link's loss fails them as before.
        int64_t oldest = 0;
        int requeued = req_streams_requeue(&oldest);
        req_streams_end();
        s_req_wait_opens += requeued;
        if (uxQueueMessagesWaiting(s_req_q)) {
            int64_t now = esp_timer_get_time();
            int64_t since = oldest ? oldest : now;
            int64_t cur = s_req_wait_since.load();
            if (!cur || since < cur) s_req_wait_since = since;
        }
    } else {
        req_end_all();
    }
#else
    (void)too_large;
    req_end_all();
#endif
    clear_pending_json_body(link.control_tx);
    free(link.identity.body);
    link.identity = {};
    free(link.svc_scratch);
    free(link.env_scratch);
    free(link.chunk_scratch);
    free(link.rx);
    link.svc_scratch = link.env_scratch = link.chunk_scratch = link.rx = nullptr;
    link.ping_pending = link.ping_in_slot = false;
    s_register_sent_us = 0;
    ESP_LOGI(MG_SESSION_TAG, "device session ended");
    ble_heap_note("at the end");
}

// What run_session() does once Noise is up, on the daemon's POST
// /link-control (run_session() opens /api/gadget/control on the Gadget API),
// then link.register in the control slot, in device session id. False if it
// couldn't start.
static bool ble_session_begin(ble_link &link, uint32_t id) {
    link.id = id;
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    s_req_replay_on = true;   // requests keep what they sent until a reply (req_streams_requeue)
#endif
    link.began_us = esp_timer_get_time();
    link.pairing_gen = s_ble_pairing_gen.load();
    s_ble_idle_close_at = 0;
    s_ble_idle_closed = false;   // a new session may be closed once
    link.rx_cap = mg_ble_session_max_message();
    link.svc_scratch = ble_alloc(OUT_SVC_SCRATCH);
    link.env_scratch = ble_alloc(OUT_ENV_SCRATCH);
    link.chunk_scratch = ble_alloc(CTRL_BODY_CHUNK_MAX);
    link.rx = link.rx_cap ? ble_alloc(link.rx_cap) : nullptr;
    link.out.Reset(mg_ble_session_client_max(), id);
    link.control_tx = {};
    link.identity = {};
    link.ping_pending = link.ping_in_slot = false;
    s_register_sent_us = 0;
    link.heartbeat_sent = false;
    link.last_heartbeat_us = 0;
    link.last_activity_us = esp_timer_get_time();
    noise_ctrl_session_generation_t prev_generation = s_session_generation_counter;
    link.session_generation = noise_ctrl_next_session_generation(s_session_generation_counter);
    s_session_generation_counter = link.session_generation;
    link.active = true;
    if (!link.svc_scratch || !link.env_scratch || !link.chunk_scratch || !link.rx) {
        ESP_LOGE(MG_SESSION_TAG, "device session: buffer alloc failed");
        return false;
    }

    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView("/link-control");
    req.end_body = false;
    auto open_sws = link.out.StartOutboundApplicationRequest(
        ServiceType::Daemon, CTRL_STREAM_ID, req,
        ByteSpan(link.svc_scratch, OUT_SVC_SCRATCH),
        ByteSpan(link.env_scratch, OUT_ENV_SCRATCH));
    if (!open_sws.ok() || !flush_outbound(nullptr, link.out, nullptr)) {
        ESP_LOGE(MG_SESSION_TAG, "device session: POST /link-control failed");
        return false;
    }

    // A result for the session that just ended with a state the gadget didn't
    // ask for (closed, no_network: iOS ends it at each wake), queued within
    // the last 10 s, goes in this one (mgcommands.h); any other, and
    // after a failure, too_large or auth_failed, is dropped.
    // Requests waiting for a session stay queued, in order.
    pending_result pr;
    int kept = 0;
    for (int n = (int)uxQueueMessagesWaiting(s_result_q); n > 0; n--) {
        if (xQueueReceive(s_result_q, &pr, 0) != pdTRUE) break;
        if (s_ble_carry && pr.session_generation == prev_generation
            && esp_timer_get_time() - pr.queued_us <= (int64_t)NOISE_CTRL_REQ_WAIT_OPEN_MS * 1000) {
            pr.session_generation = link.session_generation;
            if (xQueueSend(s_result_q, &pr, 0) == pdTRUE) {
                kept++;
                continue;
            }
        }
        free(pr.request_id);
        cJSON_Delete(pr.result);
    }
    s_ble_carry = false;
    if (kept) ESP_LOGI(MG_SESSION_TAG, "%d invoke result(s) from the last session go in this one", kept);
    req_streams_end();

    s_register_acked = false;
    s_register_rejected = false;
    s_heartbeat_registered = false;
    char *reg_json = build_register_json();
    if (!reg_json) {
        ESP_LOGE(MG_SESSION_TAG, "device session: could not build link.register");
        return false;
    }
    link.control_tx = {reg_json, strlen(reg_json), 0, false, link.session_generation};
    link.control_tx.is_registration = true;
    s_connected = true;
    ESP_LOGI(MG_SESSION_TAG, "device session open: POST /link-control over BLE");
    ble_heap_note("at open");
    return true;
}

// Why the codec refused a message, for the log.
static const char *ble_codec_error(ServiceCodecError e) {
    switch (e) {
    case ServiceCodecError::MalformedVarint: return "malformed varint";
    case ServiceCodecError::InvalidFieldKey: return "invalid field key";
    case ServiceCodecError::WrongWireType: return "wrong wire type";
    case ServiceCodecError::LengthOutOfRange: return "length out of range";
    case ServiceCodecError::ValueOutOfRange: return "value out of range";
    case ServiceCodecError::HeaderStorageTooSmall: return "more than 16 headers";
    case ServiceCodecError::UnknownEnvelopeService: return "unknown service";
    case ServiceCodecError::EmptyEnvelopePayload: return "empty payload";
    case ServiceCodecError::UnknownServiceFrameKind: return "unknown frame kind";
    case ServiceCodecError::UnexpectedRequestFrame: return "a Request frame";
    case ServiceCodecError::InvalidClientFrameKind: return "invalid frame kind";
    case ServiceCodecError::StreamIdReserved: return "reserved stream id";
    case ServiceCodecError::RequestPathInvalid: return "invalid request path";
    default: return "malformed";
    }
}

static const char *ble_frame_kind(ServiceFrameKind k) {
    switch (k) {
    case ServiceFrameKind::Request: return "Request";
    case ServiceFrameKind::Response: return "Response";
    case ServiceFrameKind::BodyChunk: return "BodyChunk";
    case ServiceFrameKind::Reset: return "Reset";
    default: return "unknown";
    }
}

// A frame on a stream this session runs: /link-control, or an open daemon
// request. There's no tunnel and no GET /api/gadget/me over BLE, so anything on
// another stream (theirs, a request that has ended, one never opened) is
// nobody's: over Wi-Fi dispatch_inbound_frame() would read its BodyChunks as
// control messages, and end the session on its Reset or refusal.
static bool ble_frame_ours(const DecodedServiceFrame &frame) {
    if (frame.stream_id == CTRL_STREAM_ID) return true;
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    if (frame.stream_id >= REQ_STREAM_BASE && req_find(frame.stream_id)) return true;
#endif
    return false;
}

// One whole message from the VM (n bytes in link.rx), already credited back
// to the client when the session took it. What nothing here handles (a frame
// of an unknown kind, a Request from the VM, a frame on a stream nobody runs,
// a control message of a type this gadget doesn't know) is logged by type and
// size on mg.session and accepted; a malformed one (not a
// ServiceResponseEnvelope, a frame that doesn't decode) is refused, logged
// the same way, and dropped. Neither ends the session: each message reaches
// the gadget whole (the device session frames them), so a bad one can't put
// the next out of step. False only for what ends a Wi-Fi session too: the
// VM resetting or refusing /link-control (dispatch_inbound_frame()).
static bool ble_session_inbound(ble_link &link, size_t n) {
    DecodedServiceResponseEnvelope envelope;
    auto er = DecodeServiceResponseEnvelopeDetailed(ConstByteSpan(link.rx, n), envelope);
    if (!er.ok() || envelope.payload.empty()) {
        ESP_LOGW(MG_SESSION_TAG, "refused a message that isn't a ServiceResponseEnvelope (%u bytes, %s): dropped",
                 (unsigned)n, er.ok() ? "empty payload" : ble_codec_error(er.error));
        return true;
    }
    HeaderView headers[16];
    DecodedServiceFrame frame;
    auto fr = DecodeServerServiceFrameDetailed(envelope.payload, Span<HeaderView>(headers, 16), frame);
    if (!fr.ok()) {
        if (fr.error == ServiceCodecError::UnknownServiceFrameKind
            || fr.error == ServiceCodecError::UnexpectedRequestFrame) {
            ESP_LOGI(MG_SESSION_TAG, "unhandled service frame: %s (%u bytes): accepted",
                     ble_codec_error(fr.error), (unsigned)n);
        } else {
            ESP_LOGW(MG_SESSION_TAG, "refused a service frame that doesn't decode (%u bytes, %s): dropped",
                     (unsigned)n, ble_codec_error(fr.error));
        }
        return true;
    }
    if (!ble_frame_ours(frame)) {
        ESP_LOGI(MG_SESSION_TAG, "unhandled %s frame on stream %lld (%u bytes): accepted",
                 ble_frame_kind(frame.kind), (long long)frame.stream_id, (unsigned)n);
        return true;
    }
    return dispatch_inbound_frame(frame, link.identity, link.session_generation);
}

// session.ping, on the session task: the heartbeat waits for the control
// slot (a result or link.register may be in it), so it never splits another
// control message. Refused without a session, and before the register
// reply (a gadget never sends a heartbeat before it's registered).
static void ble_session_ping(ble_link &link) {
    if (!link.active) {
        ESP_LOGI(MG_SESSION_TAG, "session.ping: no session");
    } else if (!s_heartbeat_registered) {
        ESP_LOGI(MG_SESSION_TAG, "session.ping: not registered yet (no link.register reply, or not registered)");
    } else if (link.ping_pending || link.ping_in_slot) {
        ESP_LOGI(MG_SESSION_TAG, "session.ping: one is already on its way");
    } else {
        link.ping_pending = true;
    }
}

// One turn: the messages from the VM while results have room, then at most
// one control BodyChunk and one request operation, each while the device
// session can queue the largest envelope they make. False if the session
// must end.
static bool ble_session_turn(ble_link &link, bool *did) {
    size_t n;
    while (uxQueueMessagesWaiting(s_result_q) < BLE_RX_RESULTS_MAX
           && (n = mg_ble_session_take(link.id, link.rx, link.rx_cap)) > 0) {
        *did = true;
        if (!ble_session_inbound(link, n)) return false;
    }
    if (mg_ble_session_room() >= OUT_ENV_SCRATCH) {
        if (link.ping_pending && !link.control_tx.json) {
            link.ping_pending = false;
            char *json = strdup(BLE_PING_JSON);
            if (json) {
                link.control_tx = {json, strlen(json), 0, false, link.session_generation};
                link.ping_in_slot = true;
            } else {
                ESP_LOGW(MG_SESSION_TAG, "session.ping: out of memory");
            }
        }
        // link.register and a ping are logged once they've gone whole.
        bool reg = link.control_tx.json && link.control_tx.is_registration;
        size_t len = link.control_tx.json ? link.control_tx.json_len : 0;
        if (!control_tx_step(nullptr, link.out, link.control_tx, link.session_generation,
                             link.heartbeat_sent, link.last_heartbeat_us,
                             link.svc_scratch, link.env_scratch, link.chunk_scratch,
                             did, &link.last_activity_us)) {
            return false;
        }
        if (len && !link.control_tx.json) {
            if (reg) {
                s_register_sent_us = esp_timer_get_time();
                ESP_LOGI(MG_SESSION_TAG, "link.register sent (%u bytes)", (unsigned)len);
                ble_heap_note("after link.register");
            } else if (link.ping_in_slot) {
                link.ping_in_slot = false;
                ESP_LOGI(MG_SESSION_TAG, "session.ping: sent link.heartbeat (%u bytes) on /link-control%s",
                         (unsigned)len, mg_ble_session_sealed() ? ", sealed" : "");
            }
        }
    }
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    // Requests go once link.register is acknowledged (the session is ready).
    if (s_register_acked && !s_req_ready.exchange(true)) {
        if (s_req_wait_since.exchange(0)) {
            ESP_LOGI(MG_SESSION_TAG, "requests that waited for the session go now");
        }
        s_req_wait_opens = 0;
    }
    if (s_req_ready && mg_ble_session_room() >= OUT_ENV_SCRATCH
        && !req_pump_tx(nullptr, link.out, link.svc_scratch, link.env_scratch,
                        link.chunk_scratch, did)) {
        return false;
    }
#endif
    return true;
}

// The session failed on the gadget's side: ask the client to close it, and
// for a new one once it has and the backoff has passed.
static void ble_session_failed(const ble_link &link) {
    uint64_t up_ms = (uint64_t)(esp_timer_get_time() - link.began_us) / 1000;
    update_reconnect_policy(SESSION_FAILED, up_ms, &s_ble_backoff_ms, &s_ble_short_failures);
    ESP_LOGW(MG_SESSION_TAG, "device session failed after %llu ms; asking the client for a new one in %u ms",
             (unsigned long long)up_ms, (unsigned)s_ble_backoff_ms);
    s_ble_reopen = true;
    s_ble_reopen_closed = false;
    s_ble_asking = false;
    s_ble_silent = false;
    s_ble_reopen_at = esp_timer_get_time() + (int64_t)s_ble_backoff_ms * 1000;
    mg_ble_session_request(false);
}

// A state the gadget didn't ask for that it asks again after
// (mgcommands.h): closed or no_network; never open or connecting, nor auth_failed,
// not_enrolled or too_large.
static bool ble_state_reasks(uint8_t st) {
    return st == mg_session_state_closed || st == mg_session_state_no_network;
}

// The series an unasked end continues: closed's or no_network's.
static int ble_ask_kind(uint8_t st) {
    return st == mg_session_state_no_network ? BLE_ASK_NO_NETWORK : BLE_ASK_CLOSED;
}

// Both re-ask series start over.
static void ble_ask_restart() {
    for (int k = 0; k < BLE_ASK_KINDS; k++) {
        s_ble_ask_ms[k] = 0;
        s_ble_asks[k] = 0;
        s_ble_ask_sent_us[k] = 0;
    }
}

// The series' next step for kind k: at once for its first ask, then 1, 2,
// 4 ... s, capped at 60 s after closed and at 15 s after no_network
// (mgcommands.h). ble_ask_peek() only looks;
// ble_ask_step() counts the ask.
static uint32_t ble_ask_peek(int k) {
    uint32_t ms = s_ble_ask_ms[k];
    return !s_ble_asks[k] ? 0
           : !ms ? BLE_ASK_FIRST_MS
           : ms >= BLE_ASK_MAX_MS[k] / 2 ? BLE_ASK_MAX_MS[k]
                                         : ms * 2;
}

static uint32_t ble_ask_step(int k) {
    s_ble_ask_ms[k] = ble_ask_peek(k);
    if (s_ble_asks[k] < UINT8_MAX) s_ble_asks[k]++;
    return s_ble_ask_ms[k];
}

// An ask after such a state, still waiting, is dropped (a failure's reopen stays).
static void ble_ask_cancel() {
    if (s_ble_asking) {
        s_ble_reopen = s_ble_reopen_closed = s_ble_asking = s_ble_silent = false;
    }
}

// A hello answered or a credentials_update saved since the last marks seen
// (hellos, saved: mg_ble_session_reask_marks()) restarts both re-ask series.
// A hello drops an ask still waiting (a new client; its states come next). A
// saved update with the session closed (since the hello the last state ended
// one), whether or not an ask waits, asks at once, the first of the ended
// kind's new series (closed's unless no_network ended it, as the Zephyr side),
// so that kind's next step is 1 s and the other's first goes at once; saved
// while open or before any state, nothing more: new
// credentials may fix what ended it, even auth_failed, and nothing else may
// ask (mgcommands.h). Each save asks once: no loop. True if it
// changed something.
static bool ble_task_active(const ble_link &link);

static bool ble_ask_marks(const ble_link &link, uint32_t hellos, uint32_t saved) {
    // Only marks newer than those taken (a state's noted marks can be older
    // than the current ones a previous pass already took).
    bool hello = (int32_t)(hellos - s_ble_ask_hellos) > 0;
    bool update = (int32_t)(saved - s_ble_ask_saved) > 0;
    if (!hello && !update) return false;
    if (hello) s_ble_ask_hellos = hellos;
    if (update) s_ble_ask_saved = saved;
    ble_ask_restart();
    if (hello) {
        ble_ask_cancel();
        s_ble_ended = false;
    } else if (s_ble_ended && !s_ble_opened_us && mg_ble_connected() && ble_task_active(link)) {
        // Only with an active task (as on Zephyr): a token refresh on an idle
        // gadget opens no session.
        // With none it's saved and the series restart; a task's start asks.
        s_ble_asks[s_ble_ended_kind] = 1;   // the first of the ended kind's new series (as Zephyr)
        s_ble_ask_kinds = (uint8_t)(1u << s_ble_ended_kind);
        s_ble_reopen = s_ble_reopen_closed = s_ble_asking = true;
        s_ble_silent = false;
        s_ble_reopen_at = esp_timer_get_time();
        ESP_LOGI(MG_SESSION_TAG, "new credentials saved with the session closed: asking for one now");
    }
    return true;
}

// Traffic the gadget owes (an active task by itself): results to send,
// requests queued, waiting for a session or open on their streams, and, with
// a session open, link.register unanswered or a control message on its way.
static bool ble_task_owed(const ble_link &link) {
    if (uxQueueMessagesWaiting(s_result_q)) return true;
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    if (uxQueueMessagesWaiting(s_req_q) || s_req_wait_since.load()) return true;
    for (const auto &s : s_req_streams) {
        if (s.id) return true;
    }
#endif
    return link.active && ((!s_register_acked && !s_register_rejected) || link.control_tx.json || link.ping_pending);
}

// The first hold still running (a user action or a wait), or -1.
static int ble_task_held(int64_t now) {
    for (int k = 0; k < NOISE_TASK_KINDS; k++) {
        if (s_task_until[k].load() > now) return k;
    }
    return -1;
}

static bool ble_task_active(const ble_link &link) {
    return ble_task_held(esp_timer_get_time()) >= 0 || ble_task_owed(link);
}

// The task's start and end, once each: a task that starts with no
// session open and none asked for asks as the next of the ended kind's series
// (closed's unless no_network ended the last session): at once when that series
// hasn't asked yet, else at its next step; the other series is untouched
// (mg-session-v1.json, mg_session_ref.py set_task, as the Zephyr gadget). The
// end drops an ask still waiting (a failure's reopen too) and sends no more
// keepalives: the session closes when the client lets it, and nothing asks
// again. True if it did something.
static bool ble_task_update(const ble_link &link) {
    bool active = ble_task_active(link);
    if (active == s_ble_task_on) return false;
    s_ble_task_on = active;
    if (!active) {
        s_ble_reopen = s_ble_reopen_closed = s_ble_asking = s_ble_silent = false;
        ESP_LOGI(MG_SESSION_TAG, "task ended: no more asks or keepalives; an open session closes in %d s unless a task comes",
                 NOISE_CTRL_IDLE_CLOSE_MS / 1000);
        return true;
    }
    int held = ble_task_held(esp_timer_get_time());
    const char *why = held >= 0 ? TASK_NAMES[held] : "traffic owed";
    if (link.active || mg_ble_session_id() || !mg_ble_connected() || s_ble_reopen) {
        ESP_LOGI(MG_SESSION_TAG, "task active (%s)", why);
        return true;
    }
    // The ended kind's next ask: this pass for its first, else when its next
    // step after its last ask comes due (at once if that's past already).
    int k = s_ble_ended_kind;
    int64_t now = esp_timer_get_time();
    int64_t at = s_ble_asks[k] ? s_ble_ask_sent_us[k] + (int64_t)ble_ask_peek(k) * 1000 : now;
    if (at < now) at = now;
    ble_ask_step(k);
    s_ble_ask_kinds = (uint8_t)(1u << k);
    s_ble_reopen = s_ble_reopen_closed = s_ble_asking = true;
    s_ble_silent = false;
    s_ble_reopen_at = at;
    ESP_LOGI(MG_SESSION_TAG, "task active (%s): asking the client for a session%s", why,
             at > now ? " at its series' next step" : "");
    return true;
}

// While a task is active, an open session is kept awake (the keepalive): an
// empty BodyChunk on the existing /link-control stream once
// nothing has gone for NOISE_CTRL_KEEPALIVE_MS, between control messages
// (never inside one) and never a new POST. True if it sent one; false if the
// send failed (the session ends) through *failed.
static bool ble_keepalive(ble_link &link, bool *failed) {
    if (!link.active || !s_ble_task_on || link.control_tx.json || link.ping_pending || link.ping_in_slot
        || esp_timer_get_time() - link.out.last_sent_us() < (int64_t)NOISE_CTRL_KEEPALIVE_MS * 1000
        || mg_ble_session_room() < OUT_ENV_SCRATCH) {
        return false;
    }
    BodyChunkView chunk;
    chunk.end_body = false;   // no data: the stream stays open
    auto sws = link.out.StartOutboundBodyChunk(ServiceType::Daemon, CTRL_STREAM_ID, chunk,
                                               ByteSpan(link.svc_scratch, OUT_SVC_SCRATCH),
                                               ByteSpan(link.env_scratch, OUT_ENV_SCRATCH));
    if (!sws.ok() || !flush_outbound(nullptr, link.out, nullptr)) {
        *failed = true;
        return false;
    }
    ESP_LOGI(MG_SESSION_TAG, "keepalive: an empty /link-control frame (task active)");
    return true;
}

// An open session with no task left: the
// gadget closes it itself NOISE_CTRL_IDLE_CLOSE_MS after the last task ended,
// with open_request [0] (the gadget-asked close both apps drain gracefully),
// once per session. A task meanwhile cancels it and the session is reused.
// The one place [0] goes at a task's end: a waiting re-ask is still dropped
// silently (ble_task_update). True if it sent the close.
static bool ble_idle_close(const ble_link &link) {
    if (!link.active || s_ble_task_on || s_ble_idle_closed) {
        s_ble_idle_close_at = 0;
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (!s_ble_idle_close_at) {
        s_ble_idle_close_at = now + (int64_t)NOISE_CTRL_IDLE_CLOSE_MS * 1000;
        return false;
    }
    if (now < s_ble_idle_close_at) return false;
    s_ble_idle_close_at = 0;
    s_ble_idle_closed = true;
    mg_ble_session_request(false);
    ESP_LOGI(MG_SESSION_TAG, "no task for %d s: closing the session (open_request [0])", NOISE_CTRL_IDLE_CLOSE_MS / 1000);
    return true;
}

// One pass of the BLE session task: follows the client's state reports
// (seen: the last one acted on) by the device session's id, which every
// report changes, so a session that ended and a new one that opened between
// two passes are told apart, and the open one is never begun twice. Then it
// moves messages both ways. True if it did something, so the task goes again
// at once.
static bool ble_session_poll(ble_link &link, uint32_t &seen) {
    bool did = false;
    uint32_t pairing = s_ble_pairing_gen.load();
    if (pairing != s_ble_pairing_seen) {
        // The last pairing's requests and results go nowhere: none waits for,
        // or is carried into, a session under the new one.
        s_ble_pairing_seen = pairing;
        s_ble_carry = false;
        req_end_all();
        pending_result pr;
        while (xQueueReceive(s_result_q, &pr, 0) == pdTRUE) {
            free(pr.request_id);
            cJSON_Delete(pr.result);
        }
        did = true;
    }
    uint32_t seq = s_ble_seq.load();
    // Hellos answered and credentials_updates saved (mg_ble_session_reask_marks()):
    // those before the state this pass takes go before it, the rest after.
    uint32_t hellos, saved;
    mg_ble_session_reask_marks(&hellos, &saved);
    if (seq != seen) {
        seen = seq;
        did = true;
        did |= ble_ask_marks(link, s_ble_state_hellos.load(), s_ble_state_saved.load());
        uint32_t id = mg_ble_session_id();
        if (id != link.id) {
            // An open restarts the series only after a session that stayed
            // open BLE_ASK_STABLE_MS (mgcommands.h); after a shorter
            // one the backoff continues where it was.
            int64_t now = esp_timer_get_time();
            if (s_ble_opened_us && now - s_ble_opened_us >= (int64_t)BLE_ASK_STABLE_MS * 1000) {
                ble_ask_restart();
            }
            s_ble_opened_us = id ? now : 0;
            if (link.active) {
                s_ble_carry = ble_state_reasks(s_ble_state.load()) && link.pairing_gen == s_ble_pairing_gen.load();
            }
            ble_session_end(link, s_ble_state.load() == mg_session_state_too_large);
            link.id = id;
            if (id) {
                s_ble_reopen = s_ble_silent = false;   // the client opened one: run it
                if (!ble_session_begin(link, id)) {
                    ble_session_end(link, false);
                    ble_session_failed(link);
                }
            }
        }
        if (!id && s_ble_reopen) s_ble_reopen_closed = true;
        uint8_t st = s_ble_state.load();
        s_ble_ended = !id && st != mg_session_state_open && st != mg_session_state_connecting;
        s_ble_ended_kind = ble_ask_kind(st);
        if (id) {
            s_ble_asking = false;   // the series goes on: only its end restarts it (above)
        } else if (!mg_ble_connected()) {
            // The link went (its session's end reports closed): nobody to ask; the next link starts at once.
            ble_ask_cancel();
            s_ble_ended = false;
            ble_ask_restart();
        } else if (!ble_state_reasks(st)) {
            if (st != mg_session_state_connecting) ble_ask_cancel();   // auth_failed, not_enrolled, too_large
        } else if (!ble_task_active(link)) {
            // No active task, no re-ask (an idle session closing is normal).
            ble_ask_cancel();
            ESP_LOGI(MG_SESSION_TAG, "the client reported the session %s; no active task, so not asking again",
                     st == mg_session_state_no_network ? "no_network" : "closed");
        } else if (!s_ble_reopen || s_ble_silent || (s_ble_asking && !(s_ble_ask_kinds & (1u << ble_ask_kind(st))))) {
            // At once, then 1, 2, 4 ... s, capped at 60 s after closed and at
            // 15 s after no_network, each kind its own series
            // (mgcommands.h). An ask of the other kind still waiting
            // stays; whichever comes due first asks for both. One set for a
            // silent client (below) is replaced: the client has spoken.
            int k = ble_ask_kind(st);
            if (s_ble_silent) s_ble_reopen = s_ble_silent = false;
            bool waiting = s_ble_reopen;
            uint32_t ms = ble_ask_step(k);
            int64_t at = esp_timer_get_time() + (int64_t)ms * 1000;
            if (!waiting || at < s_ble_reopen_at) s_ble_reopen_at = at;
            s_ble_ask_kinds = (waiting ? s_ble_ask_kinds : 0) | (1u << k);
            s_ble_reopen = true;
            s_ble_reopen_closed = true;
            s_ble_asking = true;
            ESP_LOGI(MG_SESSION_TAG, "the client reported the session %s; asking for one in %u ms",
                     st == mg_session_state_no_network ? "no_network" : "closed", (unsigned)ms);
        }
    }
    did |= ble_ask_marks(link, hellos, saved);
    // A request that waits for a session (noise_ctrl_ble_request_open: owed
    // traffic, a task by itself): never an ask without an active task.
    if (s_ble_ask_now.exchange(false) && !link.active && !mg_ble_session_id() && ble_task_active(link)) {
        did = true;
        s_ble_reopen = s_ble_reopen_closed = s_ble_asking = s_ble_silent = false;
        mg_ble_session_request(true);
        ESP_LOGI(MG_SESSION_TAG, "asked the client for a session now");
    }
    if (s_ble_ping.exchange(false)) {
        did = true;
        ble_session_ping(link);
    }
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
    // Requests that waited NOISE_CTRL_REQ_WAIT_OPEN_MS for a session end with NOISE_CTRL_REQ_NO_SESSION.
    int64_t since = s_req_wait_since.load();
    const int64_t wait_us = (int64_t)NOISE_CTRL_REQ_WAIT_OPEN_MS * 1000;
    if (since && !s_req_ready && esp_timer_get_time() - since >= wait_us) {
        // Only those that waited their own 10 s (the first one's deadline failed newer ones too). Each
        // Open's wait counts from opened_us; its body and cancel go with it. The rest stay, in order, at the front.
        int64_t now = esp_timer_get_time(), oldest = 0;
        size_t queued = (size_t)uxQueueMessagesWaiting(s_req_q);
        req_op *keep = queued ? static_cast<req_op *>(calloc(queued, sizeof(req_op))) : nullptr;
        int64_t *gone = queued ? static_cast<int64_t *>(calloc(queued, sizeof(int64_t))) : nullptr;
        size_t nkeep = 0, ngone = 0;
        req_op op;
        int n = 0;
        // At most the counted ones: keep/gone are sized from that count, and the queue may refill while this
        // drains; anything newer waits in the queue behind the ones put back.
        for (size_t taken = 0; taken < queued && req_take(&op); taken++) {
            bool drop = !keep || !gone;   // no room to sort them: all go, as before
            if (!drop && op.kind == req_op_kind::Open) {
                drop = now - op.opened_us >= wait_us;
                if (drop) gone[ngone++] = op.id;
            } else if (!drop) {
                for (size_t i = 0; i < ngone && !drop; i++) drop = gone[i] == op.id;
            }
            if (drop) {
                n += op.kind == req_op_kind::Open;
                req_drop(op, NOISE_CTRL_REQ_NO_SESSION);
                continue;
            }
            if (op.kind == req_op_kind::Open && (!oldest || op.opened_us < oldest)) oldest = op.opened_us;
            keep[nkeep++] = op;
        }
        for (size_t i = nkeep; i-- > 0;) {   // back in front of anything queued meanwhile, oldest first
            s_req_queued_bytes += (int32_t)keep[i].len;
            if (xQueueSendToFront(s_req_q, &keep[i], 0) != pdTRUE) {
                s_req_queued_bytes -= (int32_t)keep[i].len;
                n += keep[i].kind == req_op_kind::Open;
                req_drop(keep[i], NOISE_CTRL_REQ_NO_SESSION);
            }
        }
        free(keep);
        free(gone);
        s_req_wait_opens = s_req_wait_opens > n ? s_req_wait_opens - n : 0;
        s_req_wait_since = nkeep ? (oldest ? oldest : now) : 0;
        ESP_LOGW(MG_SESSION_TAG, "%d request(s) waited %d s for the session and failed (%u still wait)", n,
                 NOISE_CTRL_REQ_WAIT_OPEN_MS / 1000, (unsigned)nkeep);
        did = true;
    }
#endif
    if (link.active && !ble_session_turn(link, &did)) {
        ble_session_end(link, false);
        did = true;
        // Ended under us (a state report is waiting): nothing to ask for.
        if (s_ble_seq.load() == seen) ble_session_failed(link);
    }
    did |= ble_task_update(link);
    did |= ble_idle_close(link);
    bool failed = false;
    if (ble_keepalive(link, &failed)) {
        did = true;
    } else if (failed) {
        ble_session_end(link, false);
        did = true;
        if (s_ble_seq.load() == seen) ble_session_failed(link);
    }
    if (s_ble_reopen && s_ble_reopen_closed && esp_timer_get_time() >= s_ble_reopen_at) {
        bool asking = s_ble_asking;
        bool silent = s_ble_silent;
        s_ble_reopen = s_ble_reopen_closed = s_ble_asking = s_ble_silent = false;
        did = true;
        if (asking && !mg_ble_connected()) {
            ble_ask_restart();   // gone meanwhile: only while connected, and the next link starts over
        } else if (!ble_task_active(link)) {
            ESP_LOGI(MG_SESSION_TAG, "no active task: the ask that was due doesn't go");
        } else {
            int k = s_ble_ended_kind;
            if (silent) ble_ask_step(k);   // a silent client's ask counts as it goes
            mg_ble_session_request(true);
            int64_t sent = esp_timer_get_time();
            for (int j = 0; j < BLE_ASK_KINDS; j++) {
                if (j == k || (s_ble_ask_kinds & (1u << j))) s_ble_ask_sent_us[j] = sent;
            }
            if (asking) {
                // The next ask is set when this one goes, not only when a state
                // is heard (otherwise the asks stop after
                // the first timed ask while the client stays silent): the
                // series of the last end the client reported (closed's unless
                // no_network), its next step, counted when it goes. A state
                // the client reports meanwhile replaces it with that state's
                // own step, as before.
                int64_t now = esp_timer_get_time();
                int64_t at = now + (int64_t)ble_ask_peek(k) * 1000;
                s_ble_reopen = s_ble_reopen_closed = s_ble_asking = s_ble_silent = true;
                s_ble_ask_kinds = (uint8_t)(1u << k);
                s_ble_reopen_at = at;
                ESP_LOGI(MG_SESSION_TAG, "asked the client for a session (open_request); the next in %u ms",
                         (unsigned)((at - now) / 1000));
            } else {
                ESP_LOGI(MG_SESSION_TAG, "asked the client for a session (open_request)");
            }
        }
    }
    return did;
}

static void noise_ctrl_ble_task(void *arg) {
    (void)arg;
    static ble_link link;
    stack_monitor_t stack = STACK_MONITOR_INIT;
    uint32_t seen = 0;
    for (;;) {
        bool did = ble_session_poll(link, seen);
        stack_monitor_poll(&stack);
        if (did) {
            taskYIELD();
            continue;
        }
        // Results and requests from other tasks don't wake it: poll while
        // open. Waiting to ask again, sleep until the backoff's end.
        TickType_t wait = portMAX_DELAY;
        if (link.active) {
            wait = pdMS_TO_TICKS(IDLE_POLL_MS);
        } else if (s_ble_reopen) {
            int64_t left_ms = (s_ble_reopen_at - esp_timer_get_time()) / 1000;
            wait = pdMS_TO_TICKS(left_ms > IDLE_POLL_MS ? left_ms : IDLE_POLL_MS);
        }
#if CONFIG_HOMEHUB_DAEMON_REQUESTS
        int64_t since = s_req_wait_since.load();
        if (since && !link.active) {
            // A request waits for a session: wake at its deadline.
            int64_t left_ms = (since + (int64_t)NOISE_CTRL_REQ_WAIT_OPEN_MS * 1000 - esp_timer_get_time()) / 1000;
            TickType_t w = pdMS_TO_TICKS(left_ms > 1 ? left_ms : 1);
            wait = w < wait ? w : wait;
        }
#endif
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

extern "C" bool noise_ctrl_ble_start(const char *node_id, const char *display_name) {
    if (s_ble_task) return true;
    strncpy(s_node_id, node_id, sizeof(s_node_id) - 1);
    strncpy(s_display_name, display_name, sizeof(s_display_name) - 1);
    if (!s_result_q) s_result_q = xQueueCreate(8, sizeof(pending_result));
    req_init();
#if CONFIG_SPIRAM
    // A BLE-only build's cJSON is this session's (and its commands'): its
    // nodes PSRAM first.
    cJSON_Hooks hooks = {ble_json_alloc, free};
    cJSON_InitHooks(&hooks);
#endif
    // Its stack follows CONFIG_MUSE_STACKS_IN_PSRAM (muse_stack.h; 8 KB of
    // internal RAM back on the Watcher), like the Wi-Fi session task's:
    // nothing it runs maps flash. Its commands (on_ws_command()) hand
    // anything that does to a task of their own with an internal stack (the
    // bug report), and device.unpair's NVS erases are flash writes, which
    // leave the cache on where the option exists.
    if (!s_result_q
        || muse_task_create(noise_ctrl_ble_task, "mg_vm", BLE_SESSION_STACK, nullptr, 4, &s_ble_task,
                            tskNO_AFFINITY) != pdPASS) {
        s_ble_task = nullptr;
        ESP_LOGE(MG_SESSION_TAG, "failed to start the device session task");
        return false;
    }
    return true;
}

extern "C" void noise_ctrl_ble_state(uint8_t state, uint16_t epoch) {
    (void)epoch;
    // Called with the protocol locked (recursive): the marks as of this state.
    uint32_t hellos, saved;
    mg_ble_session_reask_marks(&hellos, &saved);
    s_ble_state_hellos = hellos;
    s_ble_state_saved = saved;
    s_ble_state = state;
    s_ble_seq++;
    if (s_ble_task) xTaskNotifyGive(s_ble_task);
}

extern "C" void noise_ctrl_ble_message(void) {
    if (s_ble_task) xTaskNotifyGive(s_ble_task);
}

extern "C" void noise_ctrl_ble_request_open(void) {
    s_ble_ask_now = true;   // the task's next pass takes it
    if (s_ble_task) xTaskNotifyGive(s_ble_task);
}

extern "C" void noise_ctrl_ble_pairing_changed(void) {
    s_ble_pairing_gen++;   // the task's next pass drops what came before
    if (s_ble_task) xTaskNotifyGive(s_ble_task);
}

extern "C" void noise_ctrl_ble_ping(void) {
    if (!s_ble_task) {
        ESP_LOGI(MG_SESSION_TAG, "session.ping: no session");
        return;
    }
    s_ble_ping = true;
    xTaskNotifyGive(s_ble_task);
}

extern "C" void noise_ctrl_task_hold(noise_task_t kind, uint32_t ms) {
    if ((int)kind < 0 || kind >= NOISE_TASK_KINDS) return;
    if (!ms) {
        s_task_until[kind] = 0;
    } else {
        if (ms > NOISE_CTRL_WAIT_MAX_MS) ms = NOISE_CTRL_WAIT_MAX_MS;
        int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
        int64_t cur = s_task_until[kind].load();
        while (cur < until && !s_task_until[kind].compare_exchange_weak(cur, until)) {
        }
    }
    if (s_ble_task) xTaskNotifyGive(s_ble_task);   // the start asks at once; the end stops the keepalive
}

extern "C" void noise_ctrl_user_action(void) {
    noise_ctrl_task_hold(NOISE_TASK_USER, NOISE_CTRL_USER_TASK_MS);
}
#else
// A Wi-Fi gadget's own session is always open (the active-task rule covers
// phone-relayed sessions only): the task holds change nothing here.
extern "C" void noise_ctrl_task_hold(noise_task_t kind, uint32_t ms) {
    (void)kind;
    (void)ms;
}

extern "C" void noise_ctrl_user_action(void) {}
#endif  // CONFIG_MUSE_GADGET_BLE_SESSION

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

extern "C" void noise_ctrl_set_status_cb(noise_ctrl_status_cb on_status) {
    s_status_cb = on_status;
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

extern "C" void noise_ctrl_set_node_id(const char *node_id) {
    if (!node_id || !*node_id || strncmp(s_node_id, node_id, sizeof(s_node_id)) == 0) return;
    // Changes only when a setup commits (stores new credentials); a
    // registration then follows.
    snprintf(s_node_id, sizeof(s_node_id), "%s", node_id);
}

extern "C" void noise_ctrl_set_host(const char *host) {
    bool allowed = muse_server_noise_allowed(host);
    if (!allowed) ESP_LOGW(TAG, "Noise host not allowed in this build; using the default");
    snprintf(s_noise_host, sizeof(s_noise_host), "%s",
             allowed && host && *host ? host : NOISE_DEFAULT_HOST);
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
    if (muse_task_slot_run(&s_task_slot, noise_ctrl_task, "noise_ctrl", NOISE_CTRL_STACK, nullptr, 5, &s_task,
                           tskNO_AFFINITY) != pdPASS) {
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

#if CONFIG_MUSE_AVATAR_SYNC
extern "C" bool noise_ctrl_copy_vm_id(char *out, size_t capacity) {
    if (!out || !capacity || !s_connect_mutex) return false;
    if (xSemaphoreTake(s_connect_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    strlcpy(out, s_vm_id, capacity);
    bool have = s_vm_id[0] && strlen(s_vm_id) < capacity;
    xSemaphoreGive(s_connect_mutex);
    return have;
}
#endif

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

#if CONFIG_HOMEHUB_DAEMON_REQUESTS
extern "C" int64_t noise_ctrl_req_open(const char *verb, const char *path,
                                       const char *const *headers, bool end_body,
                                       noise_ctrl_req_cb cb, void *ctx) {
    return noise_ctrl_req_open_hdr(verb, path, headers, end_body, cb, nullptr, ctx);
}

extern "C" int64_t noise_ctrl_req_open_hdr(const char *verb, const char *path,
                                           const char *const *headers, bool end_body,
                                           noise_ctrl_req_cb cb, noise_ctrl_req_hdr_cb hdr_cb,
                                           void *ctx) {
    bool waits = false;
#if CONFIG_MUSE_GADGET_BLE_SESSION
    // The BLE device session: a request made while it isn't ready (open, its
    // link.register acknowledged) waits for it (NOISE_CTRL_REQ_WAIT_OPEN_MS),
    // three at most, and asks for it at once.
    if (s_ble_task && !s_req_ready.load()) {
        if (s_req_wait_opens.load() >= REQ_MAX_STREAMS) return 0;
        waits = true;
    }
#endif
    if (!s_req_q || (!s_connected && !waits) || !verb || !path || !cb) return 0;
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
                 reinterpret_cast<uint8_t *>(blob), len, cb, ctx, esp_timer_get_time(), false, hdr_cb};
    if (!req_put(op, 0)) {
        free(blob);
        return 0;
    }
#if CONFIG_MUSE_GADGET_BLE_SESSION
    if (waits) {
        s_req_wait_opens++;
        int64_t none = 0;
        s_req_wait_since.compare_exchange_strong(none, esp_timer_get_time());
        ESP_LOGI(MG_SESSION_TAG, "request %lld waits for the session (asking the client for one now)", (long long)id);
        noise_ctrl_ble_request_open();   // at once
    }
#endif
    return id;
}

extern "C" bool noise_ctrl_req_send(int64_t id, const void *data, size_t len,
                                    bool end_body, int wait_ms) {
    if (!s_req_q || id < REQ_STREAM_BASE) return false;
    uint8_t *copy = static_cast<uint8_t *>(malloc(len ? len : 1));
    if (!copy) return false;
    if (len) memcpy(copy, data, len);
    req_op op = {req_op_kind::Body, end_body, id, copy, len, nullptr, nullptr, 0, false, nullptr};
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
    req_op op = {req_op_kind::Cancel, false, id, nullptr, 0, nullptr, nullptr, 0, false, nullptr};
    req_put(op, pdMS_TO_TICKS(100));
}

// What a reply frame leaves for its body in SVC_FRAME_SCRATCH, after the
// framing and the response head (status and headers).
#define REQ_FRAME_OVERHEAD 2048

extern "C" size_t noise_ctrl_req_max_body(void) {
#if CONFIG_MUSE_GADGET_BLE_SESSION
    // Over BLE a frame has to fit the device session's max_message instead.
    size_t max = mg_ble_session_max_message();
    if (max && max < SVC_FRAME_SCRATCH) return max - REQ_FRAME_OVERHEAD;
#endif
    return SVC_FRAME_SCRATCH - REQ_FRAME_OVERHEAD;
}
#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS
