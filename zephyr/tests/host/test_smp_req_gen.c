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
 * The board gate's own SMP reassembly wraps (src/mg_smp_req_gen.c, built for
 * CONFIG_MG_SMP_GATE_BOARD without CONFIG_MG_DFU, so smp_link's MG_DFU build
 * never runs them): writes fed in smp_bt's order (expected, then collect,
 * then smp_rx_req once a request is whole) over a stand-in reassembly. A
 * partial request whose first fragment came on a connection that's gone is
 * dropped before the next write is collected, and every request is stamped
 * with its first fragment's connection, which its handlers read back.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include "check.h"
#include "host_fakes.h"
#include "mg_smp_depth.h"

uint32_t mg_smp_request_gen(void);
int __wrap_smp_reassembly_expected(const struct smp_transport *smpt);
int __wrap_smp_reassembly_collect(struct smp_transport *smpt, const void *buf, uint16_t len);
void __wrap_smp_rx_req(struct smp_transport *smpt, struct net_buf *nb);
int __wrap_smp_process_request_packet(struct smp_streamer *streamer, void *req);

/* The stand-in reassembly: one request at a time, its length its first
 * byte (SMP's header carries it; the wraps don't read it). */
static struct net_buf rx;
static int want = -1, have, drops;
static struct smp_transport smpt;

int __real_smp_reassembly_expected(const struct smp_transport *t)
{
	(void)t;
	return want < 0 ? -EINVAL : want - have;
}

int __real_smp_reassembly_collect(struct smp_transport *t, const void *buf, uint16_t len)
{
	(void)t;
	if (want < 0) {
		memset(&rx, 0, sizeof(rx));
		want = ((const uint8_t *)buf)[0];
		have = 0;
	}
	if (have + len > want) {
		return -EOVERFLOW;
	}
	have += len;
	return want - have;
}

int smp_reassembly_drop(struct smp_transport *t)
{
	(void)t;
	want = -1;
	drops++;
	return 0;
}

/* smp_rx_req hands the request to SMP's work queue, which handles it. */
static int handled;
static uint32_t handled_gen;

int __real_smp_process_request_packet(struct smp_streamer *streamer, void *req)
{
	(void)streamer;
	(void)req;
	handled++;
	handled_gen = mg_smp_request_gen();
	return 0;
}

void __real_smp_rx_req(struct smp_transport *t, struct net_buf *nb)
{
	(void)t;
	CHECK(__wrap_smp_process_request_packet(NULL, nb) == 0);
	CHECK(mg_smp_request_gen() == 0); /* only while its handlers run */
}

/* Zephyr's transport frees a dropped request (smp_internal.h). */
static int freed;
static void *freed_buf, *freed_arg;

void smp_free_buf(void *buf, void *arg)
{
	freed++;
	freed_buf = buf;
	freed_arg = arg;
}

/* smp_bt_chr_write's order (Zephyr v4.4.2, subsys/mgmt/mcumgr/transport/
 * src/smp_bt.c): expected, collect, and a whole request goes on. */
static void bt_write(const uint8_t *buf, uint16_t len)
{
	int ret;

	(void)__wrap_smp_reassembly_expected(&smpt);
	ret = __wrap_smp_reassembly_collect(&smpt, buf, len);
	CHECK(ret >= 0);
	if (ret == 0) {
		want = -1;
		__wrap_smp_rx_req(&smpt, &rx);
	}
}

static void reset(void)
{
	want = -1;
	have = drops = handled = 0;
	handled_gen = 0;
	host_log_clear();
}

static void test_whole_requests(void)
{
	const uint8_t one[4] = {4, 1, 2, 3};

	printf("  whole requests carry their connection\n");
	reset();
	host_conn_gen = 3;
	bt_write(one, sizeof(one));
	CHECK(handled == 1 && handled_gen == 3);
	host_conn_gen = 5; /* no partial left: nothing to drop */
	bt_write(one, sizeof(one));
	CHECK(handled == 2 && handled_gen == 5);
	CHECK(drops == 0);
}

static void test_fragments_one_link(void)
{
	const uint8_t a[3] = {6, 1, 2}, b[3] = {3, 4, 5};

	printf("  a request over two writes on one connection\n");
	reset();
	host_conn_gen = 7;
	bt_write(a, sizeof(a));
	CHECK(handled == 0);
	bt_write(b, sizeof(b));
	CHECK(handled == 1 && handled_gen == 7 && drops == 0);
}

static void test_orphan_dropped(void)
{
	const uint8_t a[3] = {6, 1, 2}, next[6] = {6, 9, 9, 9, 9, 9};

	printf("  a partial request from a connection that's gone\n");
	reset();
	host_conn_gen = 3;
	bt_write(a, sizeof(a)); /* 3 bytes still owed, then the link drops */
	host_conn_gen = 5;
	/* The next connection's request isn't taken as those 3 bytes: the
	 * orphan goes first, and the new request is the new connection's. */
	bt_write(next, sizeof(next));
	CHECK(drops == 1);
	CHECK(strstr(host_log_buf, "from a connection that's gone") != NULL);
	CHECK(handled == 1 && handled_gen == 5);
	/* Even when its bytes would exactly complete the orphan. */
	reset();
	host_conn_gen = 8;
	bt_write(a, sizeof(a));
	host_conn_gen = 9;
	{
		const uint8_t three[3] = {3, 0, 0};

		bt_write(three, sizeof(three));
	}
	CHECK(drops == 1);
	CHECK(handled == 1 && handled_gen == 9);
}

/* The nesting limit in this build too (mg_smp_depth.h; src/zephyr/smp.c
 * has it with MG_DFU): a packet whose request nests deeper than
 * MG_SMP_MAX_DEPTH, or isn't whole CBOR inside its length, never reaches
 * Zephyr's request handling (whose OS echo the gate lets through unproven):
 * it is freed and answered MGMT_ERR_EINVAL. A whole, shallow one goes on. */
static void test_depth_dropped(void)
{
	static uint8_t pkt[8 + 504];
	static const uint8_t echo[] = {2, 0, 0, 5, 0, 0, 1, 0, 0xa1, 0x61, 'd', 0x61, 'x'};
	/* {"d":"x","z":<a text string of length 2^32>,"y":[[...]]}: on a 32-bit
	 * target zcbor skips the string as 0 bytes and decodes the nest. */
	static const uint8_t head[] = {0xa3, 0x61, 'd', 0x61, 'x', 0x61, 'z', 0x7b, 0, 0, 0,
				       1, 0, 0, 0, 0, 0x61, 'y'};
	struct smp_transport t;
	struct smp_streamer st = {.smpt = &t};
	struct net_buf nb;
	size_t body;

	printf("  a request nesting deeper than %d, or not whole CBOR, dropped before "
	       "any handler\n", MG_SMP_MAX_DEPTH);
	reset();
	freed = 0;
	host_conn_gen = 4;
	/* The deep echo, as one reassembled request. */
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 2;
	body = 7 + 493 + 1;
	pkt[2] = (uint8_t)(body >> 8);
	pkt[3] = (uint8_t)body;
	memcpy(pkt + 8, (const uint8_t[]){0xa2, 0x61, 'd', 0x61, 'x', 0x61, 'z'}, 7);
	memset(pkt + 15, 0x81, 493);
	pkt[8 + body - 1] = 0x00;
	memset(&nb, 0, sizeof(nb));
	nb.data = pkt;
	nb.len = (uint16_t)(8 + body);
	CHECK(__wrap_smp_process_request_packet(&st, &nb) == MGMT_ERR_EINVAL);
	CHECK(handled == 0);
	CHECK(freed == 1 && freed_buf == &nb && freed_arg == &t);
	CHECK(strstr(host_log_buf, "nesting deeper than 8: dropped") != NULL);
	/* The 2^32 length, then a nest of 480. */
	body = sizeof(head) + 480 + 1;
	pkt[2] = (uint8_t)(body >> 8);
	pkt[3] = (uint8_t)body;
	memcpy(pkt + 8, head, sizeof(head));
	memset(pkt + 8 + sizeof(head), 0x81, 480);
	pkt[8 + body - 1] = 0x00;
	nb.len = (uint16_t)(8 + body);
	CHECK(__wrap_smp_process_request_packet(&st, &nb) == MGMT_ERR_EINVAL);
	CHECK(handled == 0 && freed == 2);
	/* A whole, shallow echo goes on, with its connection. */
	memcpy(pkt, echo, sizeof(echo));
	memset(nb.user_data, 0, sizeof(nb.user_data));
	memcpy(nb.user_data + sizeof(nb.user_data) - 4, &host_conn_gen, 4);
	nb.len = sizeof(echo);
	CHECK(__wrap_smp_process_request_packet(&st, &nb) == 0);
	CHECK(handled == 1 && handled_gen == 4 && freed == 2);
}

HOST_MAIN(test_whole_requests, test_fragments_one_link, test_orphan_dropped, test_depth_dropped)
