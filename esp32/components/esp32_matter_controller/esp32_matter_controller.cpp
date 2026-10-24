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

// The Matter controller and its Link commands; see esp32_matter_controller.h.
//
// Matter starts with the first matter.* command (start_task). Tasks:
// commands arrive on the Link session's task; every CHIP call runs on
// the CHIP task (ScheduleWork). One operation runs at a time, in s_op under
// s_lock, with a generation number so that a CHIP callback arriving after the
// watchdog has answered (or the other way round) is dropped.

#include "esp32_matter_controller.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <functional>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_commissioning_window_opener.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_write_command.h>
#include <inet/UDPEndPointImpl.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32EndpointQueueFilter.h>
#include <setup_payload/ManualSetupPayloadParser.h>
#include <setup_payload/QRCodeSetupPayloadParser.h>
#include <json_to_tlv.h>
#include <tlv_to_json.h>

#include "esp32_matter_controller_ipv6.h"
#include "esp32_matter_controller_proto.h"

// On a board with PSRAM, devices/sdkconfig.matter-controller-psram moves
// CHIP's and esp-matter's allocations and static data there. Without it they
// take the internal RAM that Wi-Fi, the Link session and the tunnel need.
#if CONFIG_SPIRAM && !(CONFIG_CHIP_MEM_ALLOC_MODE_EXTERNAL && CONFIG_ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL \
                       && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
#error "Matter on a board with PSRAM: also load devices/sdkconfig.matter-controller-psram"
#endif

// The option has no Kconfig dependencies: a symbol hidden by one is missing
// from the configuration, and the esp-matter rule in idf_component.yml then
// fails the build on every board where it is hidden.
#if CONFIG_NVS_ENCRYPTION
#error "The Matter controller can't be combined with HOMEHUB_NVS_ENCRYPTION: this esp-matter version doesn't support the HMAC-based NVS encryption scheme"
#endif

using namespace esp_matter::controller;
using chip::ScopedNodeId;
using chip::app::ConcreteCommandPath;
using chip::app::ConcreteDataAttributePath;
using chip::app::StatusIB;
using chip::TLV::TLVReader;

static const char *TAG = "matter_controller";

// Matter's own NVS partition (the flash layouts' "matter"), shared with CHIP.
static const char *kPartition = "matter";
// The node list (NVS names are at most 15 characters).
static const char *kNamespace = "matter_ctrl";

// The controller's identity on its own fabric.
static constexpr chip::NodeId kControllerNodeId = 112233;
static constexpr chip::FabricId kFabricId = 1;
static constexpr uint16_t kListenPort = 5580;

// IDs given to devices start here and are never reused (nodes_next_id()).
static constexpr uint64_t kFirstNodeId = 100;
// JSON numbers are exact up to 2^53.
static constexpr uint64_t kMaxNodeId = 1ULL << 53;

// Watchdogs; the advertised timeout_ms are 10 s longer, so the watchdog
// answers first (main/noise_control.cpp).
static constexpr uint32_t kCommissionMs = 150000;
static constexpr uint32_t kCommandMs = 60000;

// matter.commissionables: how long to listen for devices in pairing mode.
static constexpr uint64_t kDiscoverDefaultS = 4;
static constexpr uint64_t kDiscoverMaxS = 15;

// matter.read: values kept, and roughly the bytes of JSON they may take.
static constexpr int kReadMaxValues = 48;
static constexpr size_t kReadMaxBytes = 6144;

// ---- Operation slot --------------------------------------------------------

enum op_kind_t { OP_NONE, OP_COMMISSION, OP_INVOKE, OP_READ, OP_WRITE, OP_REMOVE, OP_WINDOW, OP_DISCOVER };

static const char *op_name(op_kind_t kind) {
    switch (kind) {
    case OP_COMMISSION: return "commission";
    case OP_INVOKE: return "invoke";
    case OP_READ: return "read";
    case OP_WRITE: return "write";
    case OP_REMOVE: return "remove";
    case OP_WINDOW: return "open_window";
    case OP_DISCOVER: return "discover";
    default: return "none";
    }
}

typedef struct {
    op_kind_t kind;
    uint32_t gen;
    uint64_t session;
    char request_id[64];
    uint64_t node;
    int64_t t0_us;
    cJSON *values;        // read: values so far
    size_t values_bytes;
    bool truncated;
    char *json;           // invoke fields, write value: kept until the end
    CHIP_ERROR write_error;
    bool write_answered;  // the device answered the write
    char label[48];
} op_t;

enum state_t { STATE_OFF, STATE_STARTING, STATE_RUNNING, STATE_FAILED };

static esp32_matter_controller_reply_fn s_reply;
static SemaphoreHandle_t s_lock;
static esp_timer_handle_t s_watchdog;
static op_t s_op;
static uint32_t s_gen;
static std::atomic<int> s_state{STATE_OFF};
static char s_start_error[96];

// The start task waits here for the controller to be set up.
static SemaphoreHandle_t s_setup_done;

static double elapsed_s(void) {
    return (esp_timer_get_time() - s_op.t0_us) / 1e6;
}

// Claims the slot for an operation; false when another one runs.
static bool begin(op_kind_t kind, uint64_t session, const char *request_id, uint64_t node, uint32_t timeout_ms,
                  uint32_t *gen) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_op.kind != OP_NONE) {
        xSemaphoreGive(s_lock);
        return false;
    }
    memset(&s_op, 0, sizeof(s_op));
    s_op.kind = kind;
    s_op.gen = ++s_gen;
    s_op.session = session;
    snprintf(s_op.request_id, sizeof(s_op.request_id), "%s", request_id ? request_id : "");
    s_op.node = node;
    s_op.t0_us = esp_timer_get_time();
    *gen = s_op.gen;
    esp_timer_stop(s_watchdog);
    esp_timer_start_once(s_watchdog, (uint64_t)timeout_ms * 1000);
    xSemaphoreGive(s_lock);
    return true;
}

// Answers operation gen, if it is still the current one, and frees the slot.
// Takes ownership of result. Any task.
static void finish(uint32_t gen, cJSON *result) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_op.kind == OP_NONE || s_op.gen != gen) {
        xSemaphoreGive(s_lock);
        cJSON_Delete(result);
        return;
    }
    ESP_LOGI(TAG, "%s node %" PRIu64 ": %s in %.1f s", op_name(s_op.kind), s_op.node,
             cJSON_IsTrue(cJSON_GetObjectItem(result, "ok")) ? "ok" : "failed", elapsed_s());
    uint64_t session = s_op.session;
    char request_id[sizeof(s_op.request_id)];
    memcpy(request_id, s_op.request_id, sizeof(request_id));
    cJSON_Delete(s_op.values);
    free(s_op.json);
    s_op.kind = OP_NONE;
    s_op.values = NULL;
    s_op.json = NULL;
    esp_timer_stop(s_watchdog);
    xSemaphoreGive(s_lock);
    s_reply(session, request_id, result);
}

// The current operation's generation if it is of this kind, else 0.
static uint32_t current(op_kind_t kind) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t gen = s_op.kind == kind ? s_op.gen : 0;
    xSemaphoreGive(s_lock);
    return gen;
}

// The answer for a command whose result comes later through s_reply.
static cJSON *async_result(void) {
    cJSON *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "_async", true);
    return result;
}

static bool schedule(void (*fn)(intptr_t), intptr_t arg) {
    return chip::DeviceLayer::PlatformMgr().ScheduleWork(fn, arg) == CHIP_NO_ERROR;
}

// A node ID for the CHIP task: intptr_t is 32 bits on these chips. The
// work frees it.
static bool schedule_node(void (*fn)(intptr_t), uint64_t node) {
    uint64_t *arg = (uint64_t *)malloc(sizeof(*arg));
    if (!arg) return false;
    *arg = node;
    if (schedule(fn, (intptr_t)arg)) return true;
    free(arg);
    return false;
}

static uint64_t take_node(intptr_t arg) {
    uint64_t node = *(uint64_t *)arg;
    free((void *)arg);
    return node;
}

static void stop_pairing_in_chip_task(intptr_t arg) {
    uint64_t node = take_node(arg);
    auto *commissioner = matter_controller_client::get_instance().get_commissioner();
    if (!commissioner) return;
    LogErrorOnFailure(commissioner->StopPairing((chip::NodeId)node));
    // StopPairing ends the search for the device, but if none was found yet no
    // pairing callback runs, so esp-matter's delegate stays registered and every
    // later commissioning would be refused as "already pairing". Release it here.
    if (commissioner->GetPairingDelegate() == &pairing_command::get_instance()) {
        commissioner->RegisterPairingDelegate(nullptr);
    }
}

static void watchdog_fired(void *arg) {
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t gen = s_op.gen;
    op_kind_t kind = s_op.kind;
    uint64_t node = s_op.node;
    xSemaphoreGive(s_lock);
    if (kind == OP_NONE) return;
    if (kind == OP_COMMISSION) schedule_node(stop_pairing_in_chip_task, node);
    finish(gen, mc_error("timeout", "Matter %s for node %" PRIu64 " did not finish in time (is the device on and "
                                    "reachable?)", op_name(kind), node));
}

static cJSON *chip_error(const char *code, const char *what, CHIP_ERROR err) {
    return mc_error(code, "%s: %s", what, err.AsString());
}

// ---- Storage ---------------------------------------------------------------

static bool nvs_open_nodes(nvs_open_mode_t mode, nvs_handle_t *handle) {
    return nvs_open_from_partition(kPartition, kNamespace, mode, handle) == ESP_OK;
}

static cJSON *load_json(const char *key) {
    nvs_handle_t handle;
    if (!nvs_open_nodes(NVS_READONLY, &handle)) return NULL;
    cJSON *json = NULL;
    size_t len = 0;
    if (nvs_get_blob(handle, key, NULL, &len) == ESP_OK && len > 0) {
        char *buf = (char *)malloc(len + 1);
        if (buf && nvs_get_blob(handle, key, buf, &len) == ESP_OK) {
            buf[len] = '\0';
            json = cJSON_Parse(buf);
            memset(buf, 0, len);
        }
        free(buf);
    }
    nvs_close(handle);
    return json;
}

static bool save_json(const char *key, const cJSON *json) {
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    nvs_handle_t handle;
    bool ok = false;
    if (nvs_open_nodes(NVS_READWRITE, &handle)) {
        esp_err_t err = text ? nvs_set_blob(handle, key, text, strlen(text)) : nvs_erase_key(handle, key);
        ok = (err == ESP_OK || (!text && err == ESP_ERR_NVS_NOT_FOUND)) && nvs_commit(handle) == ESP_OK;
        nvs_close(handle);
    }
    if (text) {
        memset(text, 0, strlen(text));
        cJSON_free(text);
    }
    return ok;
}

static cJSON *nodes_load(void) {
    cJSON *nodes = load_json("nodes");
    if (cJSON_IsArray(nodes)) return nodes;
    cJSON_Delete(nodes);
    return cJSON_CreateArray();
}

static void nodes_put(uint64_t node, const char *label) {
    cJSON *nodes = nodes_load();
    mc_nodes_put(nodes, node, label, CONFIG_HOMEHUB_MATTER_CONTROLLER_MAX_NODES);
    if (!save_json("nodes", nodes)) ESP_LOGE(TAG, "could not save the node list");
    cJSON_Delete(nodes);
}

static void nodes_drop(uint64_t node) {
    cJSON *nodes = nodes_load();
    if (mc_nodes_drop(nodes, node) && !save_json("nodes", nodes)) ESP_LOGE(TAG, "could not save the node list");
    cJSON_Delete(nodes);
}

static bool node_known(uint64_t node) {
    cJSON *nodes = nodes_load();
    bool known = mc_nodes_find(nodes, node) >= 0;
    cJSON_Delete(nodes);
    return known;
}

// Node IDs are never reused: a device's operational DNS-SD name is
// <fabric>-<node ID>, and a Thread border router's SRP server keeps a name
// reserved for its first owner's key lease (days) even after that device is
// removed, so a second device given the same ID couldn't register it and
// would never be found.
static uint64_t nodes_next_id(void) {
    uint32_t next = kFirstNodeId;
    nvs_handle_t handle;
    if (!nvs_open_nodes(NVS_READWRITE, &handle)) return kFirstNodeId + esp_random() % 1000000;
    nvs_get_u32(handle, "next_id", &next);
    cJSON *nodes = nodes_load();
    while (mc_nodes_find(nodes, next) >= 0) next++;
    cJSON_Delete(nodes);
    nvs_set_u32(handle, "next_id", next + 1);
    nvs_commit(handle);
    nvs_close(handle);
    return next;
}

// ---- Commissioning ---------------------------------------------------------

// PASE (the session the setup code secures) is the first step. esp-matter
// reports its failure only here, not as a commissioning failure, and CHIP
// reports it once, after trying every address and device the code's
// discriminator turned up.
static void on_pase(CHIP_ERROR err) {
    ESP_LOGI(TAG, "PASE: %s", err == CHIP_NO_ERROR ? "established" : err.AsString());
    if (err == CHIP_NO_ERROR) return;
    uint32_t gen = current(OP_COMMISSION);
    if (gen) {
        finish(gen, chip_error("commissioning_failed",
                               "the device wasn't found on the network, or refused the code (is it in pairing "
                               "mode? A new device is added with its own app first, then shared)",
                               err));
    }
}

static void on_commission_ok(ScopedNodeId peer) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ours = s_op.kind == OP_COMMISSION && s_op.node == peer.GetNodeId();
    uint32_t gen = s_op.gen;
    double seconds = elapsed_s();
    char label[sizeof(s_op.label)];
    memcpy(label, s_op.label, sizeof(label));
    xSemaphoreGive(s_lock);
    if (!ours) return;
    nodes_put(peer.GetNodeId(), label);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "node_id", (double)peer.GetNodeId());
    if (label[0]) cJSON_AddStringToObject(payload, "label", label);
    cJSON_AddNumberToObject(payload, "seconds", (int)(seconds * 10) / 10.0);
    finish(gen, mc_ok(payload));
}

static void on_commission_fail(ScopedNodeId peer, CHIP_ERROR err, chip::Controller::CommissioningStage stage,
                               std::optional<chip::Credentials::AttestationVerificationResult> attestation) {
    (void)peer;
    uint32_t gen = current(OP_COMMISSION);
    if (!gen) return;
    if (attestation.has_value() && attestation.value() != chip::Credentials::AttestationVerificationResult::kSuccess) {
        // CHIP's AttestationVerificationResult: 1xx the PAA, 2xx the PAI,
        // 3xx the device's certificate, 6xx its certification declaration.
        int code = (int)attestation.value();
        const char *why = code == 101 ? "its manufacturer's PAA is not in this firmware's list (not a certified "
                                        "Matter device, or a manufacturer newer than the list)"
                          : code == 601 ? "its certification declaration isn't signed by the CSA (a test or "
                                          "uncertified device, which this firmware refuses)"
                          : code >= 600 && code < 700 ? "its certification declaration is not valid"
                          : code >= 700 ? "it couldn't be checked (out of memory or an internal error): try again"
                          : code >= 100 && code < 400 ? "its certificates do not check out (expired, revoked or "
                                                        "malformed)"
                                                      : "its attestation is not valid";
        finish(gen, mc_error("attestation_failed", "the device was not added: %s (CHIP result %d)", why, code));
        return;
    }
    finish(gen, mc_error("commissioning_failed", "stage %s: %s", chip::Controller::StageToString(stage),
                         err.AsString()));
}

static void on_unpair(chip::NodeId node, CHIP_ERROR err) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ours = s_op.kind == OP_REMOVE && s_op.node == node;
    uint32_t gen = s_op.gen;
    xSemaphoreGive(s_lock);
    if (!ours) return;
    if (err != CHIP_NO_ERROR) {
        finish(gen, chip_error("remove_failed", "the device did not leave the fabric", err));
        return;
    }
    nodes_drop(node);
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "node_id", (double)node);
    finish(gen, mc_ok(payload));
}

typedef struct {
    uint32_t gen;
    uint64_t node_id;
    char code[MC_CODE_MAX];
} commission_args_t;

static void free_commission_args(commission_args_t *args) {
    memset(args, 0, sizeof(*args));
    free(args);
}

// The code as CHIP reads it: false if it doesn't parse (for a manual code,
// a wrong check digit).
static bool parse_code(const char *code, chip::SetupPayload &payload) {
    CHIP_ERROR err = strncmp(code, "MT:", 3) == 0 ? chip::QRCodeSetupPayloadParser(code).populatePayload(payload)
                                                  : chip::ManualSetupPayloadParser(code).populatePayload(payload);
    return err == CHIP_NO_ERROR;
}

// Hands the pairing to CHIP with esp-matter's pairing delegate (its callbacks
// are the on_* above), or releases the delegate again if CHIP refuses.
static CHIP_ERROR pair(chip::NodeId node, chip::Controller::DeviceCommissioner *commissioner,
                       const std::function<CHIP_ERROR()> &start) {
    if (!commissioner || commissioner->GetPairingDelegate()) return CHIP_ERROR_INCORRECT_STATE;
    commissioner->RegisterPairingDelegate(&pairing_command::get_instance());
    CHIP_ERROR err = start();
    if (err != CHIP_NO_ERROR) commissioner->RegisterPairingDelegate(nullptr);
    return err;
}

// On the network (a code shared from another app): DNS-SD only.
static void commission_in_chip_task(intptr_t arg) {
    commission_args_t *args = (commission_args_t *)arg;
    auto *commissioner = matter_controller_client::get_instance().get_commissioner();
    CHIP_ERROR err = pair(args->node_id, commissioner, [&]() {
        return commissioner->PairDevice(args->node_id, args->code, chip::Controller::CommissioningParameters(),
                                        chip::Controller::DiscoveryType::kDiscoveryNetworkOnly);
    });
    if (err != CHIP_NO_ERROR) {
        finish(args->gen, chip_error("commissioning_failed", "could not start commissioning", err));
    }
    free_commission_args(args);
}

static cJSON *commission_cmd(cJSON *params, const char *request_id, uint64_t session) {
    char code[MC_CODE_MAX];
    chip::SetupPayload payload;
    if (!mc_code_normalize(mc_param_str(params, "code"), code, sizeof(code))) {
        return mc_error("invalid_param", "code must be the QR text (MT:...) or the 11- or 21-digit manual code");
    }
    if (!parse_code(code, payload)) {
        return mc_error("invalid_param", "the code doesn't check out: is a digit or character wrong?");
    }
    const char *label = mc_param_str(params, "label");
    if (label && strlen(label) >= MC_LABEL_MAX) {
        return mc_error("invalid_param", "label is too long");
    }
    // A device beyond the list couldn't be controlled or removed.
    cJSON *nodes = nodes_load();
    int count = cJSON_GetArraySize(nodes);
    cJSON_Delete(nodes);
    if (count >= CONFIG_HOMEHUB_MATTER_CONTROLLER_MAX_NODES) {
        return mc_error("too_many_devices", "this device holds at most %d Matter devices: remove one first",
                        CONFIG_HOMEHUB_MATTER_CONTROLLER_MAX_NODES);
    }

    commission_args_t *args = (commission_args_t *)calloc(1, sizeof(*args));
    if (!args) return mc_error("out_of_memory", "no memory for the request");
    snprintf(args->code, sizeof(args->code), "%s", code);
    args->node_id = nodes_next_id();
    if (!begin(OP_COMMISSION, session, request_id, args->node_id, kCommissionMs, &args->gen)) {
        free_commission_args(args);
        return mc_error("busy", "another Matter operation is running");
    }
    snprintf(s_op.label, sizeof(s_op.label), "%s", label ? label : "");
    ESP_LOGI(TAG, "commissioning node %" PRIu64 " on the network", args->node_id);
    uint32_t gen = args->gen;
    if (!schedule(commission_in_chip_task, (intptr_t)args)) {
        free_commission_args(args);
        finish(gen, mc_error("internal", "could not reach the Matter task"));
    }
    return async_result();
}

// ---- Invoke, read, write ---------------------------------------------------

typedef struct {
    uint32_t gen;
    uint64_t node;
    uint16_t endpoint;
    uint32_t cluster;
    uint32_t id;  // command or attribute
    uint16_t timed_ms;
    const char *json;  // s_op.json
} path_args_t;

// Whether json converts to TLV as esp-matter will convert it. esp-matter
// drops a command whose fields don't convert without calling back, which
// would end as a timeout.
static bool json_converts(const char *json) {
    const size_t len = 1024;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return true;  // let esp-matter try
    chip::TLV::TLVWriter writer;
    writer.Init(buf, len);
    bool ok = esp_matter::json_to_tlv(json, writer, chip::TLV::AnonymousTag()) == ESP_OK;
    free(buf);
    return ok;
}

// node_id, endpoint, cluster and id_key from params; false and an error
// result in *result when one is missing or out of range. Wildcards (all
// endpoints, clusters or attributes) only for reads.
static bool path_params(cJSON *params, const char *id_key, bool wildcards, path_args_t *args, cJSON **result) {
    char err[96];
    uint64_t node, endpoint, cluster, id, timed;
    if (!mc_param_uint(params, "node_id", kMaxNodeId, true, 0, &node, err, sizeof(err))
        || !mc_param_uint(params, "endpoint", 0xFFFF, true, 0, &endpoint, err, sizeof(err))
        || !mc_param_uint(params, "cluster", 0xFFFFFFFF, true, 0, &cluster, err, sizeof(err))
        || !mc_param_uint(params, id_key, 0xFFFFFFFF, true, 0, &id, err, sizeof(err))
        || !mc_param_uint(params, "timed_ms", 0xFFFF, false, 0, &timed, err, sizeof(err))) {
        *result = mc_error("invalid_param", "%s", err);
        return false;
    }
    if (!wildcards && (endpoint == 0xFFFF || cluster == 0xFFFFFFFF || id == 0xFFFFFFFF)) {
        *result = mc_error("invalid_param", "endpoint, cluster and %s must name one path", id_key);
        return false;
    }
    if (!node_known(node)) {
        *result = mc_error("unknown_node", "node %" PRIu64 " is not one of this home's Matter devices (see "
                                           "matter.nodes)", node);
        return false;
    }
    args->node = node;
    args->endpoint = (uint16_t)endpoint;
    args->cluster = (uint32_t)cluster;
    args->id = (uint32_t)id;
    args->timed_ms = (uint16_t)timed;
    return true;
}

static chip::Optional<uint16_t> timed(uint16_t ms) {
    return ms ? chip::MakeOptional(ms) : chip::NullOptional;
}

// Starts an operation on the CHIP task; the args are freed there.
static cJSON *start_path_op(op_kind_t kind, path_args_t *args, char *json, uint64_t session, const char *request_id,
                            void (*on_chip)(intptr_t)) {
    if (!begin(kind, session, request_id, args->node, kCommandMs, &args->gen)) {
        free(args);
        free(json);
        return mc_error("busy", "another Matter operation is running");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_op.json = json;  // freed by finish()
    if (kind == OP_READ) s_op.values = cJSON_CreateArray();
    xSemaphoreGive(s_lock);
    args->json = json;
    uint32_t gen = args->gen;
    if (!schedule(on_chip, (intptr_t)args)) {
        free(args);
        finish(gen, mc_error("internal", "could not reach the Matter task"));
    }
    return async_result();
}

static void invoke_in_chip_task(intptr_t arg) {
    path_args_t *args = (path_args_t *)arg;
    uint32_t gen = args->gen;
    auto on_done = [gen](void *, const ConcreteCommandPath &, const StatusIB &status, TLVReader *data) {
        if (!status.IsSuccess()) {
            finish(gen, chip_error("command_failed", "the device refused the command", status.ToChipError()));
            return;
        }
        cJSON *payload = cJSON_CreateObject();
        cJSON *response = NULL;
        if (data && esp_matter::tlv_to_json(*data, &response) == ESP_OK) {
            cJSON_AddItemToObject(payload, "response", response);
        } else {
            cJSON_Delete(response);
        }
        finish(gen, mc_ok(payload));
    };
    auto on_error = [gen](void *, CHIP_ERROR err) { finish(gen, chip_error("command_failed", "invoke", err)); };
    auto on_unreachable = [gen](void *, const ScopedNodeId &, CHIP_ERROR err) {
        finish(gen, chip_error("unreachable", "could not reach the device", err));
    };
    auto *command = chip::Platform::New<cluster_command>(args->node, args->endpoint, args->cluster, args->id,
                                                         args->json, timed(args->timed_ms), on_done, on_error,
                                                         on_unreachable);
    if (!command || command->send_command() != ESP_OK) {
        finish(gen, mc_error("command_failed", "could not send the command"));
    }
    free(args);
}

static cJSON *invoke_cmd(cJSON *params, const char *request_id, uint64_t session) {
    path_args_t *args = (path_args_t *)calloc(1, sizeof(*args));
    if (!args) return mc_error("out_of_memory", "no memory for the request");
    cJSON *result;
    if (!path_params(params, "command", false, args, &result)) {
        free(args);
        return result;
    }
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(params, "fields");
    if (fields && !cJSON_IsObject(fields)) {
        free(args);
        return mc_error("invalid_param", "fields must be an object keyed \"TAG:TYPE\"");
    }
    char *json = fields ? cJSON_PrintUnformatted(fields) : strdup("{}");
    if (!json) {
        free(args);
        return mc_error("out_of_memory", "no memory for the request");
    }
    if (!json_converts(json)) {
        free(args);
        free(json);
        return mc_error("invalid_param", "fields must be keyed \"TAG:TYPE\" with values of those types, e.g. "
                                         "{\"0:U8\": 128}");
    }
    return start_path_op(OP_INVOKE, args, json, session, request_id, invoke_in_chip_task);
}

static void read_in_chip_task(intptr_t arg) {
    path_args_t *args = (path_args_t *)arg;
    uint32_t gen = args->gen;
    auto on_value = [gen](uint64_t, const ConcreteDataAttributePath &path, TLVReader *data, const StatusIB &status) {
        cJSON *value = cJSON_CreateObject();
        cJSON_AddNumberToObject(value, "endpoint", path.mEndpointId);
        cJSON_AddNumberToObject(value, "cluster", path.mClusterId);
        cJSON_AddNumberToObject(value, "attribute", path.mAttributeId);
        cJSON *json = NULL;
        if (data && esp_matter::tlv_to_json(*data, &json) == ESP_OK) {
            cJSON_AddItemToObject(value, "value", json);
        } else {
            cJSON_Delete(json);
            cJSON_AddStringToObject(value, "error", status.ToChipError().AsString());
        }
        // A wildcard read can return hundreds of values; keep what fits.
        char *text = cJSON_PrintUnformatted(value);
        size_t bytes = text ? strlen(text) : kReadMaxBytes;
        cJSON_free(text);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_op.kind == OP_READ && s_op.gen == gen) {
            if (cJSON_GetArraySize(s_op.values) < kReadMaxValues && s_op.values_bytes + bytes <= kReadMaxBytes) {
                cJSON_AddItemToArray(s_op.values, value);
                s_op.values_bytes += bytes;
                value = NULL;
            } else {
                s_op.truncated = true;
            }
        }
        xSemaphoreGive(s_lock);
        cJSON_Delete(value);
    };
    auto on_done = [gen](uint64_t, const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                         const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON *values = NULL;
        bool truncated = false;
        if (s_op.kind == OP_READ && s_op.gen == gen) {
            values = s_op.values;
            truncated = s_op.truncated;
            s_op.values = NULL;
        }
        xSemaphoreGive(s_lock);
        if (!values) return;
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddItemToObject(payload, "values", values);
        if (truncated) cJSON_AddBoolToObject(payload, "truncated", true);
        finish(gen, mc_ok(payload));
    };
    auto on_unreachable = [gen](void *, const ScopedNodeId &, CHIP_ERROR err) {
        finish(gen, chip_error("unreachable", "could not reach the device", err));
    };
    auto on_error = [gen](uint64_t, CHIP_ERROR err) { finish(gen, chip_error("read_failed", "read", err)); };
    auto *command = chip::Platform::New<read_command>(args->node, args->endpoint, args->cluster, args->id,
                                                      READ_ATTRIBUTE, on_value, on_done, nullptr, on_unreachable,
                                                      on_error);
    if (!command || command->send_command() != ESP_OK) {
        finish(gen, mc_error("read_failed", "could not send the read"));
    }
    free(args);
}

static cJSON *read_cmd(cJSON *params, const char *request_id, uint64_t session) {
    path_args_t *args = (path_args_t *)calloc(1, sizeof(*args));
    if (!args) return mc_error("out_of_memory", "no memory for the request");
    cJSON *result;
    if (!path_params(params, "attribute", true, args, &result)) {
        free(args);
        return result;
    }
    return start_path_op(OP_READ, args, NULL, session, request_id, read_in_chip_task);
}

static void write_in_chip_task(intptr_t arg) {
    path_args_t *args = (path_args_t *)arg;
    uint32_t gen = args->gen;
    // The attribute's status comes to on_success or on_error. esp-matter only
    // logs a failure of the whole request (no answer, a busy device), so a
    // write that ends without either didn't happen.
    auto on_success = [gen](const ConcreteDataAttributePath &) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_op.kind == OP_WRITE && s_op.gen == gen) s_op.write_answered = true;
        xSemaphoreGive(s_lock);
    };
    auto on_error = [gen](const ConcreteDataAttributePath &, CHIP_ERROR err) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_op.kind == OP_WRITE && s_op.gen == gen) {
            s_op.write_error = err;
            s_op.write_answered = true;
        }
        xSemaphoreGive(s_lock);
    };
    auto on_done = [gen](chip::app::WriteClient *) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ours = s_op.kind == OP_WRITE && s_op.gen == gen;
        bool answered = ours && s_op.write_answered;
        CHIP_ERROR err = ours ? s_op.write_error : CHIP_NO_ERROR;
        xSemaphoreGive(s_lock);
        if (!answered) {
            finish(gen, mc_error("write_failed", "the device didn't answer the write"));
        } else if (err != CHIP_NO_ERROR) {
            finish(gen, chip_error("write_failed", "the device refused the write", err));
        } else {
            finish(gen, mc_ok(NULL));
        }
    };
    auto on_unreachable = [gen](void *, const ScopedNodeId &, CHIP_ERROR err) {
        finish(gen, chip_error("unreachable", "could not reach the device", err));
    };
    auto *command = chip::Platform::New<write_command>(args->node, args->endpoint, args->cluster, args->id,
                                                       args->json, timed(args->timed_ms), on_unreachable, on_success,
                                                       on_error, on_done);
    if (!command || command->send_command() != ESP_OK) {
        finish(gen, mc_error("write_failed", "could not send the write"));
    }
    free(args);
}

static cJSON *write_cmd(cJSON *params, const char *request_id, uint64_t session) {
    path_args_t *args = (path_args_t *)calloc(1, sizeof(*args));
    if (!args) return mc_error("out_of_memory", "no memory for the request");
    cJSON *result;
    if (!path_params(params, "attribute", false, args, &result)) {
        free(args);
        return result;
    }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "value");
    if (!cJSON_IsObject(value) || cJSON_GetArraySize(value) != 1) {
        free(args);
        return mc_error("invalid_param", "value must be one item keyed \"0:TYPE\", e.g. {\"0:U8\": 2}");
    }
    char *json = cJSON_PrintUnformatted(value);
    if (!json) {
        free(args);
        return mc_error("out_of_memory", "no memory for the request");
    }
    if (!json_converts(json)) {
        free(args);
        free(json);
        return mc_error("invalid_param", "value must be one item keyed \"0:TYPE\" with a value of that type, e.g. "
                                         "{\"0:U8\": 2}");
    }
    return start_path_op(OP_WRITE, args, json, session, request_id, write_in_chip_task);
}

// ---- Remove, open a window, list --------------------------------------------

static void remove_in_chip_task(intptr_t arg) {
    if (unpair_device((chip::NodeId)take_node(arg)) != ESP_OK) {
        uint32_t gen = current(OP_REMOVE);
        if (gen) finish(gen, mc_error("remove_failed", "could not start removing the device"));
    }
}

static cJSON *remove_cmd(cJSON *params, const char *request_id, uint64_t session) {
    char err[96];
    uint64_t node;
    if (!mc_param_uint(params, "node_id", kMaxNodeId, true, 0, &node, err, sizeof(err))) {
        return mc_error("invalid_param", "%s", err);
    }
    if (!node_known(node)) {
        return mc_error("unknown_node", "node %" PRIu64 " is not one of this home's Matter devices", node);
    }
    if (mc_param_bool(params, "forget", false)) {
        // A device that was reset or is gone can't leave the fabric itself.
        nodes_drop(node);
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddNumberToObject(payload, "node_id", (double)node);
        cJSON_AddBoolToObject(payload, "forgotten", true);
        return mc_ok(payload);
    }
    uint32_t gen;
    if (!begin(OP_REMOVE, session, request_id, node, kCommandMs, &gen)) {
        return mc_error("busy", "another Matter operation is running");
    }
    if (!schedule_node(remove_in_chip_task, node)) {
        finish(gen, mc_error("internal", "could not reach the Matter task"));
    }
    return async_result();
}

// esp-matter's opener reports only the code, not the node: a window whose
// answer came after its watchdog (60 s) could complete the next open_window.
// Unlikely enough (the device answered after a minute) to leave as is.
static void on_window_open(const char *manual_code) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ours = s_op.kind == OP_WINDOW;
    uint32_t gen = s_op.gen;
    uint64_t node = s_op.node;
    xSemaphoreGive(s_lock);
    if (!ours) return;
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "node_id", (double)node);
    cJSON_AddStringToObject(payload, "manual_code", manual_code);
    finish(gen, mc_ok(payload));
}

typedef struct {
    uint64_t node;
    uint16_t timeout_s;
} window_args_t;

static void window_in_chip_task(intptr_t arg) {
    window_args_t *args = (window_args_t *)arg;
    // An enhanced window with a new random passcode, 1000 PBKDF iterations.
    uint16_t discriminator = (uint16_t)(esp_random() & 0x0FFF);
    esp_err_t err = commissioning_window_opener::get_instance().send_open_commissioning_window_command(
        args->node, true, args->timeout_s, 1000, discriminator, 10000);
    if (err != ESP_OK) {
        uint32_t gen = current(OP_WINDOW);
        if (gen) finish(gen, mc_error("window_failed", "could not open a commissioning window"));
    }
    free(args);
}

static cJSON *window_cmd(cJSON *params, const char *request_id, uint64_t session) {
    char err[96];
    uint64_t node, timeout_s;
    if (!mc_param_uint(params, "node_id", kMaxNodeId, true, 0, &node, err, sizeof(err))
        || !mc_param_uint(params, "timeout_s", 900, false, 300, &timeout_s, err, sizeof(err))) {
        return mc_error("invalid_param", "%s", err);
    }
    if (timeout_s < 180) return mc_error("invalid_param", "timeout_s must be 180 to 900");
    if (!node_known(node)) {
        return mc_error("unknown_node", "node %" PRIu64 " is not one of this home's Matter devices", node);
    }
    window_args_t *args = (window_args_t *)calloc(1, sizeof(*args));
    if (!args) return mc_error("out_of_memory", "no memory for the request");
    args->node = node;
    args->timeout_s = (uint16_t)timeout_s;
    uint32_t gen;
    if (!begin(OP_WINDOW, session, request_id, node, kCommandMs, &gen)) {
        free(args);
        return mc_error("busy", "another Matter operation is running");
    }
    if (!schedule(window_in_chip_task, (intptr_t)args)) {
        free(args);
        finish(gen, mc_error("internal", "could not reach the Matter task"));
    }
    return async_result();
}

static cJSON *nodes_cmd(void) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "running", s_state == STATE_RUNNING);
    if (s_state == STATE_FAILED) cJSON_AddStringToObject(payload, "error", s_start_error);
    cJSON_AddItemToObject(payload, "nodes", nodes_load());
    return mc_ok(payload);
}

// ---- Discover --------------------------------------------------------------

// Devices in a commissioning window advertise "_matterc._udp" (DNS-SD): a
// Wi-Fi device directly, a Thread device through the home's border router.
// The commissioner keeps the first CHIP_DEVICE_CONFIG_MAX_DISCOVERED_NODES.
static esp_timer_handle_t s_discover_timer;
static uint32_t s_discover_seconds;  // of the one discovery running (the operation slot)

static void discover_done_in_chip_task(intptr_t arg) {
    uint32_t gen = (uint32_t)arg;
    auto *commissioner = matter_controller_client::get_instance().get_commissioner();
    cJSON *devices = cJSON_CreateArray();
    for (int i = 0; commissioner && i < CHIP_DEVICE_CONFIG_MAX_DISCOVERED_NODES; i++) {
        const chip::Dnssd::CommissionNodeData *node = commissioner->GetDiscoveredDevice(i);
        if (!node) continue;
        cJSON *device = cJSON_CreateObject();
        // How it was found: "network" (DNS-SD); Bluetooth may be added later.
        cJSON_AddStringToObject(device, "via", "network");
        cJSON_AddNumberToObject(device, "vendor_id", node->vendorId);
        cJSON_AddNumberToObject(device, "product_id", node->productId);
        cJSON_AddNumberToObject(device, "discriminator", node->longDiscriminator);
        // 1: the device's own window (its setup code works); 2: one opened by
        // another app, with the code that app shows.
        cJSON_AddNumberToObject(device, "commissioning_mode", node->commissioningMode);
        if (node->deviceType) cJSON_AddNumberToObject(device, "device_type", node->deviceType);
        if (node->deviceName[0]) cJSON_AddStringToObject(device, "name", node->deviceName);
        if (node->pairingHint) cJSON_AddNumberToObject(device, "pairing_hint", node->pairingHint);
        if (node->pairingInstruction[0]) {
            cJSON_AddStringToObject(device, "pairing_instruction", node->pairingInstruction);
        }
        cJSON_AddStringToObject(device, "instance", node->instanceName);
        cJSON_AddItemToArray(devices, device);
    }
    if (commissioner) LogErrorOnFailure(commissioner->StopCommissionableDiscovery());
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "devices", devices);
    finish(gen, mc_ok(payload));
}

static void discover_timer_fired(void *arg) {
    if (!schedule(discover_done_in_chip_task, (intptr_t)arg)) {
        finish((uint32_t)(intptr_t)arg, mc_error("internal", "could not reach the Matter task"));
    }
}

static void discover_in_chip_task(intptr_t arg) {
    uint32_t gen = (uint32_t)arg;
    auto *commissioner = matter_controller_client::get_instance().get_commissioner();
    CHIP_ERROR err = commissioner ? commissioner->DiscoverCommissionableNodes(chip::Dnssd::DiscoveryFilter())
                                  : CHIP_ERROR_INCORRECT_STATE;
    if (err != CHIP_NO_ERROR) {
        finish(gen, chip_error("discover_failed", "could not browse for devices", err));
        return;
    }
    esp_timer_stop(s_discover_timer);
    esp_timer_delete(s_discover_timer);
    const esp_timer_create_args_t timer_args = {.callback = discover_timer_fired, .arg = (void *)(intptr_t)gen,
                                                .name = "matter_discovery"};
    if (esp_timer_create(&timer_args, &s_discover_timer) != ESP_OK
        || esp_timer_start_once(s_discover_timer, (uint64_t)s_discover_seconds * 1000 * 1000) != ESP_OK) {
        finish(gen, mc_error("internal", "could not time the discovery"));
    }
}

static cJSON *discover_cmd(cJSON *params, const char *request_id, uint64_t session) {
    char err[96];
    uint64_t seconds;
    if (!mc_param_uint(params, "timeout_s", kDiscoverMaxS, false, kDiscoverDefaultS, &seconds, err, sizeof(err))) {
        return mc_error("invalid_param", "%s", err);
    }
    if (seconds < 1) seconds = 1;
    uint32_t gen;
    if (!begin(OP_DISCOVER, session, request_id, 0, (uint32_t)(seconds * 1000 + 10000), &gen)) {
        return mc_error("busy", "another Matter operation is running");
    }
    s_discover_seconds = (uint32_t)seconds;
    if (!schedule(discover_in_chip_task, (intptr_t)gen)) {
        finish(gen, mc_error("internal", "could not reach the Matter task"));
    }
    return async_result();
}

// ---- Receive queue guard ---------------------------------------------------

// CHIP posts every UDP datagram it receives as an event, to a queue of
// CONFIG_MAX_EVENT_QUEUE_SIZE, and a full queue is fatal. On a LAN with many
// Matter devices, Matter mDNS comes in bursts (a browse for commissionable
// devices draws an answer from each one in pairing mode). ESP's filter
// (ESP32EndpointQueueFilter) drops only mDNS that isn't Matter's; this one
// applies it, then caps how many datagrams wait. mDNS and Matter's own retries
// cover what is dropped. Only one filter can be installed, and CHIP reinstalls
// its own on every IPv6 address event, so this one goes in after it.
class QueueFilter : public chip::Inet::EndpointQueueFilter {
public:
    static constexpr int kMaxQueuedMdns = 8;
    static constexpr int kMaxQueued = CHIP_DEVICE_CONFIG_MAX_EVENT_QUEUE_SIZE / 2;

    // CHIP puts its own filter back on every IPv6 address event; datagrams
    // queued meanwhile leave through it, so start counting afresh.
    void Reset() {
        mQueued = 0;
        mQueuedMdns = 0;
    }

    void SetHostName(const uint8_t mac[6]) {
        static const char hex[] = "0123456789ABCDEF";
        char name[12];
        for (int i = 0; i < 6; i++) {
            name[2 * i] = hex[mac[i] >> 4];
            name[2 * i + 1] = hex[mac[i] & 0xf];
        }
        mEspFilter = mEsp.SetMdnsHostName(chip::CharSpan(name, sizeof(name))) == CHIP_NO_ERROR;
    }

    FilterOutcome FilterBeforeEnqueue(const void *endpoint, const chip::Inet::IPPacketInfo &info,
                                      const chip::System::PacketBufferHandle &payload) override {
        const bool mdns = info.DestPort == 5353;
        if (mdns && mEspFilter && mEsp.FilterBeforeEnqueue(endpoint, info, payload) == FilterOutcome::kDropPacket) {
            return FilterOutcome::kDropPacket;
        }
        if ((mdns && mQueuedMdns.load() >= kMaxQueuedMdns) || mQueued.load() >= kMaxQueued) {
            return FilterOutcome::kDropPacket;
        }
        mQueued++;
        if (mdns) mQueuedMdns++;
        return FilterOutcome::kAllowPacket;
    }

    FilterOutcome FilterAfterDequeue(const void *, const chip::Inet::IPPacketInfo &info,
                                     const chip::System::PacketBufferHandle &) override {
        Decrement(mQueued);
        if (info.DestPort == 5353) Decrement(mQueuedMdns);
        return FilterOutcome::kAllowPacket;
    }

private:
    // Saturates at 0: datagrams queued before the filter was installed are
    // dequeued through it too.
    static void Decrement(std::atomic<int> &count) {
        int v = count.load();
        while (v > 0 && !count.compare_exchange_weak(v, v - 1)) {
        }
    }

    chip::Inet::ESP32EndpointQueueFilter mEsp;
    bool mEspFilter = false;
    std::atomic<int> mQueued{0};
    std::atomic<int> mQueuedMdns{0};
};

static QueueFilter s_queue_filter;

static void install_queue_filter(void) {
    static bool named;
    uint8_t mac[6];
    if (!named && esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        s_queue_filter.SetHostName(mac);
        named = true;
    }
    s_queue_filter.Reset();
    chip::Inet::UDPEndPointImpl::SetQueueFilter(&s_queue_filter);
}

// ---- Start -----------------------------------------------------------------

static void on_matter_event(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg) {
    (void)arg;
    if (event->Type == chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged) {
        install_queue_filter();  // after CHIP's own, which it reinstalls on IPv6 events
    }
}

static void controller_setup_in_chip_task(intptr_t arg) {
    (void)arg;
    using chip::DeviceLayer::ConnectivityManager;
    // The firmware owns Wi-Fi (pairing, known networks, reconnects); CHIP
    // only watches the station.
    LogErrorOnFailure(chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
        ConnectivityManager::kWiFiStationMode_ApplicationControlled));
    install_queue_filter();
    auto &client = matter_controller_client::get_instance();
    esp_err_t err = client.init(kControllerNodeId, kFabricId, kListenPort);
    if (err == ESP_OK) err = client.setup_commissioner();
    if (err != ESP_OK) {
        snprintf(s_start_error, sizeof(s_start_error), "the commissioner did not start: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_start_error);
    } else {
        pairing_command::get_instance().set_callbacks({on_pase, on_commission_ok, on_commission_fail, on_unpair});
        // These devices are polled through the Link, not watched: no ICD
        // registration (it would give each sleepy device a key to check in with).
        pairing_command::get_instance().set_icd_registration(false);
        commissioning_window_opener::get_instance().set_callback(on_window_open);
        s_state = STATE_RUNNING;
        ESP_LOGI(TAG, "Matter controller running: fabric %llu, node %llu", (unsigned long long)kFabricId,
                 (unsigned long long)kControllerNodeId);
    }
    xSemaphoreGive(s_setup_done);
}

// Matter's NVS partition, the lock and the watchdog: the first matter.*
// command (commands come from one task, the Link session's). Without the
// partition Matter stays off, as after a failed start.
static void init_once(void) {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_setup_done = xSemaphoreCreateBinary();
    const esp_timer_create_args_t args = {.callback = watchdog_fired, .name = "matter_watchdog"};
    if (!s_lock || !s_setup_done || esp_timer_create(&args, &s_watchdog) != ESP_OK) abort();
    esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only Matter's partition: the firmware's own NVS is never erased.
        ESP_LOGW(TAG, "Matter's NVS partition is unreadable (%s); erasing it", esp_err_to_name(err));
        nvs_flash_erase_partition(kPartition);
        err = nvs_flash_init_partition(kPartition);
    }
    if (err != ESP_OK) {
        snprintf(s_start_error, sizeof(s_start_error), "no \"%s\" NVS partition: %s", kPartition,
                 esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_start_error);
        s_state = STATE_FAILED;
    }
}

typedef struct {
    char command[32];
    cJSON *params;
    char request_id[64];
    uint64_t session;
} pending_t;

static cJSON *run_command(const char *command, cJSON *params, const char *request_id, uint64_t session);

// Starts CHIP and the commissioner (about 2 s, mostly the controller's
// certificates), then runs the command that asked for it.
static void start_task(void *arg) {
    pending_t *pending = (pending_t *)arg;
    esp_err_t err = esp_matter::start(on_matter_event);
    if (err != ESP_OK) {
        snprintf(s_start_error, sizeof(s_start_error), "Matter did not start: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_start_error);
    } else {
        esp32_matter_controller_ipv6_start();
        if (schedule(controller_setup_in_chip_task, 0)) xSemaphoreTake(s_setup_done, pdMS_TO_TICKS(30000));
    }
    if (s_state != STATE_RUNNING) s_state = STATE_FAILED;
    if (pending->command[0]) {  // matter.nodes was answered already
        cJSON *result = run_command(pending->command, pending->params, pending->request_id, pending->session);
        if (cJSON_IsTrue(cJSON_GetObjectItem(result, "_async"))) {
            cJSON_Delete(result);
        } else {
            s_reply(pending->session, pending->request_id, result);
        }
    }
    cJSON_Delete(pending->params);
    free(pending);
    vTaskDelete(NULL);
}

// ---- Commands --------------------------------------------------------------

static cJSON *run_command(const char *command, cJSON *params, const char *request_id, uint64_t session) {
    if (s_state != STATE_RUNNING) {
        return mc_error("matter_unavailable", "the Matter controller is not running: %s", s_start_error);
    }
    if (strcmp(command, "matter.commission") == 0) return commission_cmd(params, request_id, session);
    if (strcmp(command, "matter.commissionables") == 0) return discover_cmd(params, request_id, session);
    if (strcmp(command, "matter.invoke") == 0) return invoke_cmd(params, request_id, session);
    if (strcmp(command, "matter.read") == 0) return read_cmd(params, request_id, session);
    if (strcmp(command, "matter.write") == 0) return write_cmd(params, request_id, session);
    if (strcmp(command, "matter.remove") == 0) return remove_cmd(params, request_id, session);
    if (strcmp(command, "matter.open_window") == 0) return window_cmd(params, request_id, session);
    return mc_error("unsupported", "unknown command %s", command);
}

cJSON *esp32_matter_controller_command(const char *command, cJSON *params, const char *request_id,
                                       uint64_t session_generation, esp32_matter_controller_reply_fn reply) {
    s_reply = reply;
    init_once();
    // The device list needs only storage, so it answers while Matter starts.
    bool nodes = strcmp(command, "matter.nodes") == 0;
    if (s_state == STATE_OFF) {
        pending_t *pending = (pending_t *)calloc(1, sizeof(*pending));
        if (!pending || strlen(command) >= sizeof(pending->command)) {
            free(pending);
            return mc_error(pending ? "unsupported" : "out_of_memory", "could not start Matter for %s", command);
        }
        snprintf(pending->command, sizeof(pending->command), "%s", nodes ? "" : command);
        pending->params = params ? cJSON_Duplicate(params, true) : NULL;
        snprintf(pending->request_id, sizeof(pending->request_id), "%s", request_id);
        pending->session = session_generation;
        s_state = STATE_STARTING;
        ESP_LOGI(TAG, "starting Matter");
        if (xTaskCreate(start_task, "matter_start", 6144, pending, 4, NULL) != pdPASS) {
            cJSON_Delete(pending->params);
            free(pending);
            s_state = STATE_OFF;
            return mc_error("out_of_memory", "could not start Matter");
        }
        if (!nodes) return async_result();
    }
    if (nodes) return nodes_cmd();
    if (s_state == STATE_STARTING) {
        return mc_error("busy", "the Matter controller is starting; try again in a few seconds");
    }
    return run_command(command, params, request_id, session_generation);
}

void esp32_matter_controller_erase(void) {
    // CHIP is held off for good (the device restarts next), so nothing it
    // stores lands after the erase.
    if (esp_matter::is_started()) chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = nvs_flash_erase_partition(kPartition);
    ESP_LOGI(TAG, "Matter fabric and devices forgotten: %s", esp_err_to_name(err));
}
