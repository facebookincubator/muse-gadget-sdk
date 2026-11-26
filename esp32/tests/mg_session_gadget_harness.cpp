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

// The whole gadget's device session on the host: mg_proto.c and mg_session.c
// (the BLE side) under main/noise_control.cpp's own link code (the control
// sender, link.invoke and link.result, the extra daemon requests, inbound
// dispatch and the device session over BLE), which test_mg_session.py slices
// out of the source unchanged. FreeRTOS, the heap and the BLE stack are host
// stand-ins; build_register_json and send_device_health are the reference
// device's (mg_session_ref.py), so the bytes can match its vectors.
//
//   vectors  every exchange of mg-session-v1.json, byte for byte
//   wifi     the same senders over ClientSession frame the same envelopes
//   extra    request streams, the heartbeat, a small max_message, too_large,
//            async results and a failed session, over BLE
//   relay    what the server sends back through the phone that nothing here
//            handles (logged by type and size, accepted, credited, the
//            session left open), malformed messages refused, link.register
//            and its reply logged, and session.ping
//   reask    the open_request series: when the gadget asks the client for a
//            session again, and how it backs off
//   heap     the session's heap use: bounded, nothing kept after it ends

#define CONFIG_HOMEHUB_DAEMON_REQUESTS 1
#define CONFIG_MUSE_GADGET_BLE_SESSION 1
#define CONFIG_HOMEHUB_ERROR_REPORT 1
#define CONFIG_SPIRAM 1
#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define SMALL_CONTROL_SESSION 0
#define CARDPUTER_CONTROL_SESSION 0
#define SVC_FRAME_SCRATCH 12288
#define WS_BUF_SIZE ClientSession::kMaxOutboundWebSocketPayloadSize
#define TUNNEL_STREAM_ID 2
#define SHOW_AGENT_NAME 0

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "link_noise_control_test_support.h"
#include <xplat/noise/core/TransportFrameCodec.h>

extern "C" {
#include "cJSON.h"
}
#include "mg_ble.h"
#include "mg_session_gadget_ble.h"
#include "mg_session_seal.h"
#include "mg_token_proof.h"
#include "noise_control.h"
#include "ota.h"
#include "mg_session_vectors.h"

static int failures;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            failures++;                                                           \
        }                                                                         \
    } while (0)

// ---- SDK stand-ins ----------------------------------------------------------

// Every log line, as "tag: text", so the relay mode can check what reached
// mg.session (the Nordic UART mirror's tag) and that nothing leaked into it.
static std::vector<std::string> logs;
static void log_line(const char *tag, const char *fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    logs.push_back(std::string(tag) + ": " + text);
    if (getenv("MG_LOG")) fprintf(stderr, "%s: %s\n", tag, text);
}
#define ESP_LOGE log_line
#define ESP_LOGW log_line
#define ESP_LOGI log_line
#define ESP_LOGD log_line
static const char *TAG = "test";

typedef int TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY (-1)
typedef struct esp_tls esp_tls_t;

struct fake_queue {
    size_t item, cap;
    std::deque<std::vector<uint8_t>> items;
};
typedef fake_queue *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned length, unsigned item) { return new fake_queue{item, length, {}}; }
static BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t) {
    if (q->items.size() >= q->cap) return pdFALSE;
    const uint8_t *p = static_cast<const uint8_t *>(item);
    q->items.emplace_back(p, p + q->item);
    return pdTRUE;
}
static inline BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t) {
    if (q->items.size() >= q->cap) return pdFALSE;
    const uint8_t *p = static_cast<const uint8_t *>(item);
    q->items.emplace_front(p, p + q->item);
    return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t) {
    if (q->items.empty()) return pdFALSE;
    memcpy(item, q->items.front().data(), q->item);
    q->items.pop_front();
    return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return (unsigned)q->items.size(); }

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
static int tasks_created;
static BaseType_t xTaskCreate(TaskFunction_t, const char *, unsigned, void *, unsigned, TaskHandle_t *task) {
    tasks_created++;
    *task = &tasks_created;   // never run: the harness polls the session itself
    return pdPASS;
}
// muse_stack.h's: the device session task (mg_vm) follows CONFIG_MUSE_STACKS_IN_PSRAM.
#define tskNO_AFFINITY 0x7fffffff
static BaseType_t muse_task_create(TaskFunction_t fn, const char *name, uint32_t stack_bytes, void *arg,
                                   unsigned prio, TaskHandle_t *task, BaseType_t core) {
    (void)core;
    return xTaskCreate(fn, name, stack_bytes, arg, prio, task);
}
static int notifies;
static void xTaskNotifyGive(TaskHandle_t) { notifies++; }
static uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
static void taskYIELD() {}
static int64_t s_now = 1000000;
static int64_t esp_timer_get_time() { return s_now; }

static void vTaskDelay(TickType_t ticks) { s_now += (int64_t)ticks * 1000; }
struct stack_monitor_t { int unused; };
#define STACK_MONITOR_INIT {0}
static void stack_monitor_poll(stack_monitor_t *) {}

#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }
#define MALLOC_CAP_INTERNAL 4
static size_t heap_caps_get_largest_free_block(int) { return 1u << 20; }
static size_t heap_caps_get_free_size(int) { return 1u << 20; }
static size_t heap_caps_get_minimum_free_size(int) { return 1u << 20; }
// The internal DMA reserve the TLS path waits for (16 KB): dma_low plays a
// gadget below it, as the Watcher's BLE-only build idles (12 KB).
static bool dma_low;
static bool noise_tx_has_dma_headroom(size_t *) { return !dma_low; }
static bool noise_tx_has_dma_headroom_reclaiming(size_t) { return !dma_low; }
static bool noise_tx_has_contiguous_dma_headroom() { return !dma_low; }
static void noise_tunnel_on_inbound(const uint8_t *, size_t) {}
static void noise_tunnel_on_session_down() {}
void ota_start(const char *, bool, ota_status_cb, void *) {}
// A Muse Gadget: Wi-Fi OTA refused by policy (ota.h).
bool ota_wifi_refused(void) { return true; }
void ota_log_refusal(const char *, const char *, const char *) {}

// Wi-Fi's wire: each sealed WebSocket payload (the test crypto seals as
// identity) is one transport frame holding one ServiceRequestEnvelope.
typedef std::vector<uint8_t> bytes_t;
static std::vector<bytes_t> wifi_envelopes;
static bool ws_send_frame_inplace(esp_tls_t *, uint8_t *payload, size_t len) {
    DecodedTransportFrame t;
    CHECK(DecodeTransportFrame(ConstByteSpan(payload, len - Transport::kTagSize), t).ok());
    CHECK(t.total_chunks == 1 && t.chunk_index == 0);
    wifi_envelopes.emplace_back(t.payload.data(), t.payload.data() + t.payload.size());
    return true;
}

// error_report.h's: one crash report waiting until a send completes.
static bool s_report_pending;
static int s_reports_sent;
static char *error_report_pending_json(void) {
    return s_report_pending ? strdup("{\"method\":\"link.error_report\",\"report\":{\"kind\":\"crash\"}}") : nullptr;
}
static void error_report_mark_sent(void) {
    s_report_pending = false;
    s_reports_sent++;
}

// ---- noise_control.cpp's link code ------------------------------------------

#include "link_state.inc"

// The reference device's link.register (mg_session_ref.py).
static char *build_register_json(void) {
    strncpy(s_register_req_id, SV_REGISTER_ID, sizeof(s_register_req_id) - 1);
    return strdup(SV_REGISTER);
}
static void send_device_health(noise_ctrl_session_generation_t, const char *);
static void noise_ota_status(const ota_event_t *, void *) {}

#include "link_sender.inc"
#include "link_requests.inc"
#include "link_identity.inc"
#include "link_results.inc"

// The reference device's device.health payload.
static void send_device_health(noise_ctrl_session_generation_t generation, const char *request_id) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddItemToObject(result, "payload", cJSON_Parse(SV_PAYLOAD_HEALTH));
    queue_result(generation, request_id, result);
}

#include "link_inbound.inc"
#include "link_policy.inc"

// ---- the BLE side: mg_proto.c and mg_session.c (mg_session_gadget_ble.c) ----

static bool holds_token = true, encrypted = true, vm_binding;
static char access_token[MG_SESSION_TOKEN_MAX] = SV_ACCESS, refresh_token[MG_SESSION_TOKEN_MAX] = SV_REFRESH;
static uint64_t stored_expires;   // the pair's access_expires_at, kept with it as the device does (0: none)
static std::vector<bytes_t> notes;          // Control notifications, in order
static std::vector<bytes_t> ble_envelopes;  // what the session code handed mg_ble
static bool record_only;                    // wifi mode: envelopes kept, nothing sent

// Runs once just before the session code's next send: the BLE host task
// changing the session under it, mid-turn.
static void (*before_send)();
extern "C" bool mg_ble_session_send(uint32_t session, const uint8_t *msg, size_t len) {
    if (before_send) {
        void (*f)() = before_send;
        before_send = nullptr;
        f();
    }
    ble_envelopes.emplace_back(msg, msg + len);
    return record_only || gb_send(session, msg, len);
}
extern "C" uint32_t mg_ble_session_id(void) { return gb_id(); }
extern "C" void mg_ble_session_reask_marks(uint32_t *hellos, uint32_t *saved) { gb_reask_marks(hellos, saved); }
extern "C" size_t mg_ble_session_room(void) { return record_only ? (size_t)1 << 30 : gb_room(); }
extern "C" uint32_t mg_ble_session_client_max(void) { return record_only ? 65536 : gb_client_max(); }
extern "C" size_t mg_ble_session_take(uint32_t session, uint8_t *out, size_t cap) { return gb_take(session, out, cap); }
extern "C" void mg_ble_session_request(bool open) { gb_request(open); }
extern "C" bool mg_ble_connected(void) { return gb_connected(); }
extern "C" size_t mg_ble_session_max_message(void) { return SV_MAX_MESSAGE; }
extern "C" bool mg_ble_session_open(void) { return gb_open(); }
extern "C" bool mg_ble_session_sealed(void) { return gb_sealed(); }

#include "link_ble.inc"
#include "link_request_api.inc"

// How many more notifications the board's stack takes (-1: any). The vectors' credentials_wait exchanges: one a step
// (the hello's answer, or the clear's), so the credentials wait for room until the buffer event.
static int stack_room = -1;
static bool f_notify(const uint8_t *d, size_t n) {
    if (stack_room == 0) return false;
    if (stack_room > 0) stack_room--;
    notes.emplace_back(d, d + n);
    return true;
}
static bool f_encrypted() { return encrypted; }
static bool f_credentials(mg_session_creds_t *c) {
    if (!holds_token) return false;
    snprintf(c->node_id, sizeof(c->node_id), "%s", SV_NODE_ID);
    snprintf(c->access_token, sizeof(c->access_token), "%s", access_token);
    snprintf(c->refresh_token, sizeof(c->refresh_token), "%s", refresh_token);
    snprintf(c->device_key, sizeof(c->device_key), "%s", SV_DEVICE_KEY);
    snprintf(c->key_id, sizeof(c->key_id), "%s", SV_KEY_ID);
    snprintf(c->vm_id, sizeof(c->vm_id), "%s", vm_binding ? SV_VM_ID : "");
    snprintf(c->vm_kind, sizeof(c->vm_kind), "%s", vm_binding ? SV_VM_KIND : "");
    c->access_expires_at = stored_expires;   // handed back last, as stored (exchange 13)
    return true;
}
static bool store_fails;   // the next stores fail (credentials_saved [0])
static bool f_store(const char *a, const char *r, uint64_t expires) {
    if (store_fails) return false;
    snprintf(access_token, sizeof(access_token), "%s", a);
    snprintf(refresh_token, sizeof(refresh_token), "%s", r);
    stored_expires = expires;   // an update without one clears it
    return true;
}

static bytes_t unhex(const char *h) {
    bytes_t b;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        b.push_back((uint8_t)v);
    }
    return b;
}
static std::string tohex(const bytes_t &b) {
    std::string s;
    char x[3];
    for (uint8_t v : b) {
        snprintf(x, sizeof(x), "%02x", v);
        s += x;
    }
    return s;
}

// ---- the session code, polled as its task would --------------------------------

static ble_link link;
static uint32_t seen;
static bool reference_heartbeat;   // the reference device sends no heartbeat

// The client gives back credit for what it got (extra mode; the vectors
// have their own credit steps).
static bool client_credits;
static size_t credited_upto;
static void gb_credit_received();

// The device session task's passes until it has nothing left to do.
// A gadget whose task is always active: a user action renewed before
// every pass, as the vectors and the older modes assume (they were written for
// the stay-reachable rule, which now holds only while a task is active).
// Set before a connect, the task is on before the link is: no ask at its start.
static bool always_task;

static void settle() {
    for (int i = 0; i < 200; i++) {
        if (always_task) noise_ctrl_task_hold(NOISE_TASK_USER, NOISE_CTRL_WAIT_MAX_MS);
        bool did = ble_session_poll(link, seen);
        if (reference_heartbeat && link.active) link.heartbeat_sent = true;
        if (client_credits) gb_credit_received();
        if (!did && (!client_credits || credited_upto == notes.size())) return;
    }
    CHECK(!"the session code never settles");
}

// What a client's command callback (app.c's on_ws_command) answers.
static bool async_next;
static std::string async_id;
static noise_ctrl_session_generation_t async_generation;
static cJSON *on_command(const char *command, cJSON *, const char *request_id,
                         noise_ctrl_session_generation_t generation) {
    cJSON *result = cJSON_CreateObject();
    if (async_next) {
        async_next = false;
        async_id = request_id;
        async_generation = generation;
        cJSON_AddBoolToObject(result, "_async", true);
        return result;
    }
    if (!strcmp(command, "display.card")) {
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddItemToObject(result, "payload", cJSON_Parse(SV_PAYLOAD_CARD));
    } else {
        std::string message = std::string("no command named ") + command;
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "message", message.c_str());
        cJSON_AddItemToObject(result, "error", error);
    }
    return result;
}


static bool client_sealed;          // this connection's session is sealed
static mgseal_keys_t client_keys;   // its keys, as the client derives them
static uint32_t client_ctr;         // the client's next counter (client -> device)

static void connect(uint16_t mtu, bool proof) {
    client_sealed = false;
    // A new connection: whatever the last one left is gone.
    gb_disconnect();
    settle();
    snprintf(access_token, sizeof(access_token), "%s", SV_ACCESS);
    snprintf(refresh_token, sizeof(refresh_token), "%s", SV_REFRESH);
    stored_expires = 0;
    holds_token = encrypted = true;
    vm_binding = false;
    // mg_ble's session_state and session_message go to the production handlers.
    gb_ops_t ops = {f_notify, f_encrypted, f_credentials, f_store, noise_ctrl_ble_state, noise_ctrl_ble_message,
                    [] { holds_token = false; }};
    bytes_t k = unhex(PV_K), dn = unhex(PV_DEVICE_NONCE), c = unhex(PV_CHALLENGE), f = unhex(PV_CONFIRM);
    gb_connect(&ops, SV_MAX_MESSAGE, SV_WINDOW, mtu, proof ? k.data() : nullptr, dn.data(), c.data(), c.size(),
               f.data(), f.size());
    CHECK(gb_proof_matched() == proof);
    notes.clear();
    credited_upto = 0;
}

static void write(const bytes_t &frame) {
    gb_write(frame.data(), frame.size());
    settle();
}

// ---- vectors ---------------------------------------------------------------------

static void test_vectors() {
    always_task = false;   // each exchange's own (the vectors' device flag and event task)
    reference_heartbeat = true;
    int bad = 0, steps = 0;
    for (int e = 0; e < SV_NEXCHANGES; e++) {
        const sv_exchange_t &ex = SV_EXCHANGES[e];
        for (int k = 0; k < NOISE_TASK_KINDS; k++) noise_ctrl_task_hold((noise_task_t)k, 0);
        // Held before the connect: on before the link is, so no ask at its start (as the reference's flag).
        if (ex.task) noise_ctrl_task_hold(NOISE_TASK_USER, NOISE_CTRL_WAIT_MAX_MS);
        connect((uint16_t)ex.mtu, false);
        if (ex.proof) {
            // The exchanges' proof (the vectors' session_seal): its K and nonces key their sealed hellos.
            bytes_t k = unhex(SV_SEAL_K), cn = unhex(SV_SEAL_CN), dn = unhex(SV_SEAL_DN);
            bytes_t ch = {mg_command_token_proof, mg_token_proof_challenge};
            ch.insert(ch.end(), cn.begin(), cn.end());
            bytes_t cf(2 + MG_TOKEN_PROOF_MAC_LEN);
            cf[0] = mg_command_token_proof;
            cf[1] = mg_token_proof_confirm;
            CHECK(mg_token_proof_mac(k.data(), false, cn.data(), dn.data(), cf.data() + 2));
            gb_ops_t ops = {f_notify, f_encrypted, f_credentials, f_store, noise_ctrl_ble_state, noise_ctrl_ble_message,
                            [] { holds_token = false; }};
            // Below MG_MIN_ATT_MTU the proof's answer can't go: proved at the floor, then the link's MTU.
            uint16_t floor_mtu = ex.mtu < MG_MIN_ATT_MTU ? MG_MIN_ATT_MTU : (uint16_t)ex.mtu;
            gb_connect(&ops, SV_MAX_MESSAGE, SV_WINDOW, floor_mtu, k.data(), dn.data(), ch.data(), ch.size(),
                       cf.data(), cf.size());
            CHECK(gb_proof_matched());
            if (floor_mtu != ex.mtu) gb_set_mtu((uint16_t)ex.mtu);
            notes.clear();
            credited_upto = 0;
        }
        holds_token = ex.holds_token;
        encrypted = ex.encrypted;
        vm_binding = ex.vm_binding;
        bool waiting = ex.credentials_wait;   // no room for the credentials until the buffer event
        for (int s = 0; s < ex.n_steps; s++, steps++) {
            notes.clear();
            if (waiting) stack_room = ex.steps[s].buffer ? -1 : 1;
            if (ex.steps[s].buffer) {
                // The board's stack has room again (the vectors' event buffer): what waited goes now.
                waiting = false;
                gb_pump();
                settle();
            } else if (ex.steps[s].proof_cn) {
                // A new matching token proof (the vectors' event proof): the session reset at its challenge.
                bytes_t k = unhex(SV_SEAL_K), cn = unhex(ex.steps[s].proof_cn);
                bytes_t ch = {mg_command_token_proof, mg_token_proof_challenge};
                ch.insert(ch.end(), cn.begin(), cn.end());
                bytes_t dn = unhex(ex.steps[s].proof_dn);
                gb_set_nonce(dn.data());
                bytes_t cf(2 + MG_TOKEN_PROOF_MAC_LEN);
                cf[0] = mg_command_token_proof;
                cf[1] = mg_token_proof_confirm;
                CHECK(mg_token_proof_mac(k.data(), false, cn.data(), dn.data(), cf.data() + 2));
                gb_write(ch.data(), ch.size());
                settle();
                std::vector<bytes_t> errs;
                for (const auto &n : notes) {
                    if (n.size() >= 2 && n[0] == mg_command_error) errs.push_back(n);
                }
                if (!errs.empty()) {
                    notes = errs;   // no key after a clear: [token_proof, not_found], the step's notify; no confirm
                } else {
                    gb_write(cf.data(), cf.size());
                    settle();
                    CHECK(gb_proof_matched());
                    notes.clear();
                }
            } else if (ex.steps[s].counters) {
                // A stand-in for a long session (the vectors' event counters): the next counters jump.
                gb_set_counters(ex.steps[s].rx, ex.steps[s].tx);
            } else if (ex.steps[s].nudge) {
                gb_nudge();
                settle();
            } else if (ex.steps[s].task >= 0) {
                // A task starts or ends (the vectors' event task): a user action held, or let go.
                noise_ctrl_task_hold(NOISE_TASK_USER, ex.steps[s].task ? NOISE_CTRL_WAIT_MAX_MS : 0);
                settle();
            } else {
                write(unhex(ex.steps[s].write));
            }
            // The vectors carry the re-ask at once (mgcommands.h, mg-session-v1.json):
            // after a closed or no_network state the gadget didn't ask for,
            // open_request [1] goes at once; and none at once after a short session
            // (exchange 8 step 7: the backoff continues; the clock doesn't
            // move here, so its 1 s step never comes due); byte for byte, nothing set aside.
            bool same = (int)notes.size() == ex.steps[s].n_notify;
            for (int i = 0; same && i < ex.steps[s].n_notify; i++) {
                same = tohex(notes[i]) == ex.steps[s].notify[i];
            }
            if (!same) {
                bad++;
                fprintf(stderr, "%s, step %d: %zu notifications, want %d\n", ex.name, s, notes.size(),
                        ex.steps[s].n_notify);
                for (const auto &n : notes) fprintf(stderr, "  got: %s\n", tohex(n).c_str());
                for (int i = 0; i < ex.steps[s].n_notify; i++) fprintf(stderr, "  want:%s\n", ex.steps[s].notify[i]);
            }
        }
        stack_room = -1;
    }
    for (int k = 0; k < NOISE_TASK_KINDS; k++) noise_ctrl_task_hold((noise_task_t)k, 0);
    CHECK(bad == 0);
    if (!failures) printf("PASS gadget vectors: %d exchanges, %d steps\n", SV_NEXCHANGES, steps);
}

// ---- wifi: the same senders over ClientSession ------------------------------------

// The scenario both transports run: link.register, results, a heartbeat, a
// control message large enough to take several chunks, and requests (with
// headers, a body, a cancel). Returns the envelopes it framed.
template <typename Session>
static void scenario(Session &session, uint8_t *chunk_scratch) {
    noise_ctrl_session_generation_t generation = 41;
    pending_json_body control_tx;
    bool heartbeat_sent = false, did = false;
    uint64_t last_heartbeat_us = 0, last_activity_us = 0;
    s_heartbeat_registered = false;
    auto turns = [&]() {
        for (int i = 0; i < 64; i++) {
            did = false;
            CHECK(control_tx_step(nullptr, session, control_tx, generation, heartbeat_sent, last_heartbeat_us,
                                  svc_scratch, env_scratch, chunk_scratch, &did, &last_activity_us));
            bool sent = false;
            CHECK(req_pump_tx(nullptr, session, svc_scratch, env_scratch, chunk_scratch, &sent));
            if (!did && !sent && !control_tx.json) return;
        }
    };
    char *reg = build_register_json();
    control_tx = {reg, strlen(reg), 0, false, generation};
    control_tx.is_registration = true;
    turns();
    s_heartbeat_registered = true;   // the register reply: the heartbeat follows
    turns();
    for (int i = 0; i < 2; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", true);
        cJSON_AddItemToObject(r, "payload", cJSON_Parse(SV_PAYLOAD_CARD));
        queue_result(generation, i ? "inv-b" : "inv-a", r);
    }
    turns();
    std::string big = "{\"blob\":\"" + std::string(20000, 'x') + "\"}";
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", cJSON_Parse(big.c_str()));
    queue_result(generation, "inv-big", r);
    turns();
    s_connected = true;
    s_req_ready = true;   // open with link.register acknowledged: a request goes at once, never waits
    s_req_next_id = REQ_STREAM_BASE;
    static int ctx;
    const char *headers[] = {"Range", "bytes=0-99", nullptr};
    auto ignore = [](void *, int, const uint8_t *, size_t, bool) {};
    int64_t a = noise_ctrl_req_open("GET", "/fs/raw/workspace/a.jpg", headers, true, ignore, &ctx);
    int64_t b = noise_ctrl_req_open("POST", "/fs/upload", nullptr, false, ignore, &ctx);
    CHECK(a && b);
    CHECK(noise_ctrl_req_send(b, "hello", 5, false, 0));
    CHECK(noise_ctrl_req_send(b, " world", 6, true, 0));
    turns();
    int64_t c = noise_ctrl_req_open("GET", "/chat/history", nullptr, true, ignore, &ctx);
    noise_ctrl_req_cancel(c);
    turns();
    req_end_all();
    clear_pending_json_body(control_tx);
    s_connected = false;
    s_req_ready = false;
}

// The same frame in both, on each transport's service: the control stream on
// the Gadget API over Wi-Fi (/api/gadget/control), on the daemon over BLE (the
// device session's /link-control, the route the phone's relay carries); the
// daemon requests on the daemon in both. Counts the control-stream frames.
static bool same_frame(const bytes_t &wifi, const bytes_t &ble, int *control) {
    DecodedServiceRequestEnvelope w, b;
    if (!DecodeServiceRequestEnvelopeDetailed(ConstByteSpan(wifi.data(), wifi.size()), w).ok()
        || !DecodeServiceRequestEnvelopeDetailed(ConstByteSpan(ble.data(), ble.size()), b).ok()) return false;
    HeaderView headers[8];
    DecodedServiceFrame frame;
    if (!DecodeServiceFrameDetailed(w.payload, Span<HeaderView>(headers), frame).ok()) return false;
    bool on_control = frame.stream_id == CTRL_STREAM_ID;
    *control += on_control;
    return w.service == (on_control ? ServiceType::Gadget : ServiceType::Daemon)
        && b.service == ServiceType::Daemon && w.payload.size() == b.payload.size()
        && !memcmp(w.payload.data(), b.payload.data(), w.payload.size());
}

static void test_wifi() {
    reset_noise_session();
    wifi_envelopes.clear();
    s_report_pending = true;   // a crash report waits: it goes once the VM confirmed the registration
    s_reports_sent = 0;
    scenario(*noise_session, ws_buf);
    CHECK(s_reports_sent == 1 && !s_report_pending);
    record_only = true;
    ble_envelopes.clear();
    BleSessionOut out;
    out.Reset(65536, 1);
    static uint8_t chunk[CTRL_BODY_CHUNK_MAX];
    s_report_pending = true;
    scenario(out, chunk);
    CHECK(s_reports_sent == 2 && !s_report_pending);   // a BLE-only gadget reports its crash too
    record_only = false;
    CHECK(wifi_envelopes.size() >= 12);
    CHECK(wifi_envelopes.size() == ble_envelopes.size());
    int control = 0, same = 0;
    for (size_t i = 0; i < wifi_envelopes.size() && i < ble_envelopes.size(); i++) {
        same += same_frame(wifi_envelopes[i], ble_envelopes[i], &control);
    }
    CHECK(same == (int)wifi_envelopes.size() && control >= 4 && control < same);
    if (same != (int)wifi_envelopes.size()) {
        fprintf(stderr, "wifi %zu envelopes, ble %zu, %d the same\n", wifi_envelopes.size(), ble_envelopes.size(), same);
    }
    bool report = false;
    for (const auto &e : ble_envelopes) {
        report |= std::string(e.begin(), e.end()).find("link.error_report") != std::string::npos;
    }
    CHECK(report);
    if (!failures) printf("PASS gadget wifi: %zu envelopes, the same frames, control on Gadget over Wi-Fi and on the daemon over BLE\n", wifi_envelopes.size());
}

// ---- extra: the client's side, by hand ---------------------------------------------

static void put32(uint8_t *b, uint32_t v) {
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
}
static bytes_t hello_frame(uint32_t max_message, uint32_t window) {
    bytes_t m = {mg_command_session, mg_session_hello, MG_SESSION_VERSION, 0, 0, 0, 0, 0, 0, 0, 0};
    put32(&m[3], max_message);
    put32(&m[7], window);
    return m;
}

// The client's hello (a gadget answers only a sealed one, one per proof): the token
// proof's challenge and confirm again (the device's nonce is the same here, so are the keys), then the
// sealed hello; from here the client's data is sealed under its counters and the device's opened.
static bytes_t client_hello(uint32_t max_message, uint32_t window) {
    bytes_t ch = unhex(PV_CHALLENGE), cf = unhex(PV_CONFIRM), k = unhex(PV_K), dn = unhex(PV_DEVICE_NONCE);
    gb_write(ch.data(), ch.size());
    gb_write(cf.data(), cf.size());
    client_sealed = true;
    client_ctr = 0;
    CHECK(mgseal_keys(&client_keys, k.data(), ch.data() + 2, dn.data(), SV_NODE_ID, strlen(SV_NODE_ID)) == MGSEAL_OK);
    bytes_t m = hello_frame(max_message, window);
    m.push_back(mg_session_hello_flag_sealed);
    return m;
}
// A credentials_update as the client sends it in its sealed session (never plain): one
// fragment, sealed under its next counter.
static bytes_t client_update() {
    static const char pair[] = "{\"access_token\":\"A2\",\"refresh_token\":\"R2\"}";
    size_t n = strlen(pair);
    bytes_t sealed(n + MGSEAL_OVERHEAD);
    memcpy(sealed.data() + 4, pair, n);
    CHECK(mgseal_seal(&client_keys, MGSEAL_DIR_CLIENT, client_ctr++, sealed.data(), n) == MGSEAL_OK);
    bytes_t f = {mg_command_session, mg_session_credentials_update,
                 (uint8_t)(mg_session_flag_first | mg_session_flag_last | mg_session_flag_sealed), 0, 0, 0, 0};
    put32(&f[3], (uint32_t)sealed.size());
    f.insert(f.end(), sealed.begin(), sealed.end());
    return f;
}
static bytes_t state_frame(uint8_t st, uint16_t epoch) {
    return {mg_command_session, mg_session_state, st, (uint8_t)epoch, (uint8_t)(epoch >> 8)};
}
static uint16_t client_mtu = 247;
static void gb_credit_received() {
    uint32_t n = 0;
    for (; credited_upto < notes.size(); credited_upto++) {
        const bytes_t &d = notes[credited_upto];
        if (d.size() >= 3 && d[0] == mg_command_session && d[1] == mg_session_data) {
            n += (uint32_t)(d.size() - (d[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER));
        }
    }
    if (n) {
        bytes_t c = {mg_command_session, mg_session_credit, 0, 0, 0, 0};
        put32(&c[2], n);
        gb_write(c.data(), c.size());
    }
}
static void client_send(const bytes_t &plain, bool then_settle = true, uint8_t extra_flags = 0) {
    bytes_t msg = plain;
    if (client_sealed && !(extra_flags & mg_session_flag_sealed)) {
        msg.assign(plain.size() + MGSEAL_OVERHEAD, 0);
        memcpy(msg.data() + 4, plain.data(), plain.size());
        CHECK(mgseal_seal(&client_keys, MGSEAL_DIR_CLIENT, client_ctr++, msg.data(), plain.size()) == MGSEAL_OK);
        extra_flags |= mg_session_flag_sealed;
    }
    size_t off = 0;
    while (off < msg.size()) {
        bool first = off == 0;
        size_t hdr = first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        size_t n = std::min<size_t>(client_mtu - 3 - hdr, msg.size() - off);
        // mgcommands.h: a fragment past the device's credit is dropped with its message, so the
        // client sends what its credit allows, waiting while it has none, as a real one does (the device's own
        // count: what it gave back less what it took).
        for (int tries = 0; gb_rx_credit() == 0 && tries < 50; tries++) settle();
        n = std::min<size_t>(n, gb_rx_credit() ? gb_rx_credit() : n);
        bytes_t f = {mg_command_session, mg_session_data,
                     (uint8_t)(extra_flags | (first ? mg_session_flag_first : 0)
                               | (off + n == msg.size() ? mg_session_flag_last : 0))};
        if (first) {
            f.resize(7);
            put32(&f[3], (uint32_t)msg.size());
        }
        f.insert(f.end(), msg.begin() + off, msg.begin() + off + n);
        gb_write(f.data(), f.size());
        off += n;
    }
    if (then_settle) settle();
}
// A ServiceResponseEnvelope with one frame from the VM.
static bytes_t vm_frame(ServiceFrameView frame) {
    static uint8_t f[16384], e[16384];
    auto fr = EncodeServiceFrame(frame, ByteSpan(f, sizeof(f)));
    CHECK(fr.ok());
    auto er = EncodeServiceResponseEnvelope(ConstByteSpan(f, fr.size()), ByteSpan(e, sizeof(e)));
    CHECK(er.ok());
    return bytes_t(e, e + er.size());
}
static bytes_t vm_response(int64_t stream, int32_t status, const std::string &body, bool end) {
    ApplicationResponseView r;
    r.status = status;
    r.body = ConstByteSpan(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    r.end_body = end;
    return vm_frame(ServiceFrameView{stream, ServiceFrameKind::Response, {}, r, {}, {}});
}
static bytes_t control_bytes_of(const std::string &json) {
    bytes_t data(4);
    put32(data.data(), (uint32_t)json.size());
    data.insert(data.end(), json.begin(), json.end());
    return data;
}
// A BodyChunk on /link-control carrying these bytes of the control stream.
static bytes_t vm_control_chunk(const bytes_t &data) {
    BodyChunkView c;
    c.data = ConstByteSpan(data.data(), data.size());
    return vm_frame(ServiceFrameView{CTRL_STREAM_ID, ServiceFrameKind::BodyChunk, {}, {}, c, {}});
}
static bytes_t vm_control(const std::string &json) {
    return vm_control_chunk(control_bytes_of(json));
}

// The device's messages in notes[from...], reassembled, each a decoded frame.
struct device_msg {
    bytes_t envelope;
    int64_t stream;
    ServiceFrameKind kind;
    std::string verb, path, body;
    bool end_body;
};
static std::vector<device_msg> device_messages(size_t from = 0, size_t *largest = nullptr) {
    std::vector<device_msg> out;
    bytes_t cur;
    for (size_t i = from; i < notes.size(); i++) {
        const bytes_t &n = notes[i];
        if (n.size() < 3 || n[0] != mg_command_session || n[1] != mg_session_data) continue;
        size_t hdr = n[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        if (n[2] & mg_session_flag_first) cur.clear();
        cur.insert(cur.end(), n.begin() + hdr, n.end());
        if (!(n[2] & mg_session_flag_last)) continue;
        if (largest && cur.size() > *largest) *largest = cur.size();
        if (client_sealed) {
            uint32_t ctr = (uint32_t)cur[0] << 24 | (uint32_t)cur[1] << 16 | (uint32_t)cur[2] << 8 | cur[3];
            size_t plain = 0;
            CHECK(mgseal_open(&client_keys, MGSEAL_DIR_DEVICE, ctr, cur.data(), cur.size(), &plain) == MGSEAL_OK);
            cur = bytes_t(cur.begin() + 4, cur.begin() + 4 + plain);
        }
        DecodedServiceRequestEnvelope env;
        DecodedServiceFrame f;
        HeaderView headers[8];
        CHECK(DecodeServiceRequestEnvelopeDetailed(ConstByteSpan(cur.data(), cur.size()), env).ok());
        CHECK(DecodeServiceFrameDetailed(env.payload, Span<HeaderView>(headers, 8), f).ok());
        device_msg m = {cur, f.stream_id, f.kind, "", "", "", false};
        if (f.kind == ServiceFrameKind::Request) {
            m.verb.assign(f.request.verb.data(), f.request.verb.size());
            m.path.assign(f.request.path.data(), f.request.path.size());
            m.end_body = f.request.end_body;
        } else if (f.kind == ServiceFrameKind::BodyChunk) {
            m.body.assign(reinterpret_cast<const char *>(f.body_chunk.data.data()), f.body_chunk.data.size());
            m.end_body = f.body_chunk.end_body;
        }
        out.push_back(m);
    }
    return out;
}
// The control stream's JSON messages, from body chunks on stream 1.
static std::vector<std::string> control_json(const std::vector<device_msg> &msgs) {
    std::string stream;
    for (const auto &m : msgs) {
        if (m.stream == CTRL_STREAM_ID && m.kind == ServiceFrameKind::BodyChunk) stream += m.body;
    }
    std::vector<std::string> out;
    size_t at = 0;
    while (stream.size() - at >= 4) {
        uint32_t n = (uint8_t)stream[at] | (uint8_t)stream[at + 1] << 8 | (uint8_t)stream[at + 2] << 16
                     | (uint32_t)(uint8_t)stream[at + 3] << 24;
        if (stream.size() - at - 4 < n) break;
        out.push_back(stream.substr(at + 4, n));
        at += 4 + n;
    }
    return out;
}
static bool has_open_request(size_t from, uint8_t value) {
    for (size_t i = from; i < notes.size(); i++) {
        if (notes[i].size() == 3 && notes[i][0] == mg_command_session && notes[i][1] == mg_session_open_request
            && notes[i][2] == value) {
            return true;
        }
    }
    return false;
}

// A session as a client opens it, up to the register reply.
static void open_session(uint32_t max_message, uint16_t epoch) {
    write(client_hello(max_message, max_message));
    write(state_frame(mg_session_state_open, epoch));
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    client_send(vm_control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}"));
}

struct req_log {
    int calls = 0, status = 0;
    std::string body;
    bool end = false;
};
static void on_req(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    auto *l = static_cast<req_log *>(ctx);
    l->calls++;
    if (status) l->status = status;
    l->body.append(reinterpret_cast<const char *>(data ? data : (const uint8_t *)""), len);
    l->end = end;
}

static void test_extra() {
    always_task = true;
    // Below the TLS path's DMA reserve throughout: the device session doesn't wait for it.
    dma_low = true;
    // The heartbeat a Wi-Fi gadget sends after the register reply goes over BLE too.
    reference_heartbeat = false;
    client_credits = true;
    connect(247, true);
    open_session(65536, 1);
    auto json = control_json(device_messages());
    CHECK(json.size() == 2 && json[0] == SV_REGISTER && json[1] == "{\"method\":\"link.heartbeat\"}");
    CHECK(s_connected && link.active);

    // A request stream: GET /fs/raw over the device session, and its reply.
    req_log fs;
    size_t mark = notes.size();
    int64_t id = noise_ctrl_req_open("GET", "/fs/raw/workspace/pic.jpg", nullptr, true, on_req, &fs);
    CHECK(id >= REQ_STREAM_BASE);
    settle();
    auto msgs = device_messages(mark);
    CHECK(msgs.size() == 1 && msgs[0].stream == id && msgs[0].kind == ServiceFrameKind::Request
          && msgs[0].verb == "GET" && msgs[0].path == "/fs/raw/workspace/pic.jpg" && msgs[0].end_body);
    client_send(vm_response(id, 200, "JPEG", false));
    BodyChunkView rest;
    rest.data = ConstByteSpan(reinterpret_cast<const uint8_t *>("DATA"), 4);
    rest.end_body = true;
    client_send(vm_frame(ServiceFrameView{id, ServiceFrameKind::BodyChunk, {}, {}, rest, {}}));
    CHECK(fs.status == 200 && fs.body == "JPEGDATA" && fs.end && fs.calls == 2);

    // A burst the session code hasn't caught up with (the client sends what
    // credit allows; nothing settles in between): every message waits, and
    // every invoke gets its result, in order.
    mark = notes.size();
    for (int i = 0; i < 24; i++) {
        client_send(vm_control("{\"method\":\"link.invoke\",\"id\":\"burst-" + std::to_string(i)
                               + "\",\"command\":\"display.card\",\"params\":{}}"), false);
    }
    settle();
    json = control_json(device_messages(mark));
    CHECK(json.size() == 24);
    for (size_t i = 0; i < json.size(); i++) {
        CHECK(json[i] == "{\"method\":\"link.result\",\"id\":\"burst-" + std::to_string(i)
                             + "\",\"ok\":true,\"payload\":" + SV_PAYLOAD_CARD + "}");
    }
    for (size_t i = mark; i < notes.size(); i++) CHECK(notes[i][0] != mg_command_error);

    // Control messages cut across BodyChunks anywhere (the stream is bytes):
    // inside a length prefix, inside a message, and a 40 KB one too long to
    // hold, skipped whole without losing step.
    mark = notes.size();
    bytes_t stream = control_bytes_of(
        "{\"method\":\"link.invoke\",\"id\":\"split-1\",\"command\":\"display.card\",\"params\":{}}");
    bytes_t huge = control_bytes_of("{\"method\":\"link.invoke\",\"id\":\"huge\",\"command\":\"display.card\","
                                    "\"params\":{\"x\":\"" + std::string(40000, 'z') + "\"}}");
    bytes_t second = control_bytes_of(
        "{\"method\":\"link.invoke\",\"id\":\"split-2\",\"command\":\"display.card\",\"params\":{}}");
    stream.insert(stream.end(), huge.begin(), huge.end());
    stream.insert(stream.end(), second.begin(), second.end());
    size_t cuts[] = {2, 30, 90, 3000, 10000, 20000, 30000, 39000, stream.size() - 50, stream.size() - 3, stream.size()};
    size_t from = 0;
    for (size_t cut : cuts) {
        client_send(vm_control_chunk(bytes_t(stream.begin() + from, stream.begin() + cut)));
        from = cut;
    }
    json = control_json(device_messages(mark));
    CHECK(json.size() == 2 && json[0].find("\"split-1\"") != std::string::npos
          && json[1].find("\"split-2\"") != std::string::npos);

    // An invoke whose result comes later from another task (app.c's async commands).
    mark = notes.size();
    async_next = true;
    client_send(vm_control("{\"method\":\"link.invoke\",\"id\":\"inv-9\",\"command\":\"display.card\",\"params\":{}}"));
    CHECK(control_json(device_messages(mark)).empty());
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    noise_ctrl_send_command_result(async_generation, async_id.c_str(), r);
    settle();
    json = control_json(device_messages(mark));
    CHECK(json.size() == 1 && json[0] == "{\"method\":\"link.result\",\"id\":\"inv-9\",\"ok\":true}");

    // too_large from the client: the request in flight ends as over Wi-Fi.
    req_log big;
    noise_ctrl_req_open("GET", "/fs/raw/huge.bin", nullptr, true, on_req, &big);
    settle();
    write(state_frame(mg_session_state_too_large, 1));
    CHECK(big.status == NOISE_CTRL_REQ_TOO_LARGE && big.end && !s_connected && !link.active);
    // A result for that session never reaches the next one.
    r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    noise_ctrl_send_command_result(async_generation, "stale", r);

    // A client that takes 4096-byte messages: every one fits, the control
    // stream still carries the whole result.
    mark = notes.size();
    open_session(4096, 2);
    std::string blob(10000, 'y');
    r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "blob", blob.c_str());
    cJSON_AddItemToObject(r, "payload", payload);
    size_t marker = notes.size();
    noise_ctrl_send_command_result(link.session_generation, "inv-10", r);
    settle();
    size_t largest = 0;
    msgs = device_messages(marker, &largest);
    json = control_json(msgs);
    CHECK(msgs.size() == 3 && largest <= 4096);
    if (msgs.size() != 3 || largest > 4096) fprintf(stderr, "%zu messages, largest %zu\n", msgs.size(), largest);
    CHECK(json.size() == 1 && json[0] == "{\"method\":\"link.result\",\"id\":\"inv-10\",\"ok\":true,\"payload\":{\"blob\":\""
                                             + blob + "\"}}");
    for (const auto &j : control_json(device_messages(mark))) CHECK(j.find("\"stale\"") == std::string::npos);

    // A failed session (the VM resets /link-control, as Wi-Fi reconnects on):
    // the device asks the client to close it, then, once it has and Wi-Fi's
    // backoff has passed (2 s after a short session), for a new one.
    mark = notes.size();
    ResetView reset{ResetCode::InternalError, StringView("gone")};
    client_send(vm_frame(ServiceFrameView{CTRL_STREAM_ID, ServiceFrameKind::Reset, {}, {}, {}, reset}));
    CHECK(!link.active && has_open_request(mark, 0) && !has_open_request(mark, 1));
    write(state_frame(mg_session_state_closed, 2));
    CHECK(!has_open_request(mark, 1));
    s_now += 1900 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1));
    s_now += 100 * 1000;
    settle();
    CHECK(has_open_request(mark, 1));
    write(state_frame(mg_session_state_open, 3));
    CHECK(link.active && control_json(device_messages(mark))[0] == SV_REGISTER);
    // It fails again at once: twice the wait (4 s).
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    mark = notes.size();
    client_send(vm_frame(ServiceFrameView{CTRL_STREAM_ID, ServiceFrameKind::Reset, {}, {}, {}, reset}));
    write(state_frame(mg_session_state_closed, 3));
    s_now += 3900 * 1000;
    settle();
    CHECK(has_open_request(mark, 0) && !has_open_request(mark, 1));
    s_now += 100 * 1000;
    settle();
    CHECK(has_open_request(mark, 1));
    // The client opens a session on its own meanwhile: it runs, and no open_request follows.
    mark = notes.size();
    client_send(vm_frame(ServiceFrameView{CTRL_STREAM_ID, ServiceFrameKind::Reset, {}, {}, {}, reset}));
    write(state_frame(mg_session_state_open, 4));
    CHECK(link.active);
    s_now += 60 * 1000 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1));
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));

    // Closed and opened again by the host task while the session task is
    // about to send the old session's result: it never goes into the new
    // session, which is begun once, from its start, and isn't a failure.
    mark = notes.size();
    r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    noise_ctrl_send_command_result(link.session_generation, "late", r);
    before_send = [] {
        gb_write(state_frame(mg_session_state_closed, 4).data(), 5);
        gb_write(state_frame(mg_session_state_open, 5).data(), 5);
    };
    settle();
    CHECK(!before_send && !has_open_request(mark, 0));
    for (const auto &j : control_json(device_messages(mark))) CHECK(j.find("\"late\"") == std::string::npos);
    auto reopened = device_messages(mark);
    CHECK(link.active && link.id == gb_id());
    CHECK(!reopened.empty() && reopened[0].kind == ServiceFrameKind::Request && reopened[0].path == "/link-control");
    int posts = 0;
    for (const auto &m : reopened) posts += m.kind == ServiceFrameKind::Request && m.path == "/link-control";
    CHECK(posts == 1);
    // A stale send (the session code still on the old id) is refused.
    CHECK(!gb_send(link.id - 1, reopened[0].envelope.data(), reopened[0].envelope.size()));

    // The client reports a state the gadget didn't ask for (no_network, or
    // closed) while none is open (a phone can fail its first
    // open while the handed-off token refreshes, and never try again;
    // iOS ends the relay's sockets at each Bluetooth wake): the device asks for
    // one with open_request [1] at once, then after 1 s, 2, 4, 8 ... and at
    // most 60 s after closed, 15 s after no_network (mgcommands.h); never
    // after auth_failed, not_enrolled or too_large (one cancels an ask still
    // waiting); the backoff starts again after a session that stayed open
    // 60 s, at a hello or saved credentials, and at the link's end (test_reask
    // runs those), and nothing is asked while it's down. (closed with none open
    // isn't reported to the session code: mg_session drops it.) The picture
    // test's press asks at once.
    mark = notes.size();
    write(state_frame(mg_session_state_no_network, 5));
    CHECK(!link.active && !s_connected && has_open_request(mark, 1));   // at once (0 ms)
    const uint32_t waits[] = {1000, 2000, 4000, 8000, 15000, 15000, 15000, 15000};   // no_network: capped at 15 s
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
        mark = notes.size();
        write(state_frame(mg_session_state_no_network, 5));
        s_now += (int64_t)(waits[i] - 100) * 1000;
        settle();
        CHECK(!has_open_request(mark, 1));
        s_now += 100 * 1000;
        settle();
        CHECK(has_open_request(mark, 1));
    }
    mark = notes.size();
    write(state_frame(mg_session_state_auth_failed, 5));
    s_now += 300LL * 1000 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1));
    // An ask still waiting is dropped by a state that needs something else
    // than a retry (not_enrolled, too_large, auth_failed).
    for (uint8_t stop : {(uint8_t)mg_session_state_not_enrolled, (uint8_t)mg_session_state_too_large,
                         (uint8_t)mg_session_state_auth_failed}) {
        mark = notes.size();
        write(state_frame(mg_session_state_no_network, 5));
        s_now += 500 * 1000;
        settle();
        write(state_frame(stop, 5));
        s_now += 120LL * 1000 * 1000;
        settle();
        CHECK(!has_open_request(mark, 1));
    }
    // connecting isn't a state to ask again after, and doesn't cancel.
    mark = notes.size();
    write(state_frame(mg_session_state_no_network, 5));
    write(state_frame(mg_session_state_connecting, 5));
    s_now += 60LL * 1000 * 1000;
    settle();
    CHECK(has_open_request(mark, 1));
    // The client closes an open session on its own (iOS at a Bluetooth wake,
    // the Watcher's 30 s closes): asked again at once (open_session's
    // hello restarted the series); the phone's no_network after it runs its
    // own series: at once, then after 1 s, then 2 s.
    open_session(65536, 6);
    CHECK(link.active && s_connected);
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    mark = notes.size();
    write(state_frame(mg_session_state_closed, 6));
    CHECK(!link.active && has_open_request(mark, 1));
    mark = notes.size();
    write(state_frame(mg_session_state_no_network, 6));
    CHECK(has_open_request(mark, 1));
    for (uint32_t w : {1000u, 2000u}) {
        // (A closed with none open isn't reported to the session code: the
        // phone's next unasked state while it stays closed is no_network.)
        mark = notes.size();
        write(state_frame(mg_session_state_no_network, 6));
        s_now += (int64_t)(w - 100) * 1000;
        settle();
        CHECK(!has_open_request(mark, 1));
        s_now += 100 * 1000;
        settle();
        CHECK(has_open_request(mark, 1));
    }
    mark = notes.size();
    noise_ctrl_ble_request_open();
    settle();
    CHECK(has_open_request(mark, 1));
    // A request the gadget makes while the session is closed (mgcommands.h,
    // Session state; the picture test's chat POST in an iOS wake's gap) waits
    // for it: accepted, the client asked at once, nothing sent until the
    // session is open and link.register acknowledged, then sent in order with
    // its body; an invoke whose session closed under it has its result go in
    // the next session too.
    {
        open_session(65536, 8);
        client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
        async_next = true;
        client_send(vm_control("{\"method\":\"link.invoke\",\"id\":\"inv-wake\",\"command\":\"display.card\",\"params\":{}}"));
        noise_ctrl_session_generation_t inv_gen = async_generation;
        write(state_frame(mg_session_state_closed, 8));   // iOS ends the relay at a wake
        CHECK(!link.active && !s_connected);
        cJSON *late = cJSON_CreateObject();
        cJSON_AddBoolToObject(late, "ok", true);
        noise_ctrl_send_command_result(inv_gen, "inv-wake", late);   // done while closed
        mark = notes.size();
        req_log chat, second;
        static const char *const hdrs[] = {"Content-Type", "application/json", nullptr};
        int64_t c1 = noise_ctrl_req_open("POST", "/chat/stream", hdrs, false, on_req, &chat);
        CHECK(c1 >= REQ_STREAM_BASE && noise_ctrl_req_send(c1, "{\"message\":\"hi\"}", 16, true, 0));
        int64_t c2 = noise_ctrl_req_open("GET", "/fs/raw/workspace/next.jpg", nullptr, true, on_req, &second);
        CHECK(c2 > c1);
        settle();
        CHECK(has_open_request(mark, 1) && device_messages(mark).empty() && chat.calls == 0);
        s_now += 400 * 1000;   // the phone opens a new session 0.4 s later
        write(state_frame(mg_session_state_open, 9));
        auto before_ack = device_messages(mark);
        bool early = false;
        for (const auto &m : before_ack) early |= m.stream == c1 || m.stream == c2;
        CHECK(!early);   // not before link.register is acknowledged
        client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
        size_t acked = notes.size();
        client_send(vm_control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}"));
        settle();
        auto out = device_messages(acked);
        int at1 = -1, at_body = -1, at2 = -1;
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i].stream == c1 && out[i].kind == ServiceFrameKind::Request && out[i].path == "/chat/stream") at1 = (int)i;
            if (out[i].stream == c1 && out[i].kind == ServiceFrameKind::BodyChunk && out[i].body == "{\"message\":\"hi\"}"
                && out[i].end_body) at_body = (int)i;
            if (out[i].stream == c2 && out[i].kind == ServiceFrameKind::Request) at2 = (int)i;
        }
        CHECK(at1 >= 0 && at_body > at1 && at2 > at_body);   // in order, with its body
        auto all = control_json(device_messages(mark));
        bool carried = false;
        for (const auto &j : all) carried |= j == "{\"method\":\"link.result\",\"id\":\"inv-wake\",\"ok\":true}";
        CHECK(carried);
        CHECK(chat.calls == 0 && !s_req_wait_since && s_req_wait_opens == 0);
        client_send(vm_response(c1, 200, "ok", true));
        client_send(vm_response(c2, 200, "JPEG", true));
        CHECK(chat.status == 200 && chat.end && second.status == 200 && second.end);
    }
    // Each waits its own 10 s (the first one's deadline failed a newer one too): one opened 8 s
    // after another still waits when the first fails, then fails 8 s later; nothing goes in a later session.
    {
        write(state_frame(mg_session_state_closed, 9));
        CHECK(!link.active);
        req_log a, b;
        CHECK(noise_ctrl_req_open("GET", "/fs/raw/workspace/a.jpg", nullptr, true, on_req, &a));
        s_now += 8000 * 1000;
        settle();
        CHECK(noise_ctrl_req_open("GET", "/fs/raw/workspace/b.jpg", nullptr, true, on_req, &b));
        s_now += 2000 * 1000;
        settle();
        CHECK(a.calls == 1 && a.status == NOISE_CTRL_REQ_NO_SESSION && b.calls == 0);
        CHECK(s_req_wait_since && s_req_wait_opens == 1 && uxQueueMessagesWaiting(s_req_q) == 1);
        s_now += 7900 * 1000;
        settle();
        CHECK(b.calls == 0);
        s_now += 100 * 1000;
        settle();
        CHECK(b.calls == 1 && b.status == NOISE_CTRL_REQ_NO_SESSION && b.end);
        CHECK(!s_req_wait_since && s_req_wait_opens == 0 && !uxQueueMessagesWaiting(s_req_q));
    }
    // ...and one that waits 10 s for a session that never opens fails cleanly
    // with NOISE_CTRL_REQ_NO_SESSION (the picture test then says "isn't
    // connected yet"); a fourth waiting at once is refused; nothing of them
    // goes in a later session.
    {
        write(state_frame(mg_session_state_closed, 9));
        CHECK(!link.active);
        req_log w[3];
        for (auto &l : w) CHECK(noise_ctrl_req_open("GET", "/fs/raw/workspace/w.jpg", nullptr, true, on_req, &l));
        req_log extra;
        CHECK(!noise_ctrl_req_open("GET", "/fs/raw/workspace/x.jpg", nullptr, true, on_req, &extra));
        s_now += 9900 * 1000;
        settle();
        CHECK(w[0].calls == 0);
        s_now += 100 * 1000;
        settle();
        for (auto &l : w) CHECK(l.calls == 1 && l.status == NOISE_CTRL_REQ_NO_SESSION && l.end && l.body.empty());
        CHECK(!s_req_wait_since && s_req_wait_opens == 0 && !uxQueueMessagesWaiting(s_req_q));
        mark = notes.size();
        open_session(65536, 10);
        settle();
        for (const auto &m : device_messages(mark)) CHECK(m.path != "/fs/raw/workspace/w.jpg");
        write(state_frame(mg_session_state_closed, 10));
    }

    // A GET or HEAD in flight when the session ends unasked (the phone's relay)
    // goes back to the wait-for-open queue once, keeping its place, and is
    // re-sent after the reopen and the register ack; one that already got
    // response bytes fails as before; a second unasked end re-sends nothing
    // more; a POST (any other method) fails with the session, never re-sent.
    {
        auto reopen = [&](uint16_t epoch) -> size_t {
            write(state_frame(mg_session_state_open, epoch));
            client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
            size_t acked = notes.size();
            client_send(vm_control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}"));
            settle();
            return acked;
        };
        auto sent_again = [&](size_t from, int64_t id, const char *path) {
            for (const auto &m : device_messages(from)) {
                if (m.stream == id && m.kind == ServiceFrameKind::Request && m.path == path) return true;
            }
            return false;
        };
        static const char *const hdrs[] = {"Content-Type", "application/json", nullptr};
        static const char *const HIST = "/chat/history?limit=1";
        // (a) A GET closed 50 ms after it went, no reply: it waits, then goes again.
        open_session(65536, 11);
        client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
        req_log a;
        size_t m0 = notes.size();
        int64_t ia = noise_ctrl_req_open("GET", HIST, nullptr, true, on_req, &a);
        CHECK(ia);
        settle();
        CHECK(sent_again(m0, ia, HIST));
        s_now += 50 * 1000;
        size_t m1 = notes.size();
        write(state_frame(mg_session_state_closed, 11));
        CHECK(!link.active && a.calls == 0 && has_open_request(m1, 1));
        s_now += 400 * 1000;
        size_t acked = reopen(12);
        CHECK(sent_again(acked, ia, HIST) && a.calls == 0);
        bool early = false;   // nothing of it between the close and the register ack
        auto before_ack = device_messages(m1);
        size_t n_before = device_messages(m1).size() - device_messages(acked).size();
        for (size_t i = 0; i < n_before && i < before_ack.size(); i++) early |= before_ack[i].stream == ia;
        CHECK(!early);
        client_send(vm_response(ia, 200, "ok", true));
        CHECK(a.status == 200 && a.end && a.body == "ok" && a.calls == 1);

        // (c) Re-sent once: a second unasked end fails it.
        req_log c;
        int64_t ic = noise_ctrl_req_open("HEAD", HIST, nullptr, true, on_req, &c);
        CHECK(ic);
        settle();
        write(state_frame(mg_session_state_closed, 12));
        CHECK(c.calls == 0);
        acked = reopen(13);
        CHECK(sent_again(acked, ic, HIST) && c.calls == 0);
        size_t m2 = notes.size();
        write(state_frame(mg_session_state_closed, 13));
        CHECK(c.calls == 1 && c.status == -1 && c.end);
        acked = reopen(14);
        for (const auto &m : device_messages(m2)) CHECK(m.stream != ic);

        // (d) A POST in flight with no reply: it fails with the session (its caller retries), never re-sent.
        req_log d;
        int64_t id = noise_ctrl_req_open("POST", "/chat/stream", hdrs, false, on_req, &d);
        CHECK(id && noise_ctrl_req_send(id, "{\"message\":\"hi\"}", 16, true, 0));
        settle();
        size_t m4 = notes.size();
        write(state_frame(mg_session_state_closed, 14));
        CHECK(d.calls == 1 && d.status == -1 && d.end);
        acked = reopen(15);
        for (const auto &m : device_messages(m4)) CHECK(m.stream != id);

        // (b) Response bytes before the close: it fails as before, nothing re-sent.
        req_log b;
        int64_t ib = noise_ctrl_req_open("POST", "/chat/stream", hdrs, false, on_req, &b);
        CHECK(ib && noise_ctrl_req_send(ib, "{}", 2, true, 0));
        settle();
        client_send(vm_response(ib, 200, "par", false));   // the reply's first bytes
        CHECK(b.calls == 1 && b.status == 200 && !b.end);
        size_t m3 = notes.size();
        write(state_frame(mg_session_state_closed, 15));
        CHECK(b.calls == 2 && b.status == -1 && b.end);   // failed at the end, as before
        acked = reopen(16);
        for (const auto &m : device_messages(m3)) CHECK(m.stream != ib);
        write(state_frame(mg_session_state_closed, 16));
    }

    // ...but never while one is open.
    open_session(65536, 7);
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    mark = notes.size();
    noise_ctrl_ble_request_open();
    settle();
    CHECK(link.active && !has_open_request(mark, 1));

    // The connection's end ends the session and its requests.
    req_log gone;
    noise_ctrl_req_open("GET", "/fs/raw/x", nullptr, true, on_req, &gone);
    settle();
    mark = notes.size();
    gb_disconnect();
    settle();
    CHECK(!link.active && !s_connected && gone.status == -1 && gone.end);
    // Its session's end reports closed: nothing is asked with the link down,
    // and the next link's first ask comes after 1 s again.
    s_now += 120LL * 1000 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1) && !s_ble_asking && s_ble_asks[0] == 0 && s_ble_asks[1] == 0);
    if (!failures) printf("PASS gadget extra\n");
}


// ---- the re-ask series (mgcommands.h) -------------------
//
// An open restarts the series only after a session that stayed open 60 s;
// after a shorter one the backoff continues (1, 2, 4 ... capped at 60 s), so a
// server that accepts and then ends cleanly at once is never asked faster. A
// hello answered or a credentials_update saved restarts it too; the link's end
// as before.

// The client opens the session the gadget asked for (no new hello), and the VM
// accepts /link-control: the session is up.
static void reopen(uint16_t epoch) {
    write(state_frame(mg_session_state_open, epoch));
    client_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    CHECK(link.active && s_connected);
}
// Holds the open session for ms, then the client ends it with st (closed, or
// no_network; not asked for): true if open_request [1] went at once.
static bool held_then_ended(uint16_t epoch, int64_t ms, uint8_t st, size_t *mark) {
    s_now += ms * 1000;
    settle();
    *mark = notes.size();
    write(state_frame(st, epoch));
    CHECK(!link.active);
    return has_open_request(*mark, 1);
}
static bool held_then_closed(uint16_t epoch, int64_t ms, size_t *mark) {
    return held_then_ended(epoch, ms, mg_session_state_closed, mark);
}
// The next ask comes exactly wait_ms after the end at mark, not 100 ms sooner.
static void asks_after(size_t mark, int64_t wait_ms) {
    s_now += (wait_ms - 100) * 1000;
    settle();
    CHECK(!has_open_request(mark, 1));
    s_now += 100 * 1000;
    settle();
    CHECK(has_open_request(mark, 1));
}

static void test_reask() {
    always_task = true;
    reference_heartbeat = true;
    client_credits = true;
    connect(247, true);
    open_session(65536, 1);   // a hello: fresh series
    uint16_t epoch = 1;
    size_t mark = 0;
    const uint8_t CLOSED = mg_session_state_closed, NONET = mg_session_state_no_network;
    // Ten 5 s sessions the server ends cleanly (closed): the first end asks
    // at once (a fresh series), every later one waits for the next step,
    // never at once, up to 60 s.
    CHECK(held_then_closed(epoch, 5000, &mark));
    printf("REASK closed#1 at once\n");
    const int64_t closed_waits[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000, 60000};
    for (int64_t w : closed_waits) {
        reopen(++epoch);
        CHECK(!held_then_closed(epoch, 5000, &mark));   // no immediate re-ask
        asks_after(mark, w);
        printf("REASK closed#%d after %lld ms\n", epoch, (long long)w);
    }
    CHECK(epoch == 10);
    // Ten 5 s sessions the phone ends with no_network: their own series (the
    // closed one doesn't hold it back): at once, then up to 15 s.
    reopen(++epoch);
    CHECK(held_then_ended(epoch, 5000, NONET, &mark));
    printf("REASK no_network#1 at once\n");
    const int64_t nonet_waits[] = {1000, 2000, 4000, 8000, 15000, 15000, 15000, 15000, 15000};
    for (int i = 0; i < 9; i++) {
        reopen(++epoch);
        CHECK(!held_then_ended(epoch, 5000, NONET, &mark));
        asks_after(mark, nonet_waits[i]);
        printf("REASK no_network#%d after %lld ms\n", i + 2, (long long)nonet_waits[i]);
    }
    // Neither restarted the other: closed goes on at 60 s, no_network at 15 s.
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));
    asks_after(mark, 60000);
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));
    asks_after(mark, 15000);
    printf("REASK neither restarts the other\n");
    // Just under 60 s open: still a short session.
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 59999, &mark));
    asks_after(mark, 60000);
    // 61 s open: both series start over: closed at once, and a no_network
    // after the next short session at once too; then 1 s each.
    reopen(++epoch);
    CHECK(held_then_closed(epoch, 61000, &mark));
    reopen(++epoch);
    CHECK(held_then_ended(epoch, 5000, NONET, &mark));
    printf("REASK 61s at once\n");
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));
    asks_after(mark, 1000);
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));
    asks_after(mark, 1000);
    // Exactly 60 s counts (open 60 s or more restarts them).
    reopen(++epoch);
    CHECK(held_then_ended(epoch, 60000, NONET, &mark));
    // An ask of one kind waiting and an end of the other: the other series'
    // first asks at once (whichever comes due first asks for both).
    reopen(++epoch);
    CHECK(held_then_closed(epoch, 5000, &mark));    // closed restarted by the 60 s session: at once
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));
    asks_after(mark, 1000);
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));   // closed: its 2 s step waits
    mark = notes.size();
    write(state_frame(NONET, epoch));   // the phone's network went too (none open)
    CHECK(!has_open_request(mark, 1));  // no_network's series was at 1 s: not at once
    asks_after(mark, 1000);             // its step comes before closed's 2 s
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));
    asks_after(mark, 2000);             // and that ask counted: no_network's next is 2 s
    printf("REASK kinds overlap\n");

    // A credentials_update saved restarts both series, and saved while the
    // session has ended asks at once, the closed series' first (exchange 8
    // step 12; as the Zephyr side does); one not saved changes nothing.
    const sv_exchange_t *ex8 = nullptr;
    for (int e = 0; e < SV_NEXCHANGES; e++) {
        if (strstr(SV_EXCHANGES[e].name, "re-ask")) ex8 = &SV_EXCHANGES[e];
    }
    CHECK(ex8 && ex8->n_steps > 12);
    auto update = [&](const char *want) {
        size_t at = notes.size();
        write(client_update());   // a sealed credentials_update
        bool answered = false;
        for (size_t i = at; i < notes.size(); i++) answered |= tohex(notes[i]) == want;
        CHECK(answered);
    };
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));   // closed's 4 s step waits
    s_now += 500 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1));
    update("890801");
    CHECK(has_open_request(mark, 1));   // asked now
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));   // closed's next: 1 s (the save's ask was its first)
    asks_after(mark, 1000);
    reopen(++epoch);
    CHECK(held_then_ended(epoch, 5000, NONET, &mark));   // no_network restarted: at once
    // Saved after a no_network end: the ask is no_network's first (its next
    // step 1 s), and closed's first still goes at once.
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));   // no_network's 1 s step waits
    update("890801");
    CHECK(has_open_request(mark, 1));
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));
    asks_after(mark, 1000);
    reopen(++epoch);
    CHECK(held_then_closed(epoch, 5000, &mark));
    // Saved while open: nothing then; the next end asks at once.
    reopen(++epoch);
    update("890801");
    CHECK(held_then_closed(epoch, 5000, &mark));
    printf("REASK credentials_update restarts\n");
    // Not saved: nothing changes (the waiting 1 s step stays, no ask now).
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));
    store_fails = true;
    update("890800");
    store_fails = false;
    CHECK(!has_open_request(mark, 1));
    asks_after(mark, 1000);
    printf("REASK unsaved update changes nothing\n");
    // Saved after auth_failed, nothing waiting (exchange 8 step 12): at once.
    reopen(++epoch);
    mark = notes.size();
    write(state_frame(mg_session_state_auth_failed, epoch));
    s_now += 120LL * 1000 * 1000;
    settle();
    CHECK(!link.active && !has_open_request(mark, 1) && !s_ble_reopen);
    update("890801");
    CHECK(has_open_request(mark, 1));
    // The same in one pass (auth_failed, then the update, before the session
    // task runs): the update comes after the state, so it still asks.
    reopen(++epoch);
    mark = notes.size();
    bytes_t af = state_frame(mg_session_state_auth_failed, epoch), upd = client_update();
    gb_write(af.data(), af.size());
    gb_write(upd.data(), upd.size());
    settle();
    CHECK(has_open_request(mark, 1));
    // Saved right after a hello, before any state: nothing more.
    write(client_hello(65536, 65536));
    mark = notes.size();
    update("890801");
    settle();
    CHECK(!has_open_request(mark, 1));
    // A hello then no_network in one pass: the hello comes first, so the
    // no_network starts fresh and asks at once (never stranded).
    reopen(++epoch);
    CHECK(held_then_ended(epoch, 5000, NONET, &mark));    // fresh after that hello: at once
    reopen(++epoch);
    CHECK(!held_then_ended(epoch, 5000, NONET, &mark));   // a 1 s step waits
    mark = notes.size();
    bytes_t h = client_hello(65536, 65536), nn = state_frame(NONET, epoch);
    gb_write(h.data(), h.size());
    gb_write(nn.data(), nn.size());
    settle();
    CHECK(has_open_request(mark, 1));
    printf("REASK saved after an end asks at once\n");
    // A state's noted marks older than those a pass already took are no
    // news (the pass read seq before the state and the marks after it).
    {
        uint32_t h0 = s_ble_ask_hellos, u0 = s_ble_ask_saved;
        CHECK(!ble_ask_marks(link, h0 - 1, u0) && !ble_ask_marks(link, h0, u0 - 1) && !ble_ask_marks(link, h0, u0));
        CHECK(s_ble_ask_hellos == h0 && s_ble_ask_saved == u0);
    }
    // A hello restarts both.
    reopen(++epoch);
    CHECK(held_then_closed(epoch, 5000, &mark));
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark));
    write(client_hello(65536, 65536));
    reopen(++epoch);
    CHECK(held_then_closed(epoch, 5000, &mark));
    printf("REASK hello restarts\n");
    // The link's end: the ask waiting is dropped when it comes due (nobody to
    // ask), both series start over, and the next link asks at once.
    reopen(++epoch);
    CHECK(!held_then_closed(epoch, 5000, &mark) && s_ble_asks[BLE_ASK_CLOSED] > 1);
    gb_disconnect();
    s_now += 120LL * 1000 * 1000;
    settle();
    CHECK(!has_open_request(mark, 1) && !s_ble_asking);
    for (int k = 0; k < BLE_ASK_KINDS; k++) CHECK(s_ble_asks[k] == 0 && s_ble_ask_ms[k] == 0);
    connect(247, true);
    open_session(65536, 1);
    CHECK(held_then_closed(1, 5000, &mark));
    printf("REASK new link at once\n");

    // A failed open (mg-session-v1 exchange 14): the gadget's open_request
    // goes, the client reports connecting, then closed (or no_network) with
    // no session ever open. Each is an end the gadget didn't ask for: the
    // first of a fresh series asks at once, every later one arms that
    // series' next step (never silent, never sooner), up to its cap; a
    // same-kind failure while an ask waits asks nothing extra and leaves its
    // timer standing; connecting arms and cancels nothing.
    connect(247, true);
    write(client_hello(65536, 65536));   // a fresh client: both series fresh, no session yet
    auto failed_open = [&](uint8_t st) {
        mark = notes.size();
        write(state_frame(mg_session_state_connecting, 1));
        CHECK(!has_open_request(mark, 1) && !link.active);
        write(state_frame(st, 1));
        CHECK(!link.active);
        return has_open_request(mark, 1);
    };
    CHECK(failed_open(CLOSED));
    printf("REASK failed open closed#1 at once\n");
    for (size_t i = 0; i < sizeof(closed_waits) / sizeof(closed_waits[0]); i++) {
        CHECK(!failed_open(CLOSED));   // the ask went; that open failed too
        asks_after(mark, closed_waits[i]);
        printf("REASK failed open closed#%zu after %lld ms\n", i + 2, (long long)closed_waits[i]);
    }
    // no_network failures run their own series: at once, then up to 15 s.
    CHECK(failed_open(NONET));
    printf("REASK failed open no_network#1 at once\n");
    for (int i = 0; i < 9; i++) {
        CHECK(!failed_open(NONET));
        asks_after(mark, nonet_waits[i]);
        printf("REASK failed open no_network#%d after %lld ms\n", i + 2, (long long)nonet_waits[i]);
    }
    // A failure while an ask of its kind waits (the client opened on its
    // own): nothing extra, and the ask goes when it was due, never dropped.
    write(client_hello(65536, 65536));
    CHECK(failed_open(CLOSED));                 // fresh: at once
    CHECK(!failed_open(CLOSED));                // 1 s step armed
    asks_after(mark, 1000);
    CHECK(!failed_open(CLOSED));                // 2 s step armed at mark
    size_t armed = mark;
    s_now += 500 * 1000;
    settle();
    CHECK(!failed_open(CLOSED));                // waiting: nothing now
    s_now += 1400 * 1000;                       // 1.9 s after the armed step's start
    settle();
    CHECK(!has_open_request(armed, 1));
    s_now += 100 * 1000;                        // 2 s: the step stands
    settle();
    CHECK(has_open_request(armed, 1));
    CHECK(!failed_open(CLOSED));                // and that ask counted: next 4 s
    asks_after(mark, 4000);
    printf("REASK failed open waiting ask stands\n");
    // Exchange 14 timed: closed at once, no_network at once (its own), then
    // closed waits for its 1 s step.
    write(client_hello(65536, 65536));
    CHECK(failed_open(CLOSED));
    CHECK(failed_open(NONET));
    CHECK(!failed_open(CLOSED));
    asks_after(mark, 1000);
    printf("REASK failed open exchange 14 timed\n");
    if (!failures) printf("PASS gadget reask\n");
}

// ---- relay: unhandled and malformed messages, session.ping --------------------------
//
// Run on a sealed session (a sealed hello, as the Muse apps' are: every data
// message sealed both ways, mgcommands.h, Sealed data), the client's side
// sealing and opening here with the shared seal and the token proof's K and
// nonces.


static bool logged(size_t from, const std::string &needle) {
    for (size_t i = from; i < logs.size(); i++) {
        if (logs[i].find(needle) != std::string::npos) return true;
    }
    return false;
}
static size_t logged_count(size_t from, const std::string &needle) {
    size_t n = 0;
    for (size_t i = from; i < logs.size(); i++) n += logs[i].find(needle) != std::string::npos;
    return n;
}
// Message bytes the device gave back as credit in notes[from...].
static uint32_t credit_since(size_t from) {
    uint32_t n = 0;
    for (size_t i = from; i < notes.size(); i++) {
        const bytes_t &d = notes[i];
        if (d.size() == 6 && d[0] == mg_command_session && d[1] == mg_session_credit) {
            n += (uint32_t)d[2] | (uint32_t)d[3] << 8 | (uint32_t)d[4] << 16 | (uint32_t)d[5] << 24;
        }
    }
    return n;
}
static size_t errors_since(size_t from, uint16_t code) {
    size_t n = 0;
    for (size_t i = from; i < notes.size(); i++) {
        const bytes_t &d = notes[i];
        n += d.size() == 5 && d[0] == mg_command_error && d[1] == mg_command_session && d[2] == mg_error_data_code
             && d[3] == (uint8_t)code && d[4] == (uint8_t)(code >> 8);
    }
    return n;
}
// What goes on the wire for a message: sealed with the client's next
// counter on a sealed session (counter || ciphertext || tag), else itself.
static bytes_t client_wire(const bytes_t &msg, uint32_t ctr) {
    if (!client_sealed) return msg;
    bytes_t w(msg.size() + MGSEAL_OVERHEAD);
    memcpy(w.data() + 4, msg.data(), msg.size());
    CHECK(mgseal_seal(&client_keys, MGSEAL_DIR_CLIENT, ctr, w.data(), msg.size()) == MGSEAL_OK);
    return w;
}
// One message from the VM through the phone, as the client sends it; the
// bytes it put on the wire (what credit counts).
static size_t relay_send(const bytes_t &msg) {
    bytes_t w = client_wire(msg, client_ctr);
    if (client_sealed) client_ctr++;
    client_send(w, true, client_sealed ? mg_session_flag_sealed : 0);
    return w.size();
}
// The device's data messages in notes[from...], opened on a sealed session
// (each with the counter it carries, which must come in order).
static std::vector<device_msg> relay_messages(size_t from = 0) {
    if (!client_sealed) return device_messages(from);
    std::vector<device_msg> out;
    bytes_t cur;
    for (size_t i = from; i < notes.size(); i++) {
        const bytes_t &n = notes[i];
        if (n.size() < 3 || n[0] != mg_command_session || n[1] != mg_session_data) continue;
        CHECK(n[2] & mg_session_flag_sealed);   // every fragment of sealed data says so
        size_t hdr = n[2] & mg_session_flag_first ? MG_SESSION_FIRST_HEADER : MG_SESSION_DATA_HEADER;
        if (n[2] & mg_session_flag_first) cur.clear();
        cur.insert(cur.end(), n.begin() + hdr, n.end());
        if (!(n[2] & mg_session_flag_last)) continue;
        uint32_t ctr = (uint32_t)cur[0] << 24 | (uint32_t)cur[1] << 16 | (uint32_t)cur[2] << 8 | cur[3];
        size_t plain = 0;
        CHECK(mgseal_open(&client_keys, MGSEAL_DIR_DEVICE, ctr, cur.data(), cur.size(), &plain) == MGSEAL_OK);
        DecodedServiceRequestEnvelope env;
        DecodedServiceFrame f;
        HeaderView headers[8];
        bytes_t envb(cur.begin() + 4, cur.begin() + 4 + plain);
        CHECK(DecodeServiceRequestEnvelopeDetailed(ConstByteSpan(envb.data(), envb.size()), env).ok());
        CHECK(DecodeServiceFrameDetailed(env.payload, Span<HeaderView>(headers, 8), f).ok());
        device_msg m = {envb, f.stream_id, f.kind, "", "", "", false};
        if (f.kind == ServiceFrameKind::Request) {
            m.path.assign(f.request.path.data(), f.request.path.size());
        } else if (f.kind == ServiceFrameKind::BodyChunk) {
            m.body.assign(reinterpret_cast<const char *>(f.body_chunk.data.data()), f.body_chunk.data.size());
        }
        m.verb = std::to_string(ctr);   // the counter, for the order check
        out.push_back(m);
    }
    return out;
}
// A ServiceResponseEnvelope around any bytes, valid frame or not.
static bytes_t vm_envelope(const bytes_t &payload) {
    static uint8_t e[16384];
    auto er = EncodeServiceResponseEnvelope(ConstByteSpan(payload.data(), payload.size()), ByteSpan(e, sizeof(e)));
    CHECK(er.ok());
    return bytes_t(e, e + er.size());
}
static bytes_t vm_chunk_on(int64_t stream, const std::string &data) {
    BodyChunkView c;
    c.data = ConstByteSpan(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    return vm_frame(ServiceFrameView{stream, ServiceFrameKind::BodyChunk, {}, {}, c, {}});
}
static const char *const kSecret = "SECRET-tok-9f3a";   // never in any log line

// One message from the VM through the phone: the session takes it whole,
// gives every byte back as credit, logs `want` on mg.session (never the
// secret) and stays open.
static void relay_case(const char *name, const bytes_t &msg, const std::string &want) {
    size_t nmark = notes.size(), lmark = logs.size();
    size_t wire = relay_send(msg);
    bool ok = link.active && s_connected && gb_open() && credit_since(nmark) == wire
              && logged(lmark, "mg.session: " + want);
    for (size_t i = lmark; i < logs.size(); i++) ok = ok && logs[i].find(kSecret) == std::string::npos;
    for (size_t i = nmark; i < notes.size(); i++) ok = ok && notes[i][0] != mg_command_error;
    CHECK(ok);
    if (!ok) {
        fprintf(stderr, "%s%s: active %d, credit %u of %zu, want \"%s\"\n", client_sealed ? "sealed: " : "", name,
                link.active, (unsigned)credit_since(nmark), wire, want.c_str());
        for (size_t i = lmark; i < logs.size(); i++) fprintf(stderr, "  log: %s\n", logs[i].c_str());
    }
}
// An invoke still gets its result: the control stream is in step.
static void invoke_answered(const std::string &id) {
    size_t mark = notes.size();
    relay_send(vm_control("{\"method\":\"link.invoke\",\"id\":\"" + id + "\",\"command\":\"display.card\",\"params\":{}}"));
    auto json = control_json(relay_messages(mark));
    CHECK(json.size() == 1 && json[0].find("\"" + id + "\"") != std::string::npos);
    if (json.size() != 1) fprintf(stderr, "%s%s: %zu results\n", client_sealed ? "sealed: " : "", id.c_str(), json.size());
}

// A connection and a sealed hello.
static void relay_connect() {
    connect(247, true);
    client_sealed = true;
    client_ctr = 0;
    bytes_t hello = hello_frame(65536, 65536);
    hello.push_back(mg_session_hello_flag_sealed);
    encrypted = false;   // no bond needed
    bytes_t k = unhex(PV_K), ch = unhex(PV_CHALLENGE), dn = unhex(PV_DEVICE_NONCE);
    CHECK(mgseal_keys(&client_keys, k.data(), ch.data() + 2, dn.data(), SV_NODE_ID, strlen(SV_NODE_ID)) == MGSEAL_OK);
    write(hello);
    CHECK(gb_sealed());
}

static void run_relay() {
    reference_heartbeat = false;
    client_credits = true;
    const std::string secret = kSecret;

    // session.ping with no session: refused, nothing sent.
    relay_connect();
    size_t lmark = logs.size(), mark = notes.size();
    noise_ctrl_ble_ping();
    settle();
    CHECK(logged(lmark, "mg.session: session.ping: no session") && relay_messages(mark).empty());

    // Open, /link-control accepted, no register reply yet: link.register is
    // logged once it has gone, and a ping is refused until it's registered.
    lmark = logs.size();
    mark = notes.size();
    write(state_frame(mg_session_state_open, 1));
    relay_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    auto msgs = relay_messages(mark);
    CHECK(link.active && msgs.size() >= 2 && msgs[0].kind == ServiceFrameKind::Request && msgs[0].path == "/link-control");
    CHECK(control_json(msgs).size() == 1 && control_json(msgs)[0] == SV_REGISTER);
    CHECK(logged_count(lmark, "mg.session: link.register sent (" + std::to_string(strlen(SV_REGISTER)) + " bytes)") == 1);
    // The device's data takes the counters after its credentials' 0, in order.
    for (size_t i = 0; i < msgs.size(); i++) CHECK(msgs[i].verb == std::to_string(i + 1));
    mark = notes.size();
    noise_ctrl_ble_ping();
    settle();
    CHECK(logged(lmark, "mg.session: session.ping: not registered yet") && relay_messages(mark).empty());

    // The register reply, logged with its result and size (never its text).
    std::string reply = "{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\",\"note\":\"" + secret + "\"}}";
    lmark = logs.size();
    relay_send(vm_control(reply));
    CHECK(logged(lmark, "mg.session: link.register reply: registered, status registered (" + std::to_string(reply.size())
                            + " bytes"));
    CHECK(!logged(lmark, "unhandled control message"));   // the reply is handled, not a stray
    for (const auto &l : logs) CHECK(l.find(secret) == std::string::npos);

    // session.ping on the open, registered session: exactly one frame, the
    // heartbeat, on /link-control, sealed.
    settle();
    mark = notes.size();
    lmark = logs.size();
    noise_ctrl_ble_ping();
    settle();
    msgs = relay_messages(mark);
    CHECK(msgs.size() == 1 && msgs[0].stream == CTRL_STREAM_ID && msgs[0].kind == ServiceFrameKind::BodyChunk);
    auto json = control_json(msgs);
    CHECK(json.size() == 1 && json[0] == "{\"method\":\"link.heartbeat\"}");
    CHECK(logged_count(lmark, std::string("mg.session: session.ping: sent link.heartbeat (27 bytes) on /link-control")
                                  + ", sealed") == 1);
    size_t sealed_notes = 0;
    for (size_t i = mark; i < notes.size(); i++) {
        const bytes_t &d = notes[i];
        if (d.size() > 7 && d[1] == mg_session_data && (d[2] & mg_session_flag_first)) {
            sealed_notes++;
            // total_len counts the sealed bytes: the plain envelope + 20.
            CHECK(((uint32_t)d[3] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 24)
                  == msgs[0].envelope.size() + MGSEAL_OVERHEAD);
        }
    }
    CHECK(sealed_notes == 1);

    // A ping asked for while a large result is going out waits for it: the
    // result whole, then the heartbeat, each once.
    std::string blob(20000, 'q');
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "blob", blob.c_str());
    cJSON_AddItemToObject(r, "payload", payload);
    noise_ctrl_send_command_result(link.session_generation, "inv-big", r);
    mark = notes.size();
    ble_session_poll(link, seen);   // the result's first chunk only
    noise_ctrl_ble_ping();
    settle();
    json = control_json(relay_messages(mark));
    CHECK(json.size() == 2 && json[0].find("\"inv-big\"") != std::string::npos && json[1] == "{\"method\":\"link.heartbeat\"}");

    // What the server sends back that nothing here handles: logged by type
    // and size, accepted, credited, and the session stays open.
    bytes_t unknown_kind = vm_envelope({0x08, 0x01, 0x7a, 0x03, 'a', 'b', 'c'});   // stream 1, field 15 only
    relay_case("unknown frame kind", unknown_kind,
               "unhandled service frame: unknown frame kind (" + std::to_string(unknown_kind.size()) + " bytes): accepted");
    ApplicationRequestView req;
    req.verb = StringView("POST");
    req.path = StringView("/device/push");
    relay_case("request from the VM", vm_frame(ServiceFrameView{9, ServiceFrameKind::Request, req, {}, {}, {}}),
               "unhandled service frame: a Request frame (");
    // A BodyChunk on a stream nobody runs, shaped like the start of a control
    // message: over Wi-Fi it would go to the control parser and swallow the next invoke.
    bytes_t stray = control_bytes_of("{\"partial\":\"" + secret);
    stray.resize(stray.size() - 3);
    bytes_t chunk5 = vm_chunk_on(5, std::string(stray.begin(), stray.end()));
    relay_case("chunk on stream 5", chunk5, "unhandled BodyChunk frame on stream 5 (" + std::to_string(chunk5.size()) + " bytes)");
    invoke_answered("after-stray");
    ResetView reset{ResetCode::Cancelled, StringView("gone")};
    relay_case("reset on stream 4", vm_frame(ServiceFrameView{4, ServiceFrameKind::Reset, {}, {}, {}, reset}),
               "unhandled Reset frame on stream 4");
    relay_case("refusal on stream 3", vm_response(3, 403, secret, true), "unhandled Response frame on stream 3");
    relay_case("tunnel stream", vm_chunk_on(TUNNEL_STREAM_ID, secret), "unhandled BodyChunk frame on stream 2");
    relay_case("no such request", vm_chunk_on(99, secret), "unhandled BodyChunk frame on stream 99");
    relay_case("reset of no request", vm_frame(ServiceFrameView{77, ServiceFrameKind::Reset, {}, {}, {}, reset}),
               "unhandled Reset frame on stream 77");
    std::string res = "{\"type\":\"res\",\"id\":\"nobody\",\"result\":{\"t\":\"" + secret + "\"}}";
    relay_case("reply nobody waits for", vm_control(res),
               "unhandled control message: type res, method -, event - (" + std::to_string(res.size()) + " bytes): accepted");
    relay_case("unknown event", vm_control("{\"type\":\"event\",\"event\":\"link.something_new\",\"data\":\"" + secret + "\"}"),
               "unhandled control message: type event, method -, event link.something_new (");
    relay_case("unknown method", vm_control("{\"type\":\"req\",\"id\":\"q\",\"method\":\"link.mystery\",\"params\":{\"x\":\"" + secret + "\"}}"),
               "unhandled control message: type req, method link.mystery, event - (");
    relay_case("type with odd characters", vm_control("{\"type\":\"" + secret + " x\"}"),
               "unhandled control message: type ?, method -, event - (");
    // An envelope with a field this gadget doesn't know still carries its frame.
    bytes_t extended = vm_control("{\"method\":\"link.invoke\",\"id\":\"ext\",\"command\":\"display.card\",\"params\":{}}");
    extended.push_back(0x78);   // field 15, varint
    extended.push_back(0x05);
    mark = notes.size();
    relay_send(extended);
    json = control_json(relay_messages(mark));
    CHECK(json.size() == 1 && json[0].find("\"ext\"") != std::string::npos);

    // Malformed: refused, logged, dropped, credited; no crash, and the session goes on.
    {
        // Not an envelope by ServiceCodec's rules: the device session refuses it itself:
        // invalid_value, its credit back, the session open, nothing handed to the Link code.
        size_t nmark = notes.size(), lmark = logs.size();
        size_t wire = relay_send(bytes_t{0xff, 0xff, 0xff, 0xff, 0x0f});
        bool err = false;
        for (size_t i = nmark; i < notes.size(); i++) err = err || notes[i][0] == mg_command_error;
        CHECK(err && link.active && gb_open() && credit_since(nmark) == wire);
        CHECK(!logged(lmark, "mg.session: refused a message that isn't a ServiceResponseEnvelope (5 bytes"));
    }
    relay_case("empty envelope", bytes_t{0x78, 0x05}, "refused a message that isn't a ServiceResponseEnvelope (2 bytes, empty payload)");
    relay_case("truncated frame", vm_envelope({0x08}), "refused a service frame that doesn't decode (");
    relay_case("frame too long for its envelope", vm_envelope({0x08, 0x01, 0x22, 0x7f, 'x'}),
               "refused a service frame that doesn't decode (");
    relay_case("not JSON", vm_control("not json " + secret), "refused a control message that isn't JSON (");
    relay_case("not an object", vm_control("[1,2,\"" + secret + "\"]"), "refused a control message that isn't a JSON object (");
    relay_case("invoke without an id", vm_control("{\"method\":\"link.invoke\",\"command\":\"display.card\"}"),
               "refused a link.invoke without an id (");
    invoke_answered("after-malformed");

    // Random bytes (a fixed seed): never a crash under ASan and UBSan, every
    // byte credited, the session open throughout.
    uint32_t seed = 0x5eed1234;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (uint8_t)(seed >> 16); };
    for (int i = 0; i < 300; i++) {
        bytes_t junk(1 + rnd() % 64);
        for (auto &b : junk) b = rnd();
        if (i % 3 == 1) junk = vm_envelope(junk);   // a valid envelope around a random frame
        // Not a frame on /link-control: those are the control stream's bytes,
        // whose Reset or refusal ends a Wi-Fi session too.
        DecodedServiceResponseEnvelope env;
        DecodedServiceFrame f;
        HeaderView hv[16];
        if (DecodeServiceResponseEnvelopeDetailed(ConstByteSpan(junk.data(), junk.size()), env).ok()
            && DecodeServerServiceFrameDetailed(env.payload, Span<HeaderView>(hv, 16), f).ok()
            && f.stream_id == CTRL_STREAM_ID) {
            continue;
        }
        size_t nmark = notes.size();
        size_t wire = relay_send(junk);
        CHECK(link.active && credit_since(nmark) == wire);
    }
    invoke_answered("after-fuzz");

    // The seal's own refusals (mgcommands.h, Sealed data), each credited
    // back with the session kept open (this receiver keeps it, as the
    // reference does). Unsealed data: insecure, changing nothing.
    bytes_t inv = vm_control("{\"method\":\"link.invoke\",\"id\":\"plain\",\"command\":\"display.card\",\"params\":{}}");
    size_t nmark = notes.size();
    int before = gb_refused();
    client_sealed = false;   // sent as it is, unsealed
    client_send(inv);
    client_sealed = true;
    CHECK(errors_since(nmark, mg_error_code_insecure) == 1 && credit_since(nmark) == inv.size()
          && gb_refused() == before + 1 && control_json(relay_messages(nmark)).empty() && link.active);
    // A replayed counter: invalid_value at the first fragment, the counter not used.
    nmark = notes.size();
    bytes_t replay = client_wire(inv, client_ctr - 1);
    client_send(replay, true, mg_session_flag_sealed);
    CHECK(errors_since(nmark, mg_error_code_invalid_value) == 1 && credit_since(nmark) == replay.size()
          && control_json(relay_messages(nmark)).empty() && link.active);
    invoke_answered("after-replay");
    // A wrong tag (a flipped byte, a message long enough for several
    // fragments): invalid_value at its last fragment, dropped, its credit
    // back, and its counter NOT used: the same counter with a good tag is
    // taken next.
    nmark = notes.size();
    bytes_t big = vm_control("{\"type\":\"res\",\"id\":\"x\",\"pad\":\"" + std::string(1500, 'p') + "\"}");
    bytes_t bad = client_wire(big, client_ctr);
    bad[bad.size() - 1] ^= 0x01;
    client_send(bad, true, mg_session_flag_sealed);
    CHECK(errors_since(nmark, mg_error_code_invalid_value) == 1 && credit_since(nmark) == bad.size() && link.active);
    bad = client_wire(inv, client_ctr);
    bad[10] ^= 0x40;   // the ciphertext
    nmark = notes.size();
    client_send(bad, true, mg_session_flag_sealed);
    CHECK(errors_since(nmark, mg_error_code_invalid_value) == 1 && control_json(relay_messages(nmark)).empty());
    invoke_answered("after-bad-tags");   // the same counter, a good tag: accepted
    // Aborted after its first fragment: its counter is used on both sides,
    // so that counter is refused afterwards and the next one is taken.
    uint32_t aborted = client_ctr++;
    bytes_t cut = client_wire(big, aborted);
    size_t first = client_mtu - 3 - MG_SESSION_FIRST_HEADER;
    bytes_t f = {mg_command_session, mg_session_data, (uint8_t)(mg_session_flag_first | mg_session_flag_sealed), 0, 0, 0, 0};
    put32(&f[3], (uint32_t)cut.size());
    f.insert(f.end(), cut.begin(), cut.begin() + first);
    nmark = notes.size();
    gb_write(f.data(), f.size());
    bytes_t abort_frame = {mg_command_session, mg_session_abort};
    gb_write(abort_frame.data(), abort_frame.size());
    settle();
    CHECK(credit_since(nmark) == first && errors_since(nmark, mg_error_code_invalid_value) == 0 && link.active);
    nmark = notes.size();
    bytes_t again = client_wire(inv, aborted);
    client_send(again, true, mg_session_flag_sealed);
    CHECK(errors_since(nmark, mg_error_code_invalid_value) == 1 && control_json(relay_messages(nmark)).empty());
    invoke_answered("after-abort");   // counter + 1
    // The counters run on across a session's end and a new one under the
    // same hello (they restart only at a sealed hello).
    write(state_frame(mg_session_state_closed, 1));
    CHECK(!link.active);
    mark = notes.size();
    write(state_frame(mg_session_state_open, 2));
    relay_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    msgs = relay_messages(mark);
    CHECK(link.active && !msgs.empty() && std::stoul(msgs[0].verb) > 1);
    relay_send(vm_control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}"));
    invoke_answered("second-session");
    for (const auto &l : logs) CHECK(l.find(secret) == std::string::npos);

    // The control stream's own Reset still ends the session, as over Wi-Fi.
    relay_send(vm_frame(ServiceFrameView{CTRL_STREAM_ID, ServiceFrameKind::Reset, {}, {}, {}, reset}));
    CHECK(!link.active);
    lmark = logs.size();
    noise_ctrl_ble_ping();
    settle();
    CHECK(logged(lmark, "mg.session: session.ping: no session"));
}

static void test_relay() {
    // With internal RAM under the TLS path's DMA reserve, as on the Watcher's
    // BLE-only build: the device session doesn't wait for it, so
    // link.register, results and ping all go.
    dma_low = true;
    run_relay();   // sealed only: no plain session exists
    dma_low = false;
    client_sealed = false;
    if (!failures) printf("PASS gadget relay: sealed\n");
}

// ---- heap: the session's own allocations over open, traffic, close, reopen ----
//
// ASan's allocator count (every malloc in this process) sampled through a
// sealed session with a 64 KiB / 64 KiB client hello, as the Muse app now
// sends: the peak above the connection's baseline, and whether anything stays
// after the session ends (a leak), over many cycles. Each cycle must open,
// register and answer every invoke, so a session that never started can't
// pass on its small footprint.
#if __has_include(<sanitizer/allocator_interface.h>)
#include <sanitizer/allocator_interface.h>
#else  // GCC ships no allocator_interface.h; its libasan has the call
extern "C" size_t __sanitizer_get_current_allocated_bytes(void);
#endif
#include <sanitizer/common_interface_defs.h>
static size_t heap_now() { return __sanitizer_get_current_allocated_bytes(); }

static void heap_cycle(uint32_t window, size_t *peak_out) {
    std::vector<bytes_t>().swap(notes);
    std::vector<std::string>().swap(logs);
    std::vector<bytes_t>().swap(ble_envelopes);
    credited_upto = 0;
    size_t base = heap_now(), peak = base;
    auto sample = [&]() { size_t h = heap_now(); if (h > peak) peak = h; };
    client_ctr = 0;
    bytes_t hello = hello_frame(65536, window);
    hello.push_back(mg_session_hello_flag_sealed);
    write(hello);
    sample();
    write(state_frame(mg_session_state_open, 1));
    sample();
    relay_send(vm_response(CTRL_STREAM_ID, 200, "", false));
    sample();
    CHECK(link.active);   // the hello was accepted and the session opened
    relay_send(vm_control("{\"type\":\"res\",\"id\":\"reg-1\",\"result\":{\"status\":\"registered\"}}"));
    sample();
    CHECK(logged(0, "mg.session: link.register reply: registered"));
    int answered = 0;
    for (int i = 0; i < 40; i++) {
        size_t mark = notes.size();
        std::string id = "\"h-" + std::to_string(i) + "\"";
        relay_send(vm_control("{\"method\":\"link.invoke\",\"id\":" + id
                              + ",\"command\":\"display.card\",\"params\":{}}"));
        relay_send(vm_chunk_on(7, std::string(3000, 'u')));   // unhandled, credited
        sample();
        for (const auto &j : control_json(relay_messages(mark))) answered += j.find(id) != std::string::npos;
        if (credited_upto == notes.size()) { std::vector<bytes_t>().swap(notes); credited_upto = 0; }
        std::vector<std::string>().swap(logs);
        std::vector<bytes_t>().swap(ble_envelopes);
    }
    noise_ctrl_ble_ping();
    settle();
    sample();
    write(state_frame(mg_session_state_closed, 1));
    sample();
    CHECK(answered == 40);
    *peak_out = peak - base;
}

static void test_heap() {
    reference_heartbeat = false;
    client_credits = true;
    for (uint32_t window : {4096u, 65536u}) {
        size_t worst = 0, after = 0;
        for (int cycle = 0; cycle < 12; cycle++) {
            connect(247, true);
            client_sealed = true;
            encrypted = false;
            bytes_t k = unhex(PV_K), ch = unhex(PV_CHALLENGE), dn = unhex(PV_DEVICE_NONCE);
            CHECK(mgseal_keys(&client_keys, k.data(), ch.data() + 2, dn.data(), SV_NODE_ID,
                              strlen(SV_NODE_ID)) == MGSEAL_OK);
            size_t peak = 0;
            heap_cycle(window, &peak);
            worst = peak > worst ? peak : worst;
            gb_disconnect();
            settle();
            // The harness's own records go, so only the firmware's allocations count.
            std::vector<bytes_t>().swap(notes);
            std::vector<std::string>().swap(logs);
            std::vector<bytes_t>().swap(ble_envelopes);
            credited_upto = 0;
            if (cycle == 1) after = heap_now();   // after two cycles: caches warmed
        }
        long grew = (long)heap_now() - (long)after;
        if (getenv("MG_HEAP_PROFILE")) __sanitizer_print_memory_profile(95, 12);
        printf("HEAP sealed window %u: peak %zu bytes above the connection, %ld bytes kept over 10 cycles\n",
               (unsigned)window, worst, grew);
        CHECK(grew <= 4096);        // nothing kept per session (the fake queue's deque may keep one block)
        CHECK(worst < 160 * 1024);  // bounded by the gadget's own limits, not the client's
    }
    client_sealed = false;
    if (!failures) printf("PASS gadget heap\n");
}

// ---- task: the active task ------------------

static int open_requests(size_t from) {
    int n = 0;
    for (size_t i = from; i < notes.size(); i++) {
        n += notes[i].size() == 3 && notes[i][0] == mg_command_session && notes[i][1] == mg_session_open_request
             && notes[i][2] == 1;
    }
    return n;
}
// The gadget's own closes (open_request [0]), from mark.
static int closes(size_t from) {
    int n = 0;
    for (size_t i = from; i < notes.size(); i++) {
        n += notes[i].size() == 3 && notes[i][0] == mg_command_session && notes[i][1] == mg_session_open_request
             && notes[i][2] == 0;
    }
    return n;
}
// Empty /link-control BodyChunks (the keepalive) and new POST /link-control, from mark.
static int keepalives(size_t from) {
    int n = 0;
    for (const auto &m : device_messages(from)) {
        n += m.stream == CTRL_STREAM_ID && m.kind == ServiceFrameKind::BodyChunk && m.body.empty() && !m.end_body;
    }
    return n;
}
static int link_posts(size_t from) {
    int n = 0;
    for (const auto &m : device_messages(from)) n += m.kind == ServiceFrameKind::Request && m.path == "/link-control";
    return n;
}
// Steps the clock 100 ms at a time for ms; renew: a user action each step (the
// task stays on). Returns each ask's time (ms after the start).
static std::vector<int64_t> run_for(int64_t ms, bool renew, std::vector<int64_t> *keeps = nullptr,
                                    std::vector<int64_t> *shut = nullptr) {
    std::vector<int64_t> asks;
    int64_t t0 = s_now;
    for (int64_t t = 0; t <= ms; t += 100) {
        if (renew) noise_ctrl_user_action();
        size_t mark = notes.size();
        settle();
        if (open_requests(mark)) asks.push_back((s_now - t0) / 1000);
        if (keeps && keepalives(mark)) keeps->push_back((s_now - t0) / 1000);
        if (shut && closes(mark)) shut->push_back((s_now - t0) / 1000);
        s_now += 100 * 1000;
    }
    return asks;
}
static std::string list(const std::vector<int64_t> &v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(i ? v[i] - v[i - 1] : v[i]);
    return s;
}

static int logged(size_t from, const char *what) {
    int n = 0;
    for (size_t i = from; i < logs.size(); i++) n += logs[i].find(what) != std::string::npos;
    return n;
}
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "vectors";
    s_command_cb = on_command;
    CHECK(noise_ctrl_ble_start(SV_NODE_ID, "Reference Gadget"));
    CHECK(tasks_created == 1);
    if (!strcmp(mode, "vectors")) {
        test_vectors();
    } else if (!strcmp(mode, "wifi")) {
        test_wifi();
    } else if (!strcmp(mode, "relay")) {
        test_relay();
    } else if (!strcmp(mode, "heap")) {
        test_heap();
    } else if (!strcmp(mode, "reask")) {
        test_reask();
    } else {
        test_extra();
    }
    return failures ? 1 : 0;
}
