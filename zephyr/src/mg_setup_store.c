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

#include "mg_setup_store.h"
#include "mg_identity.h"
#include "mg_json.h"
#include "mg_setup_crypto.h"
#include "mgcommands.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(mg_setup_store, CONFIG_MG_LOG_LEVEL);

#define TREE     "mg/setup"
#define KEY_DONE TREE "/done"
/* An earlier device session build's updated token pair (not read). */
#define KEY_ATB  TREE "/atb"
#define KEY_RTB  TREE "/rtb"
/* Forget's mark that the offline clips must go (never erased with the set). */
#define KEY_CLIPS TREE "/clips"

#define FLAG_COMPLETE     0x01
#define FLAG_WIFI_SKIPPED 0x02
/* That build's "the pair is in atb/rtb": such a record is not read. */
#define FLAG_LEGACY_TOKENS_B 0x04
#define FLAG_DEVICE_KEY   0x08
/* The set's node id is gadget-1a2b3c, the form the
 * setup offered and derived K with; a record without it is homelink- (from
 * before, or offered without CONFIG_MG_NODE_ID_GADGET). A token update
 * keeps the record's flags, so the id stays with K. */
#define FLAG_GADGET_NODE  0x10
/* A re-setup's mark that the earlier owner's offline clips must go, durable
 * with the new set; cleared once they are erased. */
#define FLAG_CLIPS        0x20

/* The two slots' entries: slot 0 keeps the names a single-slot store used,
 * so a gadget set up by earlier firmware loads unchanged. */
static const char *const KEY_AT[2] = {TREE "/at", TREE "/at1"};
static const char *const KEY_RT[2] = {TREE "/rt", TREE "/rt1"};
static const char *const KEY_K[2] = {TREE "/k", TREE "/k1"};
static const char *const KEY_DK[2] = {TREE "/dk", TREE "/dk1"};
static const char *const KEY_KID[2] = {TREE "/kid", TREE "/kid1"};
static const char *const KEY_VM[2] = {TREE "/vm", TREE "/vm1"};
static const char *const NAME_AT[2] = {"at", "at1"};
static const char *const NAME_RT[2] = {"rt", "rt1"};
static const char *const NAME_K[2] = {"k", "k1"};
static const char *const NAME_DK[2] = {"dk", "dk1"};
static const char *const NAME_KID[2] = {"kid", "kid1"};
static const char *const NAME_VM[2] = {"vm", "vm1"};

/* The commit record, little-endian. */
struct done_rec {
	uint8_t magic[4]; /* "MGS1" */
	uint8_t flags;
	uint8_t slot; /* which slot holds the set (0 in single-slot records) */
	uint8_t at_len[2];
	uint8_t rt_len[2];
	uint8_t k_len[2];
	/* The device key's VM binding (records before it end here: none). */
	uint8_t vm_kind;   /* enum mg_setup_vm_kind */
	uint8_t vm_id_len; /* the vm entry's length, 0 without one */
	/* access_expires_at, unix seconds, little-endian, 0: none (from the
	 * device session's credentials_update; records before it end here). */
	uint8_t exp[8];
};

/* A record written before the VM binding, and one before the expiry. */
#define DONE_V1_LEN 12
#define DONE_V2_LEN 14

static const uint8_t MAGIC[4] = {'M', 'G', 'S', '1'};

/* What load found: each slot's entry lengths (0: absent), and the record. */
static struct {
	size_t at[2], rt[2], k[2], dk[2], kid[2], vm[2];
	size_t atb, rtb;
	size_t done_len, clips_len; /* as listed, 0: not listed */
	bool have_done;
	bool done_unreadable; /* the record is there and its read failed */
	struct done_rec done;
} found;

/* The last load couldn't read the commit record: nothing was chosen or
 * erased, and nothing is committed until a load reads it. */
static bool load_unreadable;

static bool complete;

/* A setup committed (mg_setup_store_commit_device_key(), not a token
 * refresh): a hook for what waits on it (mg_settings.c: settings a central
 * set before setup are stored now, an image staged before it is dropped).
 * Weak, nothing by default. */
__attribute__((weak)) void mg_setup_store_committed(void)
{
}

/* The board hears the setup state at boot and at every change (mg_app.h). */
__attribute__((weak)) void mg_board_setup_state(bool set_up)
{
	(void)set_up;
}

static void set_complete(bool c)
{
	complete = c;
	mg_board_setup_state(c);
}

/* The setup erased (Forget, a reset, an unpair): the device token is gone
 * (no confirm without one). Weak: nothing by default; mg_ota.c ends
 * a pending update prompt and its admissions. */
__attribute__((weak)) void mg_setup_store_erased(void)
{
}

static bool wifi_skipped;
static bool clips_mark; /* KEY_CLIPS is stored */
static uint8_t active; /* the slot the commit record points at, when complete */

static uint16_t get16(const uint8_t b[2])
{
	return (uint16_t)(b[0] | (b[1] << 8));
}

static void put16(uint8_t b[2], size_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static void wipe(void *p, size_t n)
{
	volatile uint8_t *b = p;

	while (n--) {
		*b++ = 0;
	}
}

static int scan_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		   void *param)
{
	ARG_UNUSED(param);
	if (key == NULL) {
		return 0;
	}
	for (int i = 0; i < 2; i++) {
		if (strcmp(key, NAME_AT[i]) == 0) {
			found.at[i] = len;
		} else if (strcmp(key, NAME_RT[i]) == 0) {
			found.rt[i] = len;
		} else if (strcmp(key, NAME_K[i]) == 0) {
			found.k[i] = len;
		} else if (strcmp(key, NAME_DK[i]) == 0) {
			found.dk[i] = len;
		} else if (strcmp(key, NAME_KID[i]) == 0) {
			found.kid[i] = len;
		} else if (strcmp(key, NAME_VM[i]) == 0) {
			found.vm[i] = len;
		}
	}
	if (strcmp(key, "atb") == 0) {
		found.atb = len;
	} else if (strcmp(key, "rtb") == 0) {
		found.rtb = len;
	} else if (strcmp(key, "clips") == 0) {
		clips_mark = true;
		found.clips_len = len;
	} else if (strcmp(key, "done") == 0) {
		bool len_ok = len == sizeof(found.done) || len == DONE_V1_LEN || len == DONE_V2_LEN;

		memset(&found.done, 0, sizeof(found.done));
		found.done_len = len;
		found.have_done = len_ok && read_cb(cb_arg, &found.done, len) == (ssize_t)len;
		/* A read that failed knows nothing: unlike a record that is
		 * absent or malformed, it never erases the slots. */
		found.done_unreadable = len_ok && !found.have_done;
	}
	return 0;
}

static bool record_valid(void)
{
	const struct done_rec *d = &found.done;
	uint8_t i = d->slot;

	return found.have_done && memcmp(d->magic, MAGIC, sizeof(MAGIC)) == 0 &&
	       (d->flags & FLAG_COMPLETE) && !(d->flags & FLAG_LEGACY_TOKENS_B) && i < 2 &&
	       get16(d->k_len) == MG_SETUP_K_LEN && found.k[i] == MG_SETUP_K_LEN &&
	       get16(d->at_len) != 0 && found.at[i] == get16(d->at_len) &&
	       get16(d->rt_len) != 0 && found.rt[i] == get16(d->rt_len) &&
	       (!(d->flags & FLAG_DEVICE_KEY)
			? d->vm_kind == MG_SETUP_VM_KIND_NONE && d->vm_id_len == 0
			: found.dk[i] == MG_SETUP_DEVICE_KEY_LEN &&
				  found.kid[i] == MG_SETUP_KEY_ID_LEN &&
				  d->vm_kind <= MG_SETUP_VM_KIND_ATTESTED &&
				  d->vm_id_len <= MG_SETUP_VM_ID_MAX &&
				  (d->vm_id_len == 0 || found.vm[i] == d->vm_id_len));
}

static bool slot_used(int i)
{
	return found.at[i] || found.rt[i] || found.k[i] || found.dk[i] || found.kid[i] ||
	       found.vm[i];
}

static int delete_keys(const char *const *keys, size_t n)
{
	int first = 0;

	for (size_t j = 0; j < n; j++) {
		int err = settings_delete(keys[j]);

		if (err && err != -ENOENT && !first) {
			first = err;
		}
	}
	return first;
}

static int delete_slot(int i)
{
	const char *const keys[] = {KEY_K[i],  KEY_AT[i],  KEY_RT[i],
				    KEY_DK[i], KEY_KID[i], KEY_VM[i]};
	int first = delete_keys(keys, ARRAY_SIZE(keys));

	found.at[i] = found.rt[i] = found.k[i] = found.dk[i] = found.kid[i] = found.vm[i] = 0;
	return first;
}

static int delete_legacy(void)
{
	const char *const keys[] = {KEY_ATB, KEY_RTB};
	int first = delete_keys(keys, ARRAY_SIZE(keys));

	found.atb = found.rtb = 0;
	return first;
}

/*
 * Whether the listing saw the whole store. settings_load_subtree_direct()
 * returns 0 whatever its backend hit: ZMS ends a listing early, silently,
 * when a link of its list can't be read, and skips an entry whose name
 * can't be read. A listing that misses the record, or a slot entry, would
 * otherwise read as a half-written setup and erase both slots. So before
 * anything is erased, or the gadget is taken for not set up, each entry is
 * looked up again by its name (ZMS finds it by its hash, not through the
 * list): every one must be there with the length the listing saw, or absent
 * from both. A lookup that fails, or disagrees, makes the store unreadable.
 */
static bool listed_whole(void)
{
	const struct {
		const char *key;
		size_t len;
	} e[] = {
		{KEY_AT[0], found.at[0]},   {KEY_AT[1], found.at[1]},   {KEY_RT[0], found.rt[0]},
		{KEY_RT[1], found.rt[1]},   {KEY_K[0], found.k[0]},     {KEY_K[1], found.k[1]},
		{KEY_DK[0], found.dk[0]},   {KEY_DK[1], found.dk[1]},   {KEY_KID[0], found.kid[0]},
		{KEY_KID[1], found.kid[1]}, {KEY_VM[0], found.vm[0]},   {KEY_VM[1], found.vm[1]},
		{KEY_ATB, found.atb},       {KEY_RTB, found.rtb},       {KEY_DONE, found.done_len},
		{KEY_CLIPS, found.clips_len},
	};

	for (size_t i = 0; i < ARRAY_SIZE(e); i++) {
		ssize_t n = settings_get_val_len(e[i].key);

		if (n < 0 || (size_t)n != e[i].len) {
			LOG_ERR("setup store listing incomplete (%s: listed %u B, found %d)", e[i].key,
				(unsigned int)e[i].len, (int)n);
			return false;
		}
	}
	return true;
}

/* Erase nothing and choose nothing: the next load reads the store again,
 * and no commit runs until one does (mg_setup_store_commit()). Fail closed
 * meanwhile: a store that may hold a set is taken for a set-up gadget
 * (mg_setup_store_complete()), with none of the set's tokens, K or key to
 * use: no first-setup window, the set-up rules, and the token proof busy. */
static int load_refused(const char *why)
{
	load_unreadable = true;
	LOG_ERR("couldn't read the setup %s: nothing erased; taken as set up until it reads", why);
	mg_identity_node_refresh();
	mg_board_setup_state(mg_setup_store_complete());
	return -EIO;
}

int mg_setup_store_load(void)
{
	memset(&found, 0, sizeof(found));
	complete = false;
	wifi_skipped = false;
	clips_mark = false;
	int err = settings_load_subtree_direct(TREE, scan_cb, NULL);

	load_unreadable = false;
	if (err) {
		LOG_ERR("setup store listing failed (%d)", err);
		return load_refused("store");
	}
	if (found.done_unreadable) {
		return load_refused("record");
	}
	if (record_valid()) {
		int other = 1 - found.done.slot;

		set_complete(true);
		active = found.done.slot;
		wifi_skipped = (found.done.flags & FLAG_WIFI_SKIPPED) != 0;
		LOG_INF("setup complete (Wi-Fi %s): access %u B, refresh %u B, proof key held%s",
			wifi_skipped ? "skipped" : "set", (unsigned int)found.at[active],
			(unsigned int)found.rt[active],
			(found.done.flags & FLAG_DEVICE_KEY) ? ", device key held" : "");
		if (slot_used(other)) {
			/* A re-provisioning or token update cut short before its
			 * commit, or the old set left behind after one: never part
			 * of the set up one. */
			LOG_WRN("leftover setup entries beside the committed set: erased");
			(void)delete_slot(other);
		}
		if (!(found.done.flags & FLAG_DEVICE_KEY) && (found.dk[active] || found.kid[active])) {
			const char *const keys[] = {KEY_DK[active], KEY_KID[active]};

			(void)delete_keys(keys, ARRAY_SIZE(keys));
			found.dk[active] = found.kid[active] = 0;
		}
		if (found.done.vm_id_len == 0 && found.vm[active]) {
			(void)settings_delete(KEY_VM[active]);
			found.vm[active] = 0;
		}
		if (found.atb || found.rtb) {
			(void)delete_legacy();
		}
		mg_identity_node_refresh();
		LOG_INF("node id %s%s", mg_identity_node_id(),
			(found.done.flags & FLAG_GADGET_NODE) ? "" : " (homelink-: kept until set up again)");
		return 0;
	}
	/* Half-written or absent only if the listing was whole: a partial one
	 * is unreadable, never absent. */
	if (!listed_whole()) {
		return load_refused("store (a partial listing)");
	}
	if (found.have_done || slot_used(0) || slot_used(1) || found.atb || found.rtb) {
		LOG_WRN("half-written setup (record %s, access %u+%u B, refresh %u+%u B, key "
			"%u+%u B): erased",
			found.have_done ? "invalid" : "missing", (unsigned int)found.at[0],
			(unsigned int)found.at[1], (unsigned int)found.rt[0],
			(unsigned int)found.rt[1], (unsigned int)found.k[0], (unsigned int)found.k[1]);
		(void)mg_setup_store_erase();
	} else {
		LOG_INF("not set up: waiting for Muse Link setup");
	}
	mg_identity_node_refresh();
	mg_board_setup_state(complete);
	return 0;
}

bool mg_setup_store_complete(void)
{
	return complete || load_unreadable;
}

bool mg_setup_store_wifi_skipped(void)
{
	return wifi_skipped;
}

bool mg_setup_store_gadget_node(void)
{
	return complete && (found.done.flags & FLAG_GADGET_NODE);
}

int mg_setup_store_erase(void)
{
	int first = settings_delete(KEY_DONE);

	if (first == -ENOENT) {
		first = 0;
	}
	complete = false;
	wifi_skipped = false;
	for (int i = 0; i < 2; i++) {
		int err = delete_slot(i);

		if (err && !first) {
			first = err;
		}
	}
	int err = delete_legacy();

	if (err && !first) {
		first = err;
	}
	memset(&found, 0, sizeof(found));
	if (!first) {
		/* Every entry is gone: nothing unread is left to fail closed for. */
		load_unreadable = false;
	}
	mg_board_setup_state(mg_setup_store_complete());
	mg_identity_node_refresh();
	mg_setup_store_erased();
	if (first) {
		LOG_ERR("erasing the setup store failed (%d)", first);
	}
	return first;
}

static bool tokens_ok(size_t access_len, size_t refresh_len)
{
	return access_len != 0 && refresh_len != 0 && access_len <= MG_SETUP_TOKEN_MAX &&
	       refresh_len <= MG_SETUP_TOKEN_MAX;
}

/*
 * The credentials document the device session hands this set back in
 * (mg_vm_session.c, credentials_json: node_id, the pair, the device key and
 * its VM binding) must fit MG_SESSION_MAX_CREDENTIALS with the longest
 * access_expires_at a later credentials_update may add, as the portable Link
 * setup checks: a set that doesn't could never open a session. Measured with
 * the same writer, under the node id this commit gives the set.
 */
static bool doc_fits(const char *access, size_t access_len, const char *refresh,
		     size_t refresh_len, const struct mg_setup_device_key *key)
{
	static const char *const KINDS[] = {NULL, "svm", "cvm"};
	char dk_b64[48], kid_b64[28], vm_id[MG_SETUP_VM_ID_MAX + 1];
	struct mg_jw w;

	/* The writer takes C strings: each token whole, NUL-terminated. */
	if (memchr(access, '\0', access_len) != NULL || access[access_len] != '\0' ||
	    memchr(refresh, '\0', refresh_len) != NULL || refresh[refresh_len] != '\0') {
		return false;
	}
	mg_jw_begin(&w, NULL, MG_SESSION_MAX_CREDENTIALS + 1);
	mg_jw_str(&w, "node_id", mg_identity_offer_node_id());
	mg_jw_str(&w, "access_token", access);
	mg_jw_str(&w, "refresh_token", refresh);
	if (key != NULL) {
		size_t vl = key->vm_id != NULL ? key->vm_id_len : 0;

		if (mg_b64_encode(key->key, MG_SETUP_DEVICE_KEY_LEN, dk_b64, sizeof(dk_b64)) <= 0 ||
		    mg_b64_encode(key->key_id, MG_SETUP_KEY_ID_LEN, kid_b64, sizeof(kid_b64)) <= 0) {
			return false;
		}
		mg_jw_str(&w, "device_key", dk_b64);
		mg_jw_str(&w, "key_id", kid_b64);
		wipe(dk_b64, sizeof(dk_b64));
		if (vl) {
			memcpy(vm_id, key->vm_id, vl);
			vm_id[vl] = '\0';
			mg_jw_str(&w, "vm_id", vm_id);
		}
		if (key->vm_kind != MG_SETUP_VM_KIND_NONE) {
			mg_jw_str(&w, "vm_kind", KINDS[key->vm_kind]);
		}
	}
	mg_jw_int(&w, "access_expires_at", INT64_MAX); /* the longest it writes */
	return mg_jw_end(&w) >= 0;
}

/*
 * Writes a whole set into a slot and moves the commit record to it. Set up
 * already (again), that is the free slot, and the committed set and its
 * record stay until the record moves; otherwise the store is cleared and
 * slot 0 used. On failure whatever this wrote is erased again.
 */
static int commit_set(const char *access, size_t access_len, const char *refresh,
		      size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN],
		      const struct mg_setup_device_key *key, uint8_t flags, uint64_t exp)
{
	struct done_rec d = {.flags = flags};
	bool again = complete;
	bool dk = key != NULL;
	size_t vm_len = dk && key->vm_id != NULL ? key->vm_id_len : 0;
	struct done_rec old = found.done;
	int slot = again ? 1 - active : 0;
	int err;

	/* From a clean slate: nothing old can be mistaken for part of this. Set
	 * up already, only the free slot is cleared. */
	err = again ? delete_slot(slot) : mg_setup_store_erase();
	err = err ?: settings_save_one(KEY_AT[slot], access, access_len);
	err = err ?: settings_save_one(KEY_RT[slot], refresh, refresh_len);
	err = err ?: settings_save_one(KEY_K[slot], k, MG_SETUP_K_LEN);
	if (dk) {
		d.flags |= FLAG_DEVICE_KEY;
		d.vm_kind = (uint8_t)key->vm_kind;
		d.vm_id_len = (uint8_t)vm_len;
		err = err ?: settings_save_one(KEY_DK[slot], key->key, MG_SETUP_DEVICE_KEY_LEN);
		err = err ?: settings_save_one(KEY_KID[slot], key->key_id, MG_SETUP_KEY_ID_LEN);
		if (vm_len) {
			err = err ?: settings_save_one(KEY_VM[slot], key->vm_id, vm_len);
		}
	} else {
		d.flags &= ~FLAG_DEVICE_KEY;
	}
	if (!err) {
		memcpy(d.magic, MAGIC, sizeof(MAGIC));
		d.slot = (uint8_t)slot;
		put16(d.at_len, access_len);
		put16(d.rt_len, refresh_len);
		put16(d.k_len, MG_SETUP_K_LEN);
		for (int b = 0; b < 8; b++) {
			d.exp[b] = (uint8_t)(exp >> (8 * b));
		}
		/* The commit point: one entry, written atomically. */
		err = settings_save_one(KEY_DONE, &d, sizeof(d));
		if (err && again) {
			/* Whatever the failed write left, the old record names the
			 * old set again. */
			(void)settings_save_one(KEY_DONE, &old, sizeof(old));
		}
	}
	if (err) {
		if (again) {
			(void)delete_slot(slot);
		} else {
			(void)mg_setup_store_erase();
		}
		return err < 0 ? err : -EIO;
	}
	if (again && delete_slot(1 - slot)) {
		/* Not part of the set any more; boot erases it if this didn't. */
		LOG_WRN("couldn't erase the old set's entries yet");
	}
	found.at[slot] = access_len;
	found.rt[slot] = refresh_len;
	found.k[slot] = MG_SETUP_K_LEN;
	found.dk[slot] = dk ? MG_SETUP_DEVICE_KEY_LEN : 0;
	found.kid[slot] = dk ? MG_SETUP_KEY_ID_LEN : 0;
	found.vm[slot] = vm_len;
	found.have_done = true;
	found.done = d;
	set_complete(true);
	active = (uint8_t)slot;
	wifi_skipped = (d.flags & FLAG_WIFI_SKIPPED) != 0;
	mg_identity_node_refresh();
	return 0;
}

int mg_setup_store_commit(const char *access, size_t access_len, const char *refresh,
			  size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN])
{
	return mg_setup_store_commit_device_key(access, access_len, refresh, refresh_len, k, NULL);
}

static bool key_ok(const struct mg_setup_device_key *dk)
{
	return dk == NULL || (dk->key != NULL && dk->key_id != NULL &&
			      (unsigned int)dk->vm_kind <= MG_SETUP_VM_KIND_ATTESTED &&
			      (dk->vm_id == NULL || dk->vm_id_len <= MG_SETUP_VM_ID_MAX));
}

int mg_setup_store_commit_device_key(const char *access, size_t access_len, const char *refresh,
				     size_t refresh_len, const uint8_t k[MG_SETUP_K_LEN],
				     const struct mg_setup_device_key *dk)
{
	bool again;
	int err;

	if (!tokens_ok(access_len, refresh_len) || !key_ok(dk)) {
		return -EINVAL;
	}
	if (!doc_fits(access, access_len, refresh, refresh_len, dk)) {
		LOG_ERR("setup refused: the credentials document would pass %u B",
			(unsigned int)MG_SESSION_MAX_CREDENTIALS);
		return -EMSGSIZE;
	}
	/* The last load couldn't read the record: it may name a set this
	 * commit would erase. Load again; still unreadable, commit nothing. */
	if (load_unreadable && mg_setup_store_load() != 0) {
		LOG_ERR("storing the setup refused: the setup record can't be read");
		return -EIO;
	}
	again = complete;
	err = commit_set(access, access_len, refresh, refresh_len, k, dk,
			 FLAG_COMPLETE | FLAG_WIFI_SKIPPED |
				 (mg_identity_offer_gadget() ? FLAG_GADGET_NODE : 0) |
				 (again && IS_ENABLED(CONFIG_MG_AUDIO_QUEUE) ? FLAG_CLIPS : 0),
			 0);
	if (err) {
		if (again) {
			LOG_ERR("storing the new setup failed (%d): the old one stays", err);
		} else {
			LOG_ERR("storing the setup failed (%d): rolled back", err);
		}
		return err;
	}
	LOG_INF("setup %s (token-only, Wi-Fi skipped): access %u B, refresh %u B%s",
		again ? "replaced" : "stored", (unsigned int)access_len, (unsigned int)refresh_len,
		dk ? ", device key" : "");
	/* A setup (an ownership change), never a token refresh (commit_set is
	 * shared with credentials_update). */
	mg_setup_store_committed();
	if (dk) {
		LOG_INF("device key bound to vm_id %u B, vm_kind %d",
			(unsigned int)(dk->vm_id ? dk->vm_id_len : 0), (int)dk->vm_kind);
	}
	return 0;
}

struct read_one {
	uint8_t *out;
	size_t want;
	ssize_t got;
};

static int read_one_cb(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg,
		       void *param)
{
	struct read_one *r = param;

	if (key == NULL && len == r->want) {
		r->got = read_cb(cb_arg, r->out, r->want);
	}
	return 0;
}

/* Reads the entry name of exactly len bytes into out. */
static int read_entry(const char *name, void *out, size_t len)
{
	struct read_one r = {.out = out, .want = len, .got = -ENOENT};

	(void)settings_load_subtree_direct(name, read_one_cb, &r);
	if (r.got != (ssize_t)len) {
		memset(out, 0, len);
		return -EIO;
	}
	return 0;
}

int mg_setup_store_update_tokens(const char *access, size_t access_len, const char *refresh,
				 size_t refresh_len)
{
	return mg_setup_store_update_tokens_exp(access, access_len, refresh, refresh_len, 0);
}

uint64_t mg_setup_store_expires(void)
{
	uint64_t v = 0;

	if (!complete) {
		return 0;
	}
	for (int b = 7; b >= 0; b--) {
		v = v << 8 | found.done.exp[b];
	}
	return v;
}

int mg_setup_store_update_tokens_exp(const char *access, size_t access_len, const char *refresh,
				     size_t refresh_len, uint64_t exp)
{
	uint8_t k[MG_SETUP_K_LEN], dk[MG_SETUP_DEVICE_KEY_LEN], kid[MG_SETUP_KEY_ID_LEN];
	char vm_id[MG_SETUP_VM_ID_MAX + 1];
	struct mg_setup_device_key key = {.key = dk, .key_id = kid, .vm_id = vm_id};
	bool have_dk = mg_setup_store_has_device_key();
	int err;

	if (!complete) {
		return -ENOENT;
	}
	if (!tokens_ok(access_len, refresh_len)) {
		return -EINVAL;
	}
	/* The new pair goes into the free slot with a copy of the set's K,
	 * device key and VM binding, so one record still names one whole set. */
	err = mg_setup_store_get_k(k);
	err = err ?: (have_dk ? mg_setup_store_get_device_key(dk, kid) : 0);
	err = err ?: (have_dk ? mg_setup_store_get_vm(vm_id, &key.vm_id_len, &key.vm_kind) : 0);
	err = err ?: commit_set(access, access_len, refresh, refresh_len, k,
				have_dk ? &key : NULL, found.done.flags, exp);
	wipe(k, sizeof(k));
	wipe(dk, sizeof(dk));
	wipe(kid, sizeof(kid));
	if (err) {
		LOG_ERR("storing the new token pair failed (%d): the old pair stays", err);
		return err < 0 ? err : -EIO;
	}
	LOG_INF("token pair updated: access %u B, refresh %u B", (unsigned int)access_len,
		(unsigned int)refresh_len);
	return 0;
}

bool mg_setup_store_has_device_key(void)
{
	return complete && (found.done.flags & FLAG_DEVICE_KEY);
}

int mg_setup_store_get_k(uint8_t k[MG_SETUP_K_LEN])
{
	if (!complete) {
		return -ENOENT;
	}
	if (read_entry(KEY_K[active], k, MG_SETUP_K_LEN)) {
		LOG_ERR("couldn't read the proof key");
		return -EIO;
	}
	return 0;
}

int mg_setup_store_get_tokens(char *access, size_t access_cap, size_t *access_len, char *refresh,
			      size_t refresh_cap, size_t *refresh_len)
{
	size_t at = get16(found.done.at_len), rt = get16(found.done.rt_len);

	if (!complete) {
		return -ENOENT;
	}
	if (at >= access_cap || rt >= refresh_cap) {
		return -ENOSPC;
	}
	if (read_entry(KEY_AT[active], access, at) || read_entry(KEY_RT[active], refresh, rt)) {
		memset(access, 0, access_cap);
		memset(refresh, 0, refresh_cap);
		LOG_ERR("couldn't read the tokens");
		return -EIO;
	}
	access[at] = '\0';
	refresh[rt] = '\0';
	*access_len = at;
	*refresh_len = rt;
	return 0;
}

int mg_setup_store_get_device_key(uint8_t device_key[MG_SETUP_DEVICE_KEY_LEN],
				  uint8_t key_id[MG_SETUP_KEY_ID_LEN])
{
	if (!mg_setup_store_has_device_key()) {
		return -ENOENT;
	}
	if (read_entry(KEY_DK[active], device_key, MG_SETUP_DEVICE_KEY_LEN) ||
	    read_entry(KEY_KID[active], key_id, MG_SETUP_KEY_ID_LEN)) {
		memset(device_key, 0, MG_SETUP_DEVICE_KEY_LEN);
		LOG_ERR("couldn't read the device key");
		return -EIO;
	}
	return 0;
}

int mg_setup_store_get_vm(char vm_id[MG_SETUP_VM_ID_MAX + 1], size_t *vm_id_len,
			  enum mg_setup_vm_kind *vm_kind)
{
	size_t n = found.done.vm_id_len;

	if (!mg_setup_store_has_device_key()) {
		return -ENOENT;
	}
	if (n > MG_SETUP_VM_ID_MAX || (n && read_entry(KEY_VM[active], vm_id, n))) {
		LOG_ERR("couldn't read the device key's vm_id");
		return -EIO;
	}
	vm_id[n] = '\0';
	*vm_id_len = n;
	*vm_kind = (enum mg_setup_vm_kind)found.done.vm_kind;
	return 0;
}

int mg_setup_store_mark_clips(void)
{
	static const uint8_t one = 1;
	int err = settings_save_one(KEY_CLIPS, &one, sizeof(one));

	if (err) {
		LOG_ERR("couldn't store the offline clips' mark (%d)", err);
		return err;
	}
	clips_mark = true;
	return 0;
}

bool mg_setup_store_clips_due(void)
{
	return clips_mark || (complete && (found.done.flags & FLAG_CLIPS));
}

int mg_setup_store_clips_gone(void)
{
	int first = 0;

	if (complete && (found.done.flags & FLAG_CLIPS)) {
		struct done_rec d = found.done;

		d.flags &= ~FLAG_CLIPS;
		first = settings_save_one(KEY_DONE, &d, sizeof(d));
		if (!first) {
			found.done = d;
		}
	}
	if (clips_mark) {
		int err = settings_delete(KEY_CLIPS);

		if (err && err != -ENOENT) {
			first = first ?: err;
		} else {
			clips_mark = false;
		}
	}
	if (first) {
		LOG_ERR("couldn't clear the offline clips' mark (%d)", first);
	}
	return first;
}
