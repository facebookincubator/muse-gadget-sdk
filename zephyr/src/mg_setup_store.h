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

#ifndef MG_SETUP_STORE_H_
#define MG_SETUP_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What Muse Link setup leaves on the gadget (protocols/README.md, Gadget
 * setup over Muse Link), in Zephyr settings (ZMS), in one of two slots:
 *
 *   mg/setup/at, at1     the access token, as provisioned
 *   mg/setup/rt, rt1     the refresh token
 *   mg/setup/k, k1       the token proof key K (32 bytes, mgcommands.h)
 *   mg/setup/dk, dk1     the device key (32 bytes) and mg/setup/kid, kid1 its
 *                        key id (16 bytes), when setup provisioned one
 *   mg/setup/vm, vm1     the vm_id the key is bound to, when setup sent one
 *   mg/setup/done        the commit record: magic, flags (setup complete,
 *                        Wi-Fi skipped, device key held, node id gadget-
 *                        for a new setup, else homelink-), which slot holds
 *                        the set, its lengths and the key's vm_kind
 *
 * ZMS writes one entry atomically, not several, so "done" is the commit
 * point: a set is written into a slot first, then "done" naming it. At boot,
 * setup counts as complete only if "done" is valid and its slot's entries
 * exist with the lengths it records; the other slot's entries are leftovers
 * and are erased. Anything else (a first provisioning or a reset cut short
 * by a power loss) is erased, and the gadget is back in setup.
 *
 * Setting up a gadget that is set up already (re-provisioning, Gadget setup
 * v2) writes the new set into the free slot and moves "done" to it: until
 * then the old tokens, K and device key are the gadget's, after it only the
 * new ones (the old slot is erased next, or at boot). A new set without a
 * device key leaves none: no old key survives a re-setup. A first setup uses
 * slot 0, the names a single-slot store used. A token-only setup records
 * Wi-Fi as skipped in the same record, so nothing treats the missing Wi-Fi
 * as a half-written pairing.
 *
 * The device session's credentials_update (mgcommands.h, Device session)
 * replaces the token pair the same way: the new pair, with the set's K and
 * device key copied, goes to the free slot, then "done" moves to it. K stays
 * as setup derived it: the app keeps the K it derived from the token it
 * provisioned.
 *
 * A record from an earlier build of the device session that kept an updated
 * pair in mg/setup/atb and rtb (flag 0x04) is not read: boot erases it, and
 * the gadget is set up again. A record written before the VM binding (12
 * bytes, without vm_kind and the vm_id length) loads as a set without one.
 *
 * Tokens, keys and K are never logged: lengths only.
 */

#define MG_SETUP_TOKEN_MAX 2048
#define MG_SETUP_K_LEN     32
#define MG_SETUP_DEVICE_KEY_LEN 32
#define MG_SETUP_KEY_ID_LEN     16
/* provision_v2's vm_id: 1..64 bytes of printable ASCII. */
#define MG_SETUP_VM_ID_MAX      64

/* The VM the device key is bound to (protocols/README.md, Gadget setup over
 * Muse Link): none sent (the default VM), vm_kind "svm" (PLAIN) or "cvm"
 * (ATTESTED: the session needs the VM's attestation handshake). */
enum mg_setup_vm_kind {
	MG_SETUP_VM_KIND_NONE = 0,
	MG_SETUP_VM_KIND_PLAIN = 1,
	MG_SETUP_VM_KIND_ATTESTED = 2,
};

/* A device key as provision_v2 delivered it, with its VM binding (vm_id
 * NULL or vm_id_len 0: none sent). */
struct mg_setup_device_key {
	const uint8_t *key;    /* MG_SETUP_DEVICE_KEY_LEN bytes */
	const uint8_t *key_id; /* MG_SETUP_KEY_ID_LEN bytes */
	const char *vm_id;
	size_t vm_id_len;
	enum mg_setup_vm_kind vm_kind;
};

/* Reads the commit record and cleans up a half-written one (call after
 * settings_subsys_init()). */
int mg_setup_store_load(void);

/* Setup is complete: the gadget holds tokens and K. Also true while the
 * last load couldn't read the store (fail closed: it may hold a set; the
 * set's tokens, K and key stay unavailable, -ENOENT, until a load reads it
 * or an erase empties the store). */
bool mg_setup_store_complete(void);
bool mg_setup_store_wifi_skipped(void);
/* The stored set's node id is gadget-<6 hex> (the form its setup
 * offered and derived K with); false for homelink- and when not set up. */
bool mg_setup_store_gadget_node(void);

/*
 * Stores a token-only setup: the tokens, K, then the commit record (setup
 * complete, Wi-Fi skipped). 0 once it is all durable; on any failure a
 * negative errno, with whatever this call wrote erased again: a gadget that
 * was set up keeps its old set, one that wasn't is left with nothing.
 */
int mg_setup_store_commit(const char *access, size_t access_len, const char *refresh,
			  size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN]);

/* The same, with the device key and its VM binding when setup provisioned
 * one (NULL for none: a set-up gadget's old key is dropped with the rest of
 * its old set). -EINVAL, with nothing written, for a vm_id longer than
 * MG_SETUP_VM_ID_MAX or an unknown vm_kind. */
int mg_setup_store_commit_device_key(const char *access, size_t access_len, const char *refresh,
				     size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN],
				     const struct mg_setup_device_key *dk);

/*
 * Replaces the token pair of a complete setup in one commit (above); K, the
 * device key and its VM binding stay. 0 once the new pair is durable; otherwise the old
 * set is still the one stored and a negative errno is returned.
 */
int mg_setup_store_update_tokens(const char *access, size_t access_len, const char *refresh,
				 size_t refresh_len);
/* The same with the update's access_expires_at (unix seconds; 0: none),
 * kept in the same commit and handed back by the device session's next
 * credentials. Link setup stores none. */
int mg_setup_store_update_tokens_exp(const char *access, size_t access_len, const char *refresh,
				     size_t refresh_len, uint64_t exp);
/* The stored access_expires_at, 0 for none (or not set up). */
uint64_t mg_setup_store_expires(void);

/* Both tokens, read from storage, each NUL-terminated (lengths without the
 * NUL). -ENOENT without a complete setup, -ENOSPC if they don't fit. */
int mg_setup_store_get_tokens(char *access, size_t access_cap, size_t *access_len, char *refresh,
			      size_t refresh_cap, size_t *refresh_len);

/* Setup provisioned a device key. */
bool mg_setup_store_has_device_key(void);
/* The device key and its key id. -ENOENT without one. */
int mg_setup_store_get_device_key(uint8_t device_key[MG_SETUP_DEVICE_KEY_LEN],
				  uint8_t key_id[MG_SETUP_KEY_ID_LEN]);
/* The device key's VM binding: vm_id NUL-terminated (vm_id_len 0: none sent)
 * and its vm_kind. -ENOENT without a device key, -EIO if unreadable. */
int mg_setup_store_get_vm(char vm_id[MG_SETUP_VM_ID_MAX + 1], size_t *vm_id_len,
			  enum mg_setup_vm_kind *vm_kind);

/* K, read from storage. -ENOENT without a complete setup. */
int mg_setup_store_get_k(uint8_t k[MG_SETUP_K_LEN]);

/* Erases the commit record, then both slots' K, tokens, device key and VM
 * binding. 0, or
 * the first error (the rest is still attempted; boot recovery finishes it). */
int mg_setup_store_erase(void);

/*
 * The offline clips must go when the gadget's owner changes (with
 * CONFIG_MG_AUDIO_QUEUE). A re-setup's commit record carries the mark, so it
 * is durable exactly when the new set is (a failed or abandoned re-setup
 * leaves none); Forget stores it as its own entry, mg/setup/clips, before it
 * erases the setup (mg_setup_store_erase() keeps it). While either is there,
 * the clips are due: erase them, then clear the mark; boot does so first if
 * a power loss came between.
 */
int mg_setup_store_mark_clips(void);
bool mg_setup_store_clips_due(void);
/* Clears both marks (the record keeps its set). 0, or the first error. */
int mg_setup_store_clips_gone(void);

#endif
