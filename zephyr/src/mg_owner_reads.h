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

#ifndef MG_OWNER_READS_H_
#define MG_OWNER_READS_H_

#include <stdbool.h>

/*
 * Only the owner's phone gets the owner's content and the log stream on
 * release builds. On a release build (no automation, no bench mode) of a
 * set-up gadget (it holds a token), reads that return the owner's content
 * answer only a connection whose token proof matched; identity and status
 * reads stay open (request_status, the other get_settings fields, battery,
 * the Nordic UART's `status`); test, bench and automation builds are
 * unchanged.
 *
 * The build check (tools/check_owner_reads.py) holds this list against the
 * code: every MG_OWNER_READ("name") site is listed here and every name here
 * has a site. This app's owner-content reads:
 *   clock    device_action get_unix_time (local time and offset):
 *            proof_required
 *   client   get_settings' client_name / client_app: skipped from the
 *            answer, then proof_required once
 *   log      the Nordic UART log mirror: silent (also mg_nus_log_allowed())
 * (clip_info, read_clip and the queue's counts, GATT and the Nordic UART's
 * `queue`, already wait for the proof on a set-up gadget; capture levels go
 * only to the log.)
 */
#define MG_OWNER_READS "clock client log"

static inline bool mg_owner_read_allowed(bool release, bool set_up, bool proven)
{
	return !release || !set_up || proven;
}

#if defined(CONFIG_ZTEST)
/* A test seam: the unit tests switch the release rule (it starts as the
 * build's). */
extern bool mg_test_owner_release;
#define MG_OWNER_RELEASE() mg_test_owner_release
#else
#define MG_OWNER_RELEASE() (!IS_ENABLED(CONFIG_MG_AUTOMATION) && !IS_ENABLED(CONFIG_MG_BENCH_MODE))
#endif

/* At each owner-content read site: true if this connection may have it. */
#define MG_OWNER_READ(name)                                                                        \
	mg_owner_read_allowed(MG_OWNER_RELEASE(), mg_setup_store_complete(),                     \
			      mg_token_proof_matched())

#endif /* MG_OWNER_READS_H_ */
