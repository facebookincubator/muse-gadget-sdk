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

#ifndef MGLS_CONFIG_H_
#define MGLS_CONFIG_H_

/*
 * Compile-time limits of the portable Muse Link setup core. Override any of
 * them with -D, or put the overrides in a header and build with
 * -DMGLS_USER_CONFIG_FILE='"my_mgls_config.h"'. They size the caller-lent
 * workspace (mgls_workspace_t): nothing here costs permanent RAM.
 */

#ifdef MGLS_USER_CONFIG_FILE
#include MGLS_USER_CONFIG_FILE
#endif

/*
 * The largest decrypted provision_v2 JSON accepted, in bytes. A real phone
 * provisioning measured a 403 B access token and a 1,396 B refresh token;
 * the app's provision_v2 adds up to ~470 B of keys and optional fields
 * (username, api_url, api_url_v2, noise_host, ota_url, fleet_manager_url).
 * 2,560 B leaves room for a ~600 B access token and a ~1.5 KB refresh token
 * with every optional field. A larger message is refused with the encrypted
 * status error_storage and never overflows.
 */
#ifndef MGLS_PROVISION_MAX
#define MGLS_PROVISION_MAX 2560
#endif

/* Per-token limits checked after decryption (error_storage above them). */
#ifndef MGLS_ACCESS_TOKEN_MAX
#define MGLS_ACCESS_TOKEN_MAX 1024
#endif
#ifndef MGLS_REFRESH_TOKEN_MAX
#define MGLS_REFRESH_TOKEN_MAX 2048
#endif

/* The device session hands the pair back in one credentials document of at most this many bytes
 * (MG_SESSION_MAX_CREDENTIALS in mgcommands.h; mg_device_session's MGDS_MAX_CREDENTIALS):
 * {"node_id":"..","access_token":"..","refresh_token":"..","access_expires_at":N}, the last where the board keeps
 * one. A pair whose document would pass it with the longest access_expires_at (20 digits), or with a token byte
 * outside printable ASCII or a quote or backslash, could never be handed back: provision_v2 answers error_storage
 * and keeps the old set, as for a token over its limit. */
#ifndef MGLS_CREDENTIALS_MAX
#define MGLS_CREDENTIALS_MAX 2048
#endif

/* The longest SDK token pairing_confirmed may carry (mgst_... is ~48 chars). */
#ifndef MGLS_SDK_TOKEN_MAX
#define MGLS_SDK_TOKEN_MAX 96
#endif

/* The longest identity string (node_id, device_id, mac, firmware version);
 * "hatch-link:aa:bb:cc:dd:ee:ff" is 28. */
#ifndef MGLS_ID_MAX
#define MGLS_ID_MAX 32
#endif

/* The advertised name's prefix: mgls_format_name() builds
 * MGLS_NAME_PREFIX "<Model>-XXXXXX" (MuseGadget-<PRODUCT>-;
 * the MG- prefix is deferred). */
#ifndef MGLS_NAME_PREFIX
#define MGLS_NAME_PREFIX "MuseGadget-"
#endif

/* Members accepted in one JSON object (provision_v2 has at most 13 today). */
#ifndef MGLS_JSON_MAX_FIELDS
#define MGLS_JSON_MAX_FIELDS 24
#endif

/* Log lines through mgls_config_t.log (never tokens or keys). 0 drops the
 * strings from the image. */
#ifndef MGLS_ENABLE_LOG
#define MGLS_ENABLE_LOG 1
#endif

/* MGLS_ENABLE_SIMULATED_PRESS (default off): compiles in
 * mgls_simulated_press() for bench builds (a console ">pair.confirm"). Never
 * define it in a shipping profile.
 *
 * MGLS_TEST_HOOKS (default off): host-test hooks (a fixed ephemeral key). */

/*
 * MGLS_RELEASE: 1 (the default, and every shipping profile): mgls_init()
 * refuses a config without set_up() and may_prompt() (MGLS_E_CONFIG, every
 * hello refused), so a board that forgot one can't treat a re-setup as a
 * first setup or run without the prompt's rate. 0: a test, bench or
 * automation build, which may leave may_prompt() NULL or name
 * MGLS_HOOK_TEST_DEFAULT(set_up) / MGLS_HOOK_TEST_DEFAULT(may_prompt) to stay
 * hands-free; those names don't exist in a release build. Defaults to 0 with
 * MGLS_ENABLE_SIMULATED_PRESS (a bench build).
 */
#ifndef MGLS_RELEASE
#if defined(MGLS_ENABLE_SIMULATED_PRESS)
#define MGLS_RELEASE 0
#else
#define MGLS_RELEASE 1
#endif
#endif

/* The simulated press is a bench build's: never in a release build. */
#if defined(MGLS_ENABLE_SIMULATED_PRESS) && MGLS_RELEASE
#error "MGLS_ENABLE_SIMULATED_PRESS needs MGLS_RELEASE=0 (a bench build); never ship it"
#endif

#endif
