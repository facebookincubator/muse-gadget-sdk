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
 * A test client for the gadget in BabbleSim: scans for MG_SERVICE_UUID,
 * connects, negotiates the MTU, subscribes and runs one scenario
 * (-testid=central_offline, central_setup or central_setup_homelink) while
 * the gadget image runs the matching gadget_* hook. Audio is decoded with the vendored
 * SBC and LC3 decoders and checked for the synthetic 1 kHz tone.
 */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <lc3.h>
#include <sbc.h>

#include "babblekit/testcase.h"
#include "bstests.h"

#include "mg_json.h"
#include "mg_psa.h"
#include "mg_setup.h"
#include "mg_setup_crypto.h"
#include "mg_token_proof.h"
#include "mgcommands.h"
#include "pairing_transcript.h"

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			TEST_FAIL(__VA_ARGS__);                                                    \
			return;                                                                    \
		}                                                                                  \
	} while (0)

/* CHECK for functions that return a value. */
#define CHECK_OR(ret, cond, ...)                                                                   \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			TEST_FAIL(__VA_ARGS__);                                                    \
			return ret;                                                                \
		}                                                                                  \
	} while (0)

enum { CH_CTRL, CH_DATA, CH_NUS, CH_SETUP, CH_SETUP_RX, CH_NUS_RX, CH_N };

struct note {
	uint8_t ch;
	uint16_t len;
	uint8_t d[244];
};

K_MSGQ_DEFINE(rxq, sizeof(struct note), 256, 4);

static struct bt_conn *conn;
static K_SEM_DEFINE(sem_conn, 0, 1);
static K_SEM_DEFINE(sem_disc, 0, 1);
static K_SEM_DEFINE(sem_done, 0, 1);
static K_SEM_DEFINE(sem_gone, 0, 1);
static bt_addr_le_t peer;
static char peer_name[32];
static uint16_t h[CH_N];

static const struct bt_uuid_128 u_svc = BT_UUID_INIT_128(MG_SERVICE_UUID_LE_BYTES);
static const struct bt_uuid_128 u_ctrl = BT_UUID_INIT_128(MG_CONTROL_UUID_LE_BYTES);
static const struct bt_uuid_128 u_data = BT_UUID_INIT_128(MG_DATA_UUID_LE_BYTES);
static const struct bt_uuid_128 u_nus_tx = BT_UUID_INIT_128(MG_NUS_TX_UUID_LE_BYTES);
static const struct bt_uuid_128 u_nus_rx = BT_UUID_INIT_128(MG_NUS_RX_UUID_LE_BYTES);
static const struct bt_uuid_128 u_setup_tx = BT_UUID_INIT_128(MG_SETUP_TX_UUID_LE);
static const struct bt_uuid_128 u_setup_rx = BT_UUID_INIT_128(MG_SETUP_RX_UUID_LE);

/* ---------------------------------------------------------------- scan */

struct adv_seen {
	bool uuid;
	bool setup;
	bool name;
	int link_flags; /* Link setup's manufacturer data flags byte, or -1 */
};

/* Which payloads the scan saw, and whether both came from one address. */
static bool want_both;
static int adv_mg, adv_setup;
static bool adv_other_addr;
static bt_addr_le_t adv_addr;
static int adv_link_flags = -1; /* the last Link setup payload's flags byte */

static bool parse_ad(struct bt_data *data, void *user)
{
	struct adv_seen *s = user;
	static const uint8_t want[] = {MG_SERVICE_UUID_LE_BYTES};
	static const uint8_t setup[] = {MG_SETUP_SERVICE_UUID_LE};

	if (data->type == BT_DATA_UUID128_ALL) {
		for (int i = 0; i + 16 <= data->data_len; i += 16) {
			if (memcmp(&data->data[i], want, 16) == 0) {
				s->uuid = true;
			}
			if (memcmp(&data->data[i], setup, 16) == 0) {
				s->setup = true;
			}
		}
	} else if (data->type == BT_DATA_MANUFACTURER_DATA && data->data_len == 3 &&
		   data->data[0] == 0xff && data->data[1] == 0xff) {
		s->link_flags = data->data[2];
	} else if (data->type == BT_DATA_NAME_COMPLETE) {
		size_t n = MIN(data->data_len, sizeof(peer_name) - 1);

		memcpy(peer_name, data->data, n);
		peer_name[n] = '\0';
		s->name = true;
	}
	return true;
}

static bool seen_uuid;

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct adv_seen s = {.link_flags = -1};

	ARG_UNUSED(rssi);
	bt_data_parse(ad, parse_ad, &s);
	if (s.setup) {
		adv_link_flags = s.link_flags;
	}
	if (s.uuid || s.setup) {
		if (adv_mg + adv_setup == 0) {
			bt_addr_le_copy(&adv_addr, addr);
		} else if (!bt_addr_le_eq(addr, &adv_addr)) {
			adv_other_addr = true;
		}
		adv_mg += s.uuid;
		adv_setup += s.setup;
	}
	if (s.uuid) {
		seen_uuid = true;
		bt_addr_le_copy(&peer, addr);
	}
	/* Connect once the scan response with the name has come in too (and,
	 * when asked, once both payloads have been seen). */
	if (seen_uuid && s.name && type == BT_GAP_ADV_TYPE_SCAN_RSP &&
	    bt_addr_le_eq(addr, &peer) && (!want_both || (adv_mg && adv_setup))) {
		bt_le_scan_stop();
		/* A phone's first link (30-50 ms), not the gadget's fast set: a
		 * link already in that set gets no request, and Zephyr's
		 * controller would complete one without telling either host. */
		int err = bt_conn_le_create(&peer, BT_CONN_LE_CREATE_CONN,
					    BT_LE_CONN_PARAM(24, 40, 0, 400), &conn);
		if (err) {
			TEST_FAIL("create conn failed (%d)", err);
		}
	}
}

static void connected(struct bt_conn *c, uint8_t err)
{
	struct bt_conn_info info;

	if (err) {
		TEST_FAIL("connection failed (0x%02x)", err);
		return;
	}
	if (bt_conn_get_info(c, &info) == 0) {
		TEST_PRINT("connected at %u x 1.25 ms, latency %u, timeout %u",
			   info.le.interval_us / 1250U, info.le.latency, info.le.timeout);
	}
	k_sem_give(&sem_conn);
}

static void disconnected(struct bt_conn *c, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (c == conn) {
		bt_conn_unref(conn);
		conn = NULL;
		k_sem_give(&sem_gone);
	}
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ---------------------------------------------------------------- GATT */

static uint8_t discover_cb(struct bt_conn *c, const struct bt_gatt_attr *attr,
			   struct bt_gatt_discover_params *p)
{
	if (attr == NULL) {
		k_sem_give(&sem_disc);
		return BT_GATT_ITER_STOP;
	}
	const struct bt_gatt_chrc *chrc = attr->user_data;
	uint16_t v = chrc->value_handle;

	if (!bt_uuid_cmp(chrc->uuid, &u_ctrl.uuid)) {
		h[CH_CTRL] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_data.uuid)) {
		h[CH_DATA] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_nus_tx.uuid)) {
		h[CH_NUS] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_nus_rx.uuid)) {
		h[CH_NUS_RX] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_setup_tx.uuid)) {
		h[CH_SETUP] = v;
	} else if (!bt_uuid_cmp(chrc->uuid, &u_setup_rx.uuid)) {
		h[CH_SETUP_RX] = v;
	}
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t notify_cb(struct bt_conn *c, struct bt_gatt_subscribe_params *p, const void *data,
			 uint16_t len)
{
	struct note n;

	if (data == NULL) {
		return BT_GATT_ITER_STOP;
	}
	n.ch = CH_N;
	for (int i = 0; i < CH_N; i++) {
		if (p->value_handle == h[i]) {
			n.ch = i;
		}
	}
	if (n.ch == CH_NUS) {
		return BT_GATT_ITER_CONTINUE; /* the log mirror */
	}
	n.len = MIN(len, sizeof(n.d));
	memcpy(n.d, data, n.len);
	if (k_msgq_put(&rxq, &n, K_NO_WAIT)) {
		TEST_FAIL("rx queue overflow");
	}
	return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params subs[CH_N];
static struct bt_gatt_discover_params sub_disc[CH_N];

static void sub_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_subscribe_params *p)
{
	ARG_UNUSED(c);
	ARG_UNUSED(p);
	if (err) {
		TEST_FAIL("subscribe failed (%u)", err);
	}
	k_sem_give(&sem_done);
}

static int subscribe(int ch)
{
	struct bt_gatt_subscribe_params *p = &subs[ch];

	if (h[ch] == 0) {
		return -ENOENT;
	}
	memset(p, 0, sizeof(*p));
	p->notify = notify_cb;
	p->subscribe = sub_cb;
	p->value = BT_GATT_CCC_NOTIFY;
	p->value_handle = h[ch];
	p->ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
	p->end_handle = 0xffff;
	p->disc_params = &sub_disc[ch];
	int err = bt_gatt_subscribe(conn, p);

	if (err) {
		return err;
	}
	return k_sem_take(&sem_done, K_SECONDS(5));
}

static void mtu_cb(struct bt_conn *c, uint8_t err, struct bt_gatt_exchange_params *p)
{
	ARG_UNUSED(p);
	if (err) {
		TEST_FAIL("MTU exchange failed");
	}
	k_sem_give(&sem_done);
}

static void write_cmd(int ch, const uint8_t *buf, size_t len)
{
	int err = bt_gatt_write_without_response(conn, h[ch], buf, len, false);

	if (err) {
		TEST_FAIL("write failed (%d)", err);
	}
}

/* The bench switch on the Nordic UART: this connection gets mic output
 * without a token proof (the test central holds no proof key). */
static void bench_on(void)
{
	static const char w[] = "bench 1\n";

	write_cmd(CH_NUS_RX, (const uint8_t *)w, sizeof(w) - 1);
	k_sleep(K_MSEC(100));
}

#define CMD(...)                                                                                   \
	do {                                                                                       \
		const uint8_t _b[] = {__VA_ARGS__};                                                \
		write_cmd(CH_CTRL, _b, sizeof(_b));                                                \
	} while (0)

/* Next notification; any channel. */
static bool next(struct note *n, int ms)
{
	return k_msgq_get(&rxq, n, K_MSEC(ms)) == 0;
}

/* Waits for a notification on ch starting with cmd, dropping others. */
static bool expect(int ch, uint8_t cmd, struct note *n, int ms)
{
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		if (next(n, MAX(1, (int)(end - k_uptime_get()))) && n->ch == ch && n->len &&
		    n->d[0] == cmd) {
			return true;
		}
	}
	return false;
}

static void drain(void)
{
	struct note n;

	while (next(&n, 200)) {
	}
}

/* Scan, connect, MTU, discover, subscribe. */
static int connect_and_setup(void)
{
	static struct bt_gatt_exchange_params mtu;
	static struct bt_gatt_discover_params disc;
	int err;

	seen_uuid = false;
	adv_mg = adv_setup = 0;
	adv_other_addr = false;
	adv_link_flags = -1;
	memset(h, 0, sizeof(h));
	/* Every report, not just the first per address: until it is set up, the
	 * gadget's payload takes turns between Link setup and mg. */
	err = bt_le_scan_start(BT_LE_SCAN_PARAM(BT_LE_SCAN_TYPE_ACTIVE, BT_LE_SCAN_OPT_NONE,
						BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW),
			       device_found);
	if (err) {
		return err;
	}
	if (k_sem_take(&sem_conn, K_SECONDS(10))) {
		return -ETIMEDOUT;
	}
	mtu.func = mtu_cb;
	err = bt_gatt_exchange_mtu(conn, &mtu);
	if (err || k_sem_take(&sem_done, K_SECONDS(5))) {
		return -EIO;
	}
	disc.uuid = NULL;
	disc.func = discover_cb;
	disc.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc.type = BT_GATT_DISCOVER_CHARACTERISTIC;
	err = bt_gatt_discover(conn, &disc);
	if (err || k_sem_take(&sem_disc, K_SECONDS(10))) {
		return -EIO;
	}
	if (!h[CH_CTRL] || !h[CH_DATA] || !h[CH_NUS] || !h[CH_SETUP] || !h[CH_SETUP_RX]) {
		return -ENOENT;
	}
	err = subscribe(CH_CTRL);
	err = err ?: subscribe(CH_DATA);
	err = err ?: subscribe(CH_NUS);
	err = err ?: subscribe(CH_SETUP);
	TEST_PRINT("connected to %s, ATT MTU %u", peer_name, bt_gatt_get_mtu(conn));
	return err;
}

static void disconnect(void)
{
	bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	k_sem_take(&sem_gone, K_SECONDS(5));
}

/* --------------------------------------------------------------- audio */

static int16_t pcm[16000 * 3];
static int npcm;
static int dec_rate = 16000; /* the decoders' rate: 16000, or 24000 for LC3 */
static sbc_t sbc_dec;
static LC3_DECODER_MEM_T(10000, 24000) lc3_mem;
static lc3_decoder_t lc3_dec;

#define MIN_RMS 300 /* about -41 dBFS */

/* Share of the decoded energy in the synthetic source's 1 kHz tone; 0 when
 * the capture is quieter than MIN_RMS. */
static double tone_ratio(void)
{
	double total = 0, re = 0, im = 0;
	int start = dec_rate / 20; /* skip the first 50 ms */

	for (int i = start; i < npcm; i++) {
		double ph = 2 * 3.14159265358979 * 1000.0 * i / dec_rate;

		total += (double)pcm[i] * pcm[i];
		re += pcm[i] * cos(ph);
		im += pcm[i] * sin(ph);
	}
	int n = npcm - start;

	if (n <= 0 || total == 0) {
		return 0;
	}
	/* The capture must carry sound, not valid-codec silence: an RMS
	 * floor (the synthetic tone is 1250-2500 peak, RMS 880-1770). */
	double rms = sqrt(total / n);

	if (rms < MIN_RMS) {
		TEST_PRINT("capture RMS %.0f below %d: silence", rms, MIN_RMS);
		return 0;
	}
	/* Power of the 1 kHz component: 2|X|^2/N against sum x^2. */
	return 2 * (re * re + im * im) / n / total;
}

static bool decode_sbc(const uint8_t *d, size_t len, int *frames)
{
	struct sbc_frame f;

	for (size_t off = 0; off < len;) {
		if (sbc_probe(&d[off], &f) || f.msbc || f.freq != SBC_FREQ_16K ||
		    f.mode != SBC_MODE_MONO || f.nblocks != 16 || f.nsubbands != 8) {
			return false;
		}
		unsigned int sz = sbc_get_frame_size(&f);

		if (off + sz > len || npcm + 128 > (int)ARRAY_SIZE(pcm) ||
		    sbc_decode(&sbc_dec, &d[off], sz, &f, &pcm[npcm], 1, NULL, 0)) {
			return false;
		}
		npcm += 128;
		off += sz;
		(*frames)++;
	}
	return true;
}

static bool decode_lc3(const uint8_t *d, size_t len, int frame_bytes, int *frames)
{
	if (len % frame_bytes) {
		return false;
	}
	int ns = dec_rate / 100;

	for (size_t off = 0; off < len; off += frame_bytes) {
		if (npcm + ns > (int)ARRAY_SIZE(pcm) ||
		    lc3_decode(lc3_dec, &d[off], frame_bytes, LC3_PCM_FORMAT_S16, &pcm[npcm], 1)) {
			return false;
		}
		npcm += ns;
		(*frames)++;
	}
	return true;
}

static void reset_decoders_at(int rate)
{
	npcm = 0;
	dec_rate = rate;
	sbc_reset(&sbc_dec);
	lc3_dec = lc3_setup_decoder(10000, rate, 0, &lc3_mem);
}

static void reset_decoders(void)
{
	reset_decoders_at(16000);
}

/* --------------------------------------------------------------- tests */

static uint16_t err_code(const struct note *n)
{
	return n->len >= 5 ? sys_get_le16(&n->d[3]) : 0;
}

/* A clip decodes by its own clip_info format. */
static bool read_clip(uint16_t idx, uint32_t bytes, uint8_t codec, uint16_t frame_bytes,
		      int *frames)
{
	static uint8_t clip[40000];
	struct note n;
	uint32_t got = 0;

	if (bytes > sizeof(clip)) {
		return false;
	}
	CMD(mg_command_audio_queue, mg_audio_queue_command_read_clip, idx & 0xff, idx >> 8);
	if (!expect(CH_CTRL, mg_command_change_data_type, &n, 2000) || n.len != 2 ||
	    n.d[1] != mg_data_type_audio_queue) {
		TEST_PRINT("no change_data_type(audio_queue)");
		return false;
	}
	while (got < bytes && next(&n, 3000)) {
		if (n.ch != CH_DATA) {
			continue;
		}
		if (got + n.len > bytes || n.len > bt_gatt_get_mtu(conn) - 3) {
			return false;
		}
		memcpy(&clip[got], n.d, n.len);
		got += n.len;
	}
	if (got != bytes) {
		TEST_PRINT("clip %u: %u of %u bytes", idx, got, bytes);
		return false;
	}
	reset_decoders();
	bool ok = codec == mg_data_type_audio_lc3 ? decode_lc3(clip, bytes, frame_bytes, frames)
						  : decode_sbc(clip, bytes, frames);

	return ok && tone_ratio() > 0.9;
}

static void test_offline(void)
{
	struct note n;

	TEST_START("central_offline");
	CHECK(bt_enable(NULL) == 0, "bt_enable");
	CHECK(connect_and_setup() == 0, "connect/setup failed");
	CMD(mg_command_request_status);
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no features");
	CHECK(memchr(&n.d[1], mg_command_audio_queue, n.len - 1) != NULL, "no audio_queue");
	drain();

	/* Turn the queue on, start empty, and leave. */
	CMD(mg_command_set_settings, mg_setting_parameter_audio_queue_enabled, 1);
	CMD(mg_command_audio_queue, mg_audio_queue_command_clear);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 5000), "no status after clear");
	CHECK(n.len == 13 && n.d[1] == mg_audio_queue_command_status && n.d[2] == 1 &&
		      sys_get_le16(&n.d[11]) == 0,
	      "queue not empty/enabled");
	uint32_t capacity = sys_get_le32(&n.d[7]);

	disconnect();
	TEST_PRINT("queue on (capacity %u), disconnected", capacity);

	/* The gadget records two utterances meanwhile. */
	k_sleep(K_SECONDS(6));
	CHECK(connect_and_setup() == 0, "reconnect failed");
	bench_on(); /* clips are read only by a proven central (or the bench switch) */
	CMD(mg_command_audio_queue, mg_audio_queue_command_status);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 2000), "no status");
	uint16_t count = sys_get_le16(&n.d[11]);

	TEST_PRINT("queue: %u clips, %u bytes used", count, sys_get_le32(&n.d[3]));
	CHECK(count == 2, "expected 2 clips, got %u", count);

	uint32_t start[2], bytes[2], ms[2];
	uint8_t codec[2];
	uint16_t fbytes[2];

	for (uint16_t i = 0; i < 2; i++) {
		CMD(mg_command_audio_queue, mg_audio_queue_command_clip_info, i, 0);
		CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 2000), "no clip_info");
		/* [.., codec, start_ms, bytes, rate, channels, frame_bytes,
		 * duration_us]: SBC 16000/1/60/8000, or LC3 16 kb/s 16000/1/20/10000. */
		/* ... then start_unix_ms (uint64; 0 here: nobody set the clock). */
		CHECK(n.len == 28 && sys_get_le64(&n.d[20]) == 0 &&
			      n.d[1] == mg_audio_queue_command_clip_info &&
			      sys_get_le16(&n.d[2]) == i && sys_get_le16(&n.d[13]) == 16000 &&
			      n.d[15] == 1,
		      "bad clip_info");
		codec[i] = n.d[4];
		start[i] = sys_get_le32(&n.d[5]);
		bytes[i] = sys_get_le32(&n.d[9]);
		fbytes[i] = sys_get_le16(&n.d[16]);
		uint16_t us = sys_get_le16(&n.d[18]);

		CHECK((codec[i] == mg_data_type_audio_sbc && fbytes[i] == 60 && us == 8000) ||
			      (codec[i] == mg_data_type_audio_lc3 && fbytes[i] == 20 && us == 10000),
		      "clip format %u, %u B, %u us", codec[i], fbytes[i], us);
		CHECK(bytes[i] % fbytes[i] == 0, "clip isn't whole frames");
		ms[i] = bytes[i] / fbytes[i] * us / 1000;
		TEST_PRINT("clip %u: %s, %u B frames, starts at %u ms, %u bytes (%u ms)", i,
			   codec[i] == mg_data_type_audio_lc3 ? "LC3" : "SBC", fbytes[i], start[i],
			   bytes[i], ms[i]);
	}
	CHECK(start[1] > start[0] + 1000, "clip timestamps");
	/* 1.2 s and 0.8 s presses (with up to 1 s of pre-roll trimmed to the press). */
	CHECK(ms[0] > 1040 && ms[0] < 1320, "clip 0 length");
	CHECK(ms[1] > 640 && ms[1] < 920, "clip 1 length");

	for (uint16_t i = 0; i < 2; i++) {
		int frames = 0;

		CHECK(read_clip(i, bytes[i], codec[i], fbytes[i], &frames), "reading clip %u failed",
		      i);
		TEST_PRINT("clip %u downloaded: %d frames decode to the 1 kHz tone", i, frames);
	}

	CMD(mg_command_audio_queue, mg_audio_queue_command_read_clip, 7, 0);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000), "no error for a bad index");
	CHECK(n.d[1] == mg_command_audio_queue && err_code(&n) == mg_error_code_invalid_value,
	      "bad error for a bad index");

	CMD(mg_command_audio_queue, mg_audio_queue_command_clear);
	CHECK(expect(CH_CTRL, mg_command_audio_queue, &n, 30000), "no status after clear");
	CHECK(sys_get_le16(&n.d[11]) == 0 && sys_get_le32(&n.d[3]) == 0, "not cleared");
	disconnect();
	TEST_PASS("offline queue: 2 clips listed, downloaded, decoded and cleared");
}

/* ----------------------------------------------------- Muse Link setup */

/* The app's side of Link setup (esp32/main/link_pairing.c is the device
 * reference), then the token proof over mgcommands with the K both sides
 * derive. */

static struct mg_setup_keys sk; /* sk.rx: mobile -> device, sk.tx: device -> mobile */
static uint64_t s_tx_ctr, s_rx_ctr;
static char node_id[32];
/* The node id form a new setup must offer: gadget- (gadget.conf), or
 * homelink- for a gadget built with the default CONFIG_MG_NODE_ID_GADGET=n
 * (central_setup_homelink). */
static const char *want_prefix = "gadget-";

static void setup_write(const char *msg)
{
	size_t len = strlen(msg);
	size_t chunk = bt_gatt_get_mtu(conn) - 3 - 3;
	size_t total = (len + chunk - 1) / chunk;
	uint8_t pkt[244];

	for (size_t i = 0; i < total; i++) {
		size_t n = MIN(chunk, len - i * chunk);

		pkt[0] = 0xFE;
		pkt[1] = i;
		pkt[2] = total;
		memcpy(&pkt[3], &msg[i * chunk], n);
		for (int tries = 0; bt_gatt_write_without_response(conn, h[CH_SETUP_RX], pkt, n + 3,
							     false) != 0;
		     tries++) {
			if (tries > 100) {
				TEST_FAIL("setup write failed");
				return;
			}
			k_sleep(K_MSEC(10));
		}
	}
}

/* The next setup message: a reassembled chunked one or a raw status. */
static char *setup_read(int ms)
{
	static char buf[2048];
	size_t len = 0;
	struct note n;
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end) {
		if (!next(&n, MAX(1, (int)(end - k_uptime_get()))) || n.ch != CH_SETUP) {
			continue;
		}
		if (n.len < 3 || n.d[0] != 0xFE) {
			memcpy(buf, n.d, n.len);
			buf[n.len] = '\0';
			return buf;
		}
		if (len + n.len - 3 >= sizeof(buf)) {
			return NULL;
		}
		memcpy(&buf[len], &n.d[3], n.len - 3);
		len += n.len - 3;
		if (n.d[1] + 1 == n.d[2]) {
			buf[len] = '\0';
			return buf;
		}
	}
	return NULL;
}

static void setup_write_enc(const char *plain)
{
	static uint8_t ct[1024];
	static char ct_b64[1400], env[1600];
	char tag[32], ctr[24];
	size_t n = strlen(plain);

	mg_setup_seal(sk.rx, MG_SETUP_M2D, s_tx_ctr, sk.session_id_b64, (const uint8_t *)plain, n,
		      ct);
	mg_b64url_encode(ct, n, ct_b64, sizeof(ct_b64));
	mg_b64url_encode(&ct[n], 16, tag, sizeof(tag));
	snprintf(ctr, sizeof(ctr), "%u", (unsigned int)s_tx_ctr++);
	snprintf(env, sizeof(env),
		 "{\"action\":\"pairing_encrypted\",\"session_id\":\"%s\",\"counter\":\"%s\","
		 "\"ciphertext\":\"%s\",\"tag\":\"%s\"}",
		 sk.session_id_b64, ctr, ct_b64, tag);
	setup_write(env);
}

/* The device's next record, opened; its status (or NULL). */
static const char *setup_read_status(int ms)
{
	static struct mg_json_obj o, p;
	static char pt[600];
	uint8_t raw[700];
	size_t cl, tl;
	char *m = setup_read(ms);

	if (!m || mg_json_parse(m, strlen(m), &o) || !mg_json_str(&o, "type") ||
	    strcmp(mg_json_str(&o, "type"), "pairing_encrypted") != 0) {
		TEST_PRINT("not a record: %s", m ? m : "(nothing)");
		return NULL;
	}
	const char *c = mg_json_str(&o, "ciphertext"), *t = mg_json_str(&o, "tag");

	if (!c || !t || mg_b64url_decode(c, strlen(c), raw, sizeof(raw) - 16, &cl) ||
	    cl >= sizeof(pt) || mg_b64url_decode(t, strlen(t), &raw[cl], 16, &tl) ||
	    mg_setup_open(sk.tx, MG_SETUP_D2M, s_rx_ctr, sk.session_id_b64, raw, cl + 16,
			  (uint8_t *)pt)) {
		TEST_PRINT("record doesn't open");
		return NULL;
	}
	s_rx_ctr++;
	pt[cl] = '\0';
	TEST_PRINT("setup <- %s", pt);
	if (mg_json_parse(pt, cl, &p)) {
		return NULL;
	}
	return mg_json_str(&p, "status");
}

/* get_device_info .. pairing_confirmed, the gadget's button pressed by its
 * hook. */
static bool link_handshake(void)
{
	static struct mg_json_obj o;
	static char info[600], ready[1024];
	psa_key_id_t key;
	uint8_t mpub[65], dpub[65], mn[16], dn[16], ss[32], th[32];
	char mpub_b64[96], mn_b64[32], th_b64[48];
	size_t n;

	s_tx_ctr = s_rx_ctr = 0;
	setup_write("{\"action\":\"get_device_info\"}");
	char *m = setup_read(3000);

	CHECK_OR(false, m, "no device_info");
	snprintf(info, sizeof(info), "%s", m);
	CHECK_OR(false, mg_json_parse(m, strlen(m), &o) == 0, "device_info isn't JSON");
	CHECK_OR(false, mg_json_str(&o, "wifi") && strcmp(mg_json_str(&o, "wifi"), "none") == 0,
		 "wifi not none: %s", info);
	CHECK_OR(false, mg_json_get(&o, "mgcommands") && mg_json_get(&o, "mgcommands")->num == 1,
		 "no mgcommands 1");
	CHECK_OR(false, mg_json_get(&o, "pairing_protocol")->num == 5, "not protocol 5");
	CHECK_OR(false, strcmp(mg_json_str(&o, "pairing_auth"), "none") == 0, "not community");
	snprintf(node_id, sizeof(node_id), "%s", mg_json_str(&o, "node_id"));
	char want_node[32];

	/* A new setup's node id is <prefix><6 hex>. */
	snprintf(want_node, sizeof(want_node), "%s%c%c%c%c%c%c", want_prefix, peer_name[11] | 0x20,
		 peer_name[12] | 0x20, peer_name[13] | 0x20, peer_name[14] | 0x20,
		 peer_name[15] | 0x20, peer_name[16] | 0x20);
	for (char *p = want_node; *p; p++) {
		if (*p >= 'A' && *p <= 'Z') {
			*p += 32;
		}
	}
	CHECK_OR(false, strcmp(node_id, want_node) == 0, "node_id %s for %s", node_id, peer_name);
	TEST_PRINT("device_info: %s", info);

	CHECK_OR(false, mg_setup_p256_generate(&key, mpub) == 0 && mg_psa_random(mn, 16) == 0,
		 "keygen");
	mg_b64url_encode(mpub, 65, mpub_b64, sizeof(mpub_b64));
	mg_b64url_encode(mn, 16, mn_b64, sizeof(mn_b64));
	char hello[300];

	snprintf(hello, sizeof(hello),
		 "{\"action\":\"pairing_client_hello\",\"version\":5,\"pairing_auth\":\"none\","
		 "\"pairing_policy\":\"confirm_press\",\"mobile_pub\":\"%s\",\"mobile_nonce\":\"%s\"}",
		 mpub_b64, mn_b64);
	setup_write(hello);
	m = setup_read(5000);
	CHECK_OR(false, m, "no pairing_ready");
	snprintf(ready, sizeof(ready), "%s", m);
	CHECK_OR(false, mg_json_parse(ready, strlen(ready), &o) == 0 &&
				strcmp(mg_json_str(&o, "type"), "pairing_ready") == 0,
		 "not pairing_ready: %s", m);
	const char *dpub_s = mg_json_str(&o, "device_pub"), *dn_s = mg_json_str(&o, "device_nonce");

	CHECK_OR(false, dpub_s && dn_s &&
				mg_b64url_decode(dpub_s, strlen(dpub_s), dpub, 65, &n) == 0 &&
				n == 65 && mg_b64url_decode(dn_s, strlen(dn_s), dn, 16, &n) == 0,
		 "bad device_pub / nonce");
	const pairing_transcript_fields_t f = {
		.device_id = mg_json_str(&o, "device_id"),
		.node_id = mg_json_str(&o, "node_id"),
		.mac = mg_json_str(&o, "mac"),
		.firmware_version = mg_json_str(&o, "firmware_version"),
		.mobile_pub = mpub_b64,
		.device_pub = dpub_s,
		.mobile_nonce = mn_b64,
		.device_nonce = dn_s,
	};
	char *t = pairing_transcript_build(true, 0, "confirm_press", &f);

	CHECK_OR(false, t && mg_psa_sha256((const uint8_t *)t, strlen(t), th) == 0, "transcript");
	free(t);
	mg_b64url_encode(th, 32, th_b64, sizeof(th_b64));
	CHECK_OR(false, strcmp(th_b64, mg_json_str(&o, "transcript_hash")) == 0,
		 "transcript hash differs");
	CHECK_OR(false, mg_setup_ecdh(key, dpub, ss) == 0 &&
				mg_setup_derive(ss, mn, dn, th, &sk, NULL) == 0,
		 "key agreement");
	psa_destroy_key(key);
	CHECK_OR(false, strcmp(sk.session_id_b64, mg_json_str(&o, "session_id")) == 0,
		 "session id differs");
	TEST_PRINT("pairing_ready: transcript hash and session id agree");

	setup_write_enc("{\"action\":\"pairing_client_finished\"}");
	const char *st = setup_read_status(5000);

	CHECK_OR(false, st && strcmp(st, "confirm_required") == 0, "no confirm_required");
	/* The gadget's hook presses the button. */
	st = setup_read_status(10000);
	CHECK_OR(false, st && strcmp(st, "pairing_confirmed") == 0, "no pairing_confirmed");
	return true;
}

static uint8_t proof_k[32];

/* mgcommands.h, Token proof, with the stdlib-free PSA helpers. */
static int proof_key(const char *access, const char *node, uint8_t k[32])
{
	return mg_psa_hkdf32((const uint8_t *)MG_TOKEN_PROOF_SALT, strlen(MG_TOKEN_PROOF_SALT),
			     (const uint8_t *)access, strlen(access), (const uint8_t *)node,
			     strlen(node), k);
}

static void proof_mac(bool device, const uint8_t cn[16], const uint8_t dn[16], uint8_t mac[32])
{
	const char *label = device ? MG_TOKEN_PROOF_DEVICE_LABEL : MG_TOKEN_PROOF_CLIENT_LABEL;
	const uint8_t *parts[] = {(const uint8_t *)label, cn, dn};
	const size_t lens[] = {strlen(label), 16, 16};

	mg_psa_hmac_sha256(proof_k, 32, parts, lens, 3, mac);
}

/* challenge / response / confirm; returns the result byte, or -1. */
static int run_proof(void)
{
	struct note n;
	uint8_t cmd[2 + 32], mac[32];

	cmd[0] = mg_command_token_proof;
	cmd[1] = mg_token_proof_challenge;
	mg_psa_random(&cmd[2], 16);
	write_cmd(CH_CTRL, cmd, 18);
	if (!expect(CH_CTRL, mg_command_token_proof, &n, 3000) ||
	    n.d[1] != mg_token_proof_response || n.len != 50) {
		return -1;
	}
	proof_mac(true, &cmd[2], &n.d[2], mac);
	if (!mg_psa_ct_equal(mac, &n.d[18], 32)) {
		TEST_PRINT("device MAC doesn't verify");
		return -1;
	}
	uint8_t conf[34] = {mg_command_token_proof, mg_token_proof_confirm};

	proof_mac(false, &cmd[2], &n.d[2], &conf[2]);
	write_cmd(CH_CTRL, conf, sizeof(conf));
	if (!expect(CH_CTRL, mg_command_token_proof, &n, 3000) ||
	    n.d[1] != mg_token_proof_result) {
		return -1;
	}
	return n.d[2];
}

static void test_setup(void)
{
	static const char access[] = "bsim-access-0123456789abcdef";
	static const char access2[] = "bsim-access-second-provisioning";
	struct note n;
	char msg[300];

	TEST_START("central_setup");
	CHECK(bt_enable(NULL) == 0, "bt_enable");
	CHECK(mg_psa_init() == 0, "psa");

	/* Unprovisioned: both payloads, from one address. */
	want_both = true;
	CHECK(connect_and_setup() == 0, "connect/setup failed");
	want_both = false;
	CHECK(adv_mg && adv_setup && !adv_other_addr,
	      "advertising: mg %d, Link setup %d, other address %d", adv_mg, adv_setup,
	      adv_other_addr);
	TEST_PRINT("advertising: mg %d, Link setup %d, one address", adv_mg, adv_setup);
	/* Gadget setup v2: bit 1, a Muse Gadget over Bluetooth; bit 0 clear. */
	CHECK(adv_link_flags == 0x02, "Link setup flags byte 0x%02x, want 0x02", adv_link_flags);

	/* No token yet. */
	CMD(mg_command_token_proof, mg_token_proof_challenge, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	    11, 12, 13, 14, 15);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000) && n.d[1] == mg_command_token_proof &&
		      err_code(&n) == mg_error_code_not_found,
	      "challenge without a token isn't not_found");

	if (!link_handshake()) {
		return;
	}
	setup_write_enc("{\"action\":\"provision_v2\",\"ssid\":\"home\",\"password\":\"x\","
			"\"access_token\":\"a\",\"refresh_token\":\"r\",\"token_type\":\"device\"}");
	const char *st = setup_read_status(3000);

	CHECK(st && strcmp(st, "error_wifi_unsupported") == 0, "Wi-Fi not refused");
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":"
		 "\"bsim-refresh\",\"token_type\":\"device\"}",
		 access);
	setup_write_enc(msg);
	st = setup_read_status(5000);
	CHECK(st && strcmp(st, "auth_ok") == 0, "no auth_ok");
	TEST_PRINT("token-only provisioning: auth_ok");

	/* mgcommands on the same connection: 34 listed, the proof matches. */
	CHECK(proof_key(access, node_id, proof_k) == 0, "K");
	drain();
	CMD(mg_command_request_status);
	CHECK(expect(CH_CTRL, mg_command_supported_features, &n, 2000), "no features");
	CHECK(memchr(&n.d[1], mg_command_token_proof, n.len - 1) != NULL, "34 not listed");
	drain();
	CHECK(run_proof() == 1, "proof didn't match");
	TEST_PRINT("token proof: match");
	disconnect();

	/* Set up: mg only, the proof still matches. */
	k_sleep(K_MSEC(500));
	CHECK(connect_and_setup() == 0, "reconnect failed");
	CHECK(adv_mg && !adv_setup, "Link setup still advertised (%d)", adv_setup);
	CHECK(run_proof() == 1, "proof didn't match after reconnecting");

	/* Gadget setup v2: set up again on this connection, found by its mg
	 * advertisement: the same handshake and press (the gadget's hook
	 * presses again), and the commit replaces tokens and K. */
	if (!link_handshake()) {
		return;
	}
	snprintf(msg, sizeof(msg),
		 "{\"action\":\"provision_v2\",\"access_token\":\"%s\",\"refresh_token\":"
		 "\"bsim-refresh-2\",\"token_type\":\"device\"}",
		 access2);
	setup_write_enc(msg);
	st = setup_read_status(5000);
	CHECK(st && strcmp(st, "auth_ok") == 0, "no auth_ok re-provisioning");
	drain();
	CHECK(run_proof() == -1, "the old K still verifies");
	CHECK(proof_key(access2, node_id, proof_k) == 0, "K");
	drain();
	CHECK(run_proof() == 1, "the new K doesn't match");
	TEST_PRINT("re-provisioned: the old K is gone, the new one matches");
	/* clear: result 1, then the gadget is back in setup. */
	CMD(mg_command_token_proof, mg_token_proof_clear);
	CHECK(expect(CH_CTRL, mg_command_token_proof, &n, 3000) &&
		      n.d[1] == mg_token_proof_result && n.d[2] == 1,
	      "clear not answered with result 1");
	disconnect();

	k_sleep(K_MSEC(500));
	want_both = true;
	CHECK(connect_and_setup() == 0, "reconnect after clear failed");
	want_both = false;
	CHECK(adv_setup > 0, "Link setup not advertised after clear");
	CMD(mg_command_token_proof, mg_token_proof_challenge, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	    11, 12, 13, 14, 15);
	CHECK(expect(CH_CTRL, mg_command_error, &n, 2000) && err_code(&n) == mg_error_code_not_found,
	      "token survived clear");
	disconnect();
	TEST_PASS("Link setup (v5, button), token-only provisioning, re-provisioning, token proof "
		  "and clear verified");
}

/* The setup scenario against a gadget offering the default node id. */
static void test_setup_homelink(void)
{
	want_prefix = "homelink-";
	test_setup();
}

static const struct bst_test_instance tests[] = {
	{.test_id = "central_offline", .test_main_f = test_offline},
	{.test_id = "central_setup", .test_main_f = test_setup},
	{.test_id = "central_setup_homelink", .test_main_f = test_setup_homelink},
	BSTEST_END_MARKER,
};

static struct bst_test_list *install(struct bst_test_list *list)
{
	return bst_add_tests(list, tests);
}

bst_test_install_t test_installers[] = {install, NULL};

int main(void)
{
	bst_main();
	return 0;
}
