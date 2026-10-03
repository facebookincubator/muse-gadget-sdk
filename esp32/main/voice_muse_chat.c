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

// The Voice PE borrows Muse's voice session (components/muse/
// muse_chat_session.cpp) without the rest of Muse. These stand in for the Muse
// settings, state, Wi-Fi and Link hooks it calls: the session always uses
// Link's paired account, Link's Noise host and Link's preferred VM.

#include "voice_muse_chat.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "app.h"
#include "config_store.h"
#include "wifi_mgr.h"

#include "muse_link.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_wifi.h"

// The session task's stack is in PSRAM, where reading flash would crash, so
// what it asks for comes from here rather than NVS.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_linked;
static char s_host[MUSE_HOST_MAX + 1];

void voice_hatch_refresh(void) {
    char host[MUSE_HOST_MAX + 1];
    if (!config_get_str("noise_host", host, sizeof(host))) host[0] = '\0';
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_host, host, sizeof(s_host));
    portEXIT_CRITICAL(&s_lock);
    atomic_store(&s_linked, config_is_provisioned());
}

bool muse_link_hatch_linked(void) {
    return atomic_load(&s_linked);
}

// app_hatch_vm_credentials does its work on a task of its own.
bool muse_link_hatch_vm(const char *want_vm, char *vm_id, size_t id_cap, char *vm_name,
                        size_t name_cap, char **vm_token) {
    return atomic_load(&s_linked) &&
           app_hatch_vm_credentials(want_vm, vm_id, id_cap, vm_name, name_cap, vm_token);
}

bool muse_wifi_connected(void) {
    return wifi_mgr_is_connected();
}

// Empty: the session falls back to its default host.
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]) {
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_host, MUSE_HOST_MAX + 1);
    portEXIT_CRITICAL(&s_lock);
}

// No VM of its own: Link's preferred one.
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]) {
    out[0] = '\0';
}

// No token of its own: Link's account.
void muse_settings_hatch_token(char out[MUSE_TOKEN_MAX + 1]) {
    out[0] = '\0';
}

size_t muse_settings_hatch_token_len(void) {
    return 0;
}

// No screen to show replies on: anything played goes to the speaker.
bool muse_settings_speaker_on(void) {
    return true;
}

// No screen either, so reply text is never shown; Muse's default page.
void muse_state_page(int *cols, int *lines) {
    *cols = 16;
    *lines = 2;
}
