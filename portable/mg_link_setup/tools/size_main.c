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

/*
 * The link root for tools/size.sh: keeps every entry point a firmware
 * calls, so --gc-sections leaves exactly what an integration links.
 */

#include "mg_link_setup.h"
#include "mg_token_proof.h"
#include "mgls_store.h"

void mgls_size_entry(void);

typedef void (*fn)(void);

/* Function pointers through a volatile table: nothing can be dropped. */
static volatile fn keep[] = {
	(fn)mgls_init,     (fn)mgls_connected,  (fn)mgls_disconnected, (fn)mgls_mtu_changed,
	(fn)mgls_rx,       (fn)mgls_poll,       (fn)mgls_idle,         (fn)mgls_press,
	(fn)mgls_dismiss,  (fn)mgls_confirm_gesture, (fn)mgls_press_key,
	(fn)mgls_conn_connected, (fn)mgls_conn_disconnected, (fn)mgls_conn_mtu_changed, (fn)mgls_conn_rx, (fn)mgls_conn,
	(fn)mgls_confirm_armed, (fn)mgls_session_active, (fn)mgls_read_status, (fn)mgls_abort,
	(fn)mgls_adv_flags, (fn)mgls_adv_build, (fn)mgls_format_name,
	(fn)mgtp_init,     (fn)mgtp_reset,      (fn)mgtp_rx,           (fn)mgtp_matched,
#if defined(MGLS_SIZE_STORE)
	(fn)mglss_load,    (fn)mglss_set_up,    (fn)mglss_commit,      (fn)mglss_erase,
	(fn)mglss_get_k,   (fn)mglss_read_token,
#endif
};

void mgls_size_entry(void)
{
	for (unsigned i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
		keep[i]();
	}
}
