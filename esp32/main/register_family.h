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

/* link.register's device_family and model_id (protocols/README.md Node
 * family).
 *
 * Every build of this SDK registers as family "gadget" (BLE-only, Wi-Fi +
 * BLE and Wi-Fi only alike); other families are reserved for Muse's own
 * devices. A gadget's new setup takes a gadget-<id> node id
 * (CONFIG_MUSE_GADGET_NODE_ID_PREFIX), but one set up earlier keeps its
 * homelink-<id> (identity.h). model_id is the board's overlay id
 * (CONFIG_MUSE_DEVICE_MODEL_ID, muse-gadget-<board>) in either family; the
 * catalog code stays in the advertised name only.
 * Include after sdkconfig.h. */
#pragma once

#if !defined(CONFIG_MUSE_DEVICE_MODEL_ID)
#define CONFIG_MUSE_DEVICE_MODEL_ID "muse-gadget"
#endif

static inline const char *register_device_family(void)
{
    return "gadget";
}

static inline const char *register_model_id(void)
{
    return CONFIG_MUSE_DEVICE_MODEL_ID;
}

/* metadata.model_name: the
 * board's short product name (CONFIG_HOMEHUB_MODEL_NAME: StopWatch, Watcher,
 * Mosaico, C6, W175), so the agent can name the gadget in hand; "" (not
 * sent) where the board's overlay sets none. */
static inline const char *register_model_name(void)
{
#if defined(CONFIG_HOMEHUB_MODEL_NAME)
    return CONFIG_HOMEHUB_MODEL_NAME;
#else
    return "";
#endif
}
