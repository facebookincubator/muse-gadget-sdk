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
 * Each SMP request's source connection without CONFIG_MG_DFU
 * (CONFIG_MG_SMP_GATE_BOARD builds Zephyr's own smp.c, not src/zephyr/smp.c,
 * whose change 3 and src/mg_ota.c's reassembly wraps do this for MG_DFU):
 * smp_rx_req(), which smp_bt calls once a request is complete, stamps the
 * connection the request's first fragment came on (mg_smp_rx_source_gen())
 * into the last 4 bytes of the request's user data, and
 * smp_process_request_packet(), inside which the request's handlers run,
 * takes it out as mg_smp_request_gen(). Both are wrapped at link time
 * (CMakeLists.txt), so src/mg_smp_gate.c refuses a request whose connection
 * isn't the link any more.
 *
 * With SMP reassembly (a request over several writes) smp_reassembly_expected()
 * and smp_reassembly_collect() are wrapped too (smp_bt asks the first before
 * it collects each write, on the Bluetooth RX thread, the only caller of
 * both): collect records the connection of a request's first fragment, and
 * expected drops a partly received request whose connection isn't the link
 * any more (mg_smp_rx_orphan(): a central that never subscribed leaves its
 * partial request behind, and GATT's disconnect cleanup skips it), so the
 * next connection's writes start a request of their own instead of
 * completing it. This build has no receive-gap check (CONFIG_MG_SMP_RX_GAP_MS
 * is MG_DFU's): a partial request on the same connection waits for its
 * bytes as upstream's does.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>

#include <zephyr/logging/log.h>
#if defined(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
#include <mgmt/mcumgr/transport/smp_reassembly.h>
#endif

#include "mg_ota_policy.h"
#include "mg_smp_depth.h"
#include "mg_transport.h"

#include "mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK(); /* the -Wl,--wrap functions below */

LOG_MODULE_DECLARE(mg_smp_gate, CONFIG_MG_LOG_LEVEL);

/* After smp_bt's own user data (a pointer and an id). */
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE >=
		     MAX(CONFIG_MCUMGR_TRANSPORT_NETBUF_MIN_USER_DATA_SIZE, 2 * sizeof(void *)) +
			     sizeof(uint32_t),
	     "the request's source generation needs 4 bytes after the transport's user data");
#define REQ_GEN_OFF (CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE - sizeof(uint32_t))

void __real_smp_rx_req(struct smp_transport *smpt, struct net_buf *nb);
int __real_smp_process_request_packet(struct smp_streamer *streamer, void *req);

static uint32_t req_gen; /* the request being handled (SMP's work queue only) */

uint32_t mg_smp_request_gen(void)
{
	return req_gen;
}

#if defined(CONFIG_MCUMGR_TRANSPORT_REASSEMBLY)
static uint32_t rx_first_gen; /* the partial request's first fragment's connection */

int __real_smp_reassembly_expected(const struct smp_transport *smpt);
int __real_smp_reassembly_collect(struct smp_transport *smpt, const void *buf, uint16_t len);

int __wrap_smp_reassembly_expected(const struct smp_transport *smpt)
{
	int expected = __real_smp_reassembly_expected(smpt);

	if (mg_smp_rx_orphan(expected, rx_first_gen, mg_transport_conn_gen())) {
		LOG_WRN("SMP: dropped a partly received request from a connection that's gone");
		(void)smp_reassembly_drop((struct smp_transport *)smpt);
		return -EINVAL;
	}
	return expected;
}

int __wrap_smp_reassembly_collect(struct smp_transport *smpt, const void *buf, uint16_t len)
{
	if (__real_smp_reassembly_expected(smpt) < 0) {
		rx_first_gen = mg_transport_conn_gen(); /* a first fragment */
	}
	return __real_smp_reassembly_collect(smpt, buf, len);
}

uint32_t mg_smp_rx_source_gen(void)
{
	return rx_first_gen;
}
#else
/* Each write is a whole request, taken on the connection that sent it. */
uint32_t mg_smp_rx_source_gen(void)
{
	return mg_transport_conn_gen();
}
#endif

void __wrap_smp_rx_req(struct smp_transport *smpt, struct net_buf *nb)
{
	uint32_t gen = mg_smp_rx_source_gen();

	memcpy((uint8_t *)net_buf_user_data(nb) + REQ_GEN_OFF, &gen, sizeof(gen));
	__real_smp_rx_req(smpt, nb);
}

/* Zephyr's transport (subsys/mgmt/mcumgr/transport/src/smp.c, declared in
 * its smp_internal.h): frees a request buffer and its user data. */
void smp_free_buf(void *buf, void *arg);

int __wrap_smp_process_request_packet(struct smp_streamer *streamer, void *req)
{
	struct net_buf *nb = req;
	int rc;

	/* The nesting limit (mg_smp_depth.h), as src/zephyr/smp.c's change 5
	 * with MG_DFU: every request whole CBOR inside its length, none nesting
	 * deeper than MG_SMP_MAX_DEPTH, before any handler decodes one (the gate
	 * lets the OS group's echo through unproven, and upstream's work queue
	 * stack is 2 KB). The packet is dropped, as the transport frees one. */
	if (!mg_smp_depth_ok(nb->data, nb->len)) {
		LOG_WRN("SMP: a request not whole CBOR or nesting deeper than %d: dropped",
			MG_SMP_MAX_DEPTH);
		smp_free_buf(req, streamer->smpt);
		return MGMT_ERR_EINVAL;
	}
	memcpy(&req_gen, (uint8_t *)net_buf_user_data(req) + REQ_GEN_OFF, sizeof(req_gen));
	rc = __real_smp_process_request_packet(streamer, req);
	req_gen = 0;
	return rc;
}
