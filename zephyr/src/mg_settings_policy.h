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

/* When a changed stored setting reaches flash. A set-up gadget saves at once
 * (the token proof gates the change). One that isn't set up takes changes
 * from any central, so they reach flash at most once per
 * MG_SETTINGS_SAVE_GAP_MS, the window starting at boot: a restart (open to
 * anyone before setup) buys no immediate write. Setup's commit and a planned
 * power off flush. Host-tested (test_gadget). */

#ifndef MG_SETTINGS_POLICY_H
#define MG_SETTINGS_POLICY_H

#include <mgcommands.h>

#include <stdbool.h>
#include <stdint.h>

#define MG_SETTINGS_SAVE_GAP_MS (10 * 60 * 1000)

/* last_save_ms: this boot's last write, 0 for none (the boot counts as one). */
static inline bool mg_settings_save_due(bool set_up, int64_t last_save_ms, int64_t now_ms)
{
	return set_up || now_ms - last_save_ms >= MG_SETTINGS_SAVE_GAP_MS;
}

/* Settings that belong to the connection, never stored (mgcommands.h,
 * set_settings): push-to-talk, haptics, link mode and params, client name
 * and app. Every other setting is persisted. */
static inline bool mg_settings_param_per_connection(uint8_t param)
{
	return param == mg_setting_parameter_push_to_talk_enabled ||
	       param == mg_setting_parameter_haptics_enabled ||
	       param == mg_setting_parameter_link_mode ||
	       param == mg_setting_parameter_link_params ||
	       param == mg_setting_parameter_client_name ||
	       param == mg_setting_parameter_client_app;
}

/* On a set-up gadget a central without this connection's token proof
 * changes no persisted setting (mgcommands.h, set_settings): each such pair
 * is skipped, the write's other pairs still apply, then proof_required
 * once. A gadget that isn't set up takes them (saved at most once per
 * MG_SETTINGS_SAVE_GAP_MS). */
static inline bool mg_settings_skip_persisted(bool set_up, bool proven)
{
	return set_up && !proven;
}

#endif /* MG_SETTINGS_POLICY_H */
