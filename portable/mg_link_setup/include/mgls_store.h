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

#ifndef MGLS_STORE_H_
#define MGLS_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mg_link_setup.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional: the setup set on two raw flash slots, for SDKs with a flash
 * driver but no atomic key-value store. It gives the
 * setup core's commit() its contract: the new set replaces the old one
 * atomically, a failed or interrupted commit leaves the old one, and once a
 * commit is done the old slot is erased (never two token sets).
 *
 * Slot layout (little-endian), each slot one or more erase blocks:
 *
 *   0   u32  seq        commit sequence, higher wins (serial arithmetic)
 *   4   u8   flags      MGLS_CRED_SETUP_COMPLETE | MGLS_CRED_WIFI_SKIPPED
 *   5   u8   version    1
 *   6   u16  access_len
 *   8   u16  refresh_len
 *   10  u16  reserved   0
 *   12  u8   K[32]
 *   44  access token, refresh token
 *   ..  u32  CRC-32 (IEEE) of everything above
 *   C   commit unit: "MGS2" then 0xFF to MGLSS_UNIT, at C = the record
 *       length rounded up to MGLSS_UNIT; written last, in its own unit
 *
 * Writes go out in MGLSS_UNIT-byte units aligned to the slot (padded with
 * 0xFF), so a driver that programs whole words or pages works unchanged;
 * nothing is programmed twice. A slot is valid when its commit unit holds
 * the magic and the CRC matches. mglss_load() keeps the newest valid slot
 * and erases anything else (a torn commit, or an old slot a power loss kept
 * from being erased).
 */

#ifndef MGLSS_UNIT
#define MGLSS_UNIT 32
#endif

#define MGLSS_HDR_LEN 44

typedef struct mglss_flash {
	void *ctx;
	uint32_t slot_addr[2]; /* each aligned to an erase block */
	uint32_t slot_size;    /* bytes per slot, a multiple of the erase block */
	int (*erase)(void *ctx, uint32_t addr, uint32_t len);
	int (*write)(void *ctx, uint32_t addr, const void *data, uint32_t len);
	int (*read)(void *ctx, uint32_t addr, void *data, uint32_t len);
	/* Optional (may be NULL): one line for an event the board should log, never with token bytes. */
	void (*log)(void *ctx, const char *line);
} mglss_flash_t;

typedef struct mglss {
	const mglss_flash_t *f;
	uint32_t seq;
	int8_t cur; /* the valid slot, -1 (none), or -2 (a slot couldn't be read: nothing known) */
	uint8_t flags;
	uint16_t access_len, refresh_len;
} mglss_t;

/* At boot: finds the valid slot, erases torn or superseded ones. 0, -2 if a
 * slot couldn't be read (nothing chosen or erased; the state is then
 * "unreadable" until a load reads both slots), or -1 if an erase failed. */
int mglss_load(mglss_t *st, const mglss_flash_t *f);
/* The setup core's set_up(): true while a set is loaded, and while the state
 * is unreadable (a load, or the reload inside a commit or an erase, got -2):
 * unreadable is not empty, so the gadget fails closed as set up. */
bool mglss_set_up(const mglss_t *st);
/* The setup core's commit(). With no set loaded it loads again first, and
 * a slot still unreadable fails the commit with nothing written (-2: it could
 * hold a set that would win at the next boot). If the old slot's erase fails
 * while its set still stands, the new set is taken back and the commit fails
 * (never two token sets); -2 if that left a slot unreadable. */
int mglss_commit(mglss_t *st, const mgls_credentials_t *cred);
/* Erases both slots (reset, token proof clear, unpair). < 0 if an erase
 * failed: the state is then what the slots still hold (a set that didn't
 * erase stays set up, as at the next boot); -2 if a slot couldn't be read
 * back (unreadable: mglss_set_up() true). */
int mglss_erase(mglss_t *st);
/* K for the token proof: 0, MGTP_NO_TOKEN (1) when not set up, < 0 on error
 * (an unreadable state included). */
int mglss_get_k(const mglss_t *st, uint8_t k[32]);
/* Copies a token out for the firmware's own use (refresh_token: false for
 * the access token). 0 and *len, or < 0 (not set up, too small). */
int mglss_read_token(const mglss_t *st, bool refresh, char *out, size_t cap, size_t *len);

/* The largest record a backend must hold in one piece: 4,095 B keeps it
 * inside a key-value store entry of up to 4,095 B (some stores' reads
 * truncate silently past their limit) and one 4 KiB flash sector. The record, commit unit included, is checked against
 * it at build time. */
#ifndef MGLSS_RECORD_MAX
#define MGLSS_RECORD_MAX 4095
#endif

/* The bytes a slot needs for the largest set (3,168 B with the defaults). */
#define MGLSS_SLOT_NEED                                                                            \
	((((MGLSS_HDR_LEN + MGLS_ACCESS_TOKEN_MAX + MGLS_REFRESH_TOKEN_MAX + 4) + MGLSS_UNIT - 1) /  \
	  MGLSS_UNIT) *                                                                            \
		 MGLSS_UNIT +                                                                      \
	 MGLSS_UNIT)

#ifdef __cplusplus
}
#endif

#endif
