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

typedef enum {
    OTA_RESULT_APPLIED,   // written + verified; device will reboot shortly
    OTA_RESULT_SKIPPED,   // OTA disabled or incoming image not newer; not installed
    OTA_RESULT_FAILED,    // download / verification / flash error
} ota_result_t;

typedef struct {
    ota_result_t result;
    const char *detail;          // short human-readable reason / error
    const char *new_version;     // incoming image version (NULL if unknown)
    const char *running_version; // currently running image version
} ota_event_t;

// Reports the outcome of an OTA attempt. May run synchronously when disabled or
// unable to start, otherwise on the OTA task before reboot on success.
// `user` is the pointer passed to ota_start (caller-owned).
typedef void (*ota_status_cb)(const ota_event_t *ev, void *user);

// Whether this build accepts OTA updates (CONFIG_HOMEHUB_OTA_ENABLED).
bool ota_is_enabled(void);

// Download a firmware image from `url` (HTTPS) with the streaming esp_https_ota
// API. The incoming image's embedded descriptor is read first: unless `force`
// is set, the install is skipped when the image's version is not newer than the
// running one. On success the image is verified (SHA-256 + signature, when
// signing is enabled) and the device reboots. Runs on its own task; returns
// immediately. `cb` may be NULL. When disabled, synchronously reports SKIPPED
// without starting a download or task; force cannot override this setting.
void ota_start(const char *url, bool force, ota_status_cb cb, void *user);

// Before an update switches the boot partition, ota_start records the image it
// installed (its partition and ELF SHA-256, in NVS), so the boot that follows
// can tell its own update from an image another tool installed. Whether that
// record names the running image (false when OTA is disabled):
bool ota_verify_recorded_for_running(void);
// Forgets the record, once the boot's health check has ended either way.
void ota_verify_forget(void);

// Whether the running image is a PENDING_VERIFY install by the ESP-Mosaico's
// Vibe Mode firmware, which accepts its own installs: the one image the
// health check leaves alone. False on every other board and build, and for an
// image device.ota recorded.
bool ota_installed_by_vibe_mode(void);
