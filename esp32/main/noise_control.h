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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

typedef uint64_t noise_ctrl_session_generation_t;

static inline noise_ctrl_session_generation_t
noise_ctrl_next_session_generation(noise_ctrl_session_generation_t current) {
    current++;
    return current == 0 ? 1 : current;
}

static inline bool noise_ctrl_session_is_current(
    noise_ctrl_session_generation_t result_generation,
    noise_ctrl_session_generation_t current_generation) {
    return result_generation != 0 && result_generation == current_generation;
}

// 24 hours, in microseconds.
static const uint64_t NOISE_CTRL_HEARTBEAT_INTERVAL_US = 86400000000ULL;

// Traffic does not affect the heartbeat deadline; a new session starts unsent.
static inline bool noise_ctrl_heartbeat_due(
    bool registered, bool sent, uint64_t now_us, uint64_t last_sent_us) {
    return registered &&
           (!sent || now_us - last_sent_us >= NOISE_CTRL_HEARTBEAT_INTERVAL_US);
}

typedef void (*noise_ctrl_status_cb)(const char *status);

typedef cJSON *(*noise_ctrl_command_cb)(const char *command, cJSON *params,
                                       const char *request_id,
                                       noise_ctrl_session_generation_t session_generation);

void noise_ctrl_init(const char *node_id, const char *display_name,
                     noise_ctrl_status_cb on_status);

void noise_ctrl_set_command_cb(noise_ctrl_command_cb cb);

// Called from the session task with the agent's name (GET /identity) once per
// Noise session, shortly after the device registers.
typedef void (*noise_ctrl_agent_name_cb)(const char *name);

void noise_ctrl_set_agent_name_cb(noise_ctrl_agent_name_cb cb);

// Override the Noise host. NULL or empty restores the compiled default.
void noise_ctrl_set_host(const char *host);

// Off (the default), the session checks for traffic every tick. On, once it
// has been quiet POWER_SAVE_QUIET_MS, every POWER_SAVE_POLL_MS instead, so the
// CPU can sleep between (Muse with its screen off); traffic goes back to every
// tick at once.
void noise_ctrl_set_power_save(bool on);

bool noise_ctrl_connect(const char *vm_id, const char *auth_token,
                        const char *network_ssid);

void noise_ctrl_disconnect(void);

// Starts the last session again, after noise_ctrl_disconnect(): the same VM
// and token, with network_ssid as in noise_ctrl_connect(). False if there was
// none or it couldn't start.
bool noise_ctrl_reconnect(const char *network_ssid);

bool noise_ctrl_is_connected(void);

// From noise_ctrl_connect() until noise_ctrl_disconnect(), connected or not:
// the session reconnects by itself meanwhile.
bool noise_ctrl_is_running(void);

void noise_ctrl_send_command_result(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id, cJSON *result);

// Results of commands that didn't come over this session (the serial console,
// on-device scripts: muse_hw_commands.c) by their generation: the hook takes
// such a result and returns true before it would be queued for the server.
typedef bool (*noise_ctrl_result_hook)(
    noise_ctrl_session_generation_t session_generation,
    const char *request_id, cJSON *result);
void noise_ctrl_set_result_hook(noise_ctrl_result_hook hook);

// ---- Extra daemon requests on this session (Muse builds only) ----
//
// Any task can open an HTTP request to the VM daemon on its own stream of this
// session. A refused or reset request never affects the control session.
//
// The callback runs on the session task and must return quickly. status is the
// HTTP status on the response head (with any inline body), 0 on a later body
// chunk, and -1 once the stream is gone (reset, or the session ended); the
// stream is done after end or -1.
typedef void (*noise_ctrl_req_cb)(void *ctx, int status, const uint8_t *data,
                                  size_t len, bool end);

// headers: name, value pairs ending with NULL (may be NULL). Returns the stream
// id, or 0 if the session is down or busy. Otherwise the callback always ends
// the request with end or -1, even if it could not be sent.
int64_t noise_ctrl_req_open(const char *verb, const char *path,
                            const char *const *headers, bool end_body,
                            noise_ctrl_req_cb cb, void *ctx);
// Queues body bytes (copied). Waits up to wait_ms for room; false if none.
bool noise_ctrl_req_send(int64_t id, const void *data, size_t len,
                         bool end_body, int wait_ms);
// Cancels the request. Until the session task gets to the cancel, the callback
// may still run, even with -1 if the session ends, so ctx must stay valid.
void noise_ctrl_req_cancel(int64_t id);

#ifdef __cplusplus
}
#endif
