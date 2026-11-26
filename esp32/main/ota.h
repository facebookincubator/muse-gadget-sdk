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

typedef enum {
    OTA_RESULT_APPLIED,   // written + verified; device will reboot shortly
    OTA_RESULT_SKIPPED,   // OTA disabled or incoming image not newer; not installed
    OTA_RESULT_FAILED,    // download / verification / flash error
    OTA_RESULT_UNSUPPORTED, // this build takes no Wi-Fi OTA (ota_wifi_refused); nothing downloaded
    OTA_RESULT_REFUSED,   // the image isn't this board's (chip or board description); nothing written
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

// A Muse Gadget (a build with musegadgets BLE audio) updates from the Muse
// apps over Bluetooth (MCUmgr SMP) and refuses every Wi-Fi OTA: Link
// setup's ota_url and Link's BLE device.ota action. An image built for the
// same chip with the dev key would pass both ESP-IDF's chip id and signature
// checks. CONFIG_MUSE_GADGET_WIFI_OTA_SAME_BOARD lets them through for an
// image whose board description names this exact board (checked before
// anything is written). A re-setup of a set-up gadget never takes an ota_url
// either way. Whether this build refuses by policy (false on plain Home Link
// builds and with the opt-in):
bool ota_wifi_refused(void);
// Logs a refusal by policy: who asked (`via`), the version offered when the
// request named one (NULL: unknown, nothing was downloaded) and the URL's
// host and path (ota_url_for_log).
void ota_log_refusal(const char *via, const char *url, const char *offered_version);
// A URL's host and path for a log line: never its userinfo, query or
// fragment (signed URLs carry their tokens there), and path segments that
// look like tokens (32 or more of [A-Za-z0-9_-]) shown as their length.
void ota_url_for_log(const char *url, char *out, size_t cap);

// One update at a time: a Wi-Fi OTA (ota_start, from
// Link setup) and a Bluetooth one (MCUmgr SMP, mg_smp_esp.c)
// both write the other slot, so each claims it first, and the other is
// refused as busy, never queued. A second Wi-Fi OTA is refused the same way;
// the Bluetooth update's own claim succeeds again. Releasing one you don't
// hold does nothing.
typedef enum { OTA_OWNER_NONE = 0, OTA_OWNER_WIFI = 1, OTA_OWNER_BLE = 2 } ota_owner_t;
bool ota_claim(ota_owner_t who);
void ota_release(ota_owner_t who);
ota_owner_t ota_owner(void);

// Download a firmware image from `url` (HTTPS) with the streaming esp_https_ota
// API. The incoming image's embedded descriptor is read first: unless `force`
// is set, the install is skipped when the image's version is not newer than the
// running one. On success the image is verified (SHA-256 + signature, when
// signing is enabled) and the device reboots. Runs on its own task; returns
// immediately. `cb` may be NULL. When disabled, synchronously reports SKIPPED
// without starting a download or task; force cannot override this setting.
// A build that refuses Wi-Fi OTA (ota_wifi_refused) reports UNSUPPORTED the
// same way, and logs it. With the same-board opt-in, the image's first bytes
// (its header and board description) are checked before esp_ota_begin and
// the first write: another chip's or board's image is REFUSED.
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
// image ota_start recorded.
bool ota_installed_by_vibe_mode(void);
