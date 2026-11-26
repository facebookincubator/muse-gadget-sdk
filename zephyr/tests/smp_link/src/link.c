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
 * The "fake GATT" link (the one
 * esp32/components/muse_gadget_ble/smp/conformance/smplink.py speaks): UDP
 * datagrams of one type byte and a value. Data
 * from the client is one ATT write without response (at most MTU - 3
 * bytes); data to it is one notification. The SMP side copies smp_bt.c:
 * every write goes through Zephyr's SMP reassembly into the board's
 * buffers, a response is cut into MTU - 3 notifications, and a response to
 * a request from an earlier connection is dropped. A disconnect drops a
 * partly received request, as smp_bt's CCC cleanup does for a central that
 * subscribed; a disconnect whose value byte is 1 is a central that never
 * subscribed, and keeps it, as smp_bt does (GATT's cleanup skips a CCC that
 * was never enabled), so the board's own reassembly wraps (src/mg_ota.c)
 * must drop it before the next connection's writes complete it.
 */

#include <errno.h>
#include <string.h>

#include <stdio.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include <mgmt/mcumgr/transport/smp_reassembly.h>

#include "mg_ota.h"
#include "sim.h"

/* The largest attribute value ATT allows (Core spec Vol 3 Part F 3.2.9). */
#define ATT_MAX_VALUE 512U

enum {
	T_DATA = 0x00,
	T_CONNECT = 0x01,
	T_DISCONNECT = 0x02,
	T_READY = 0x04,
	T_DROP_NEXT = 0x05,
	T_STATUS = 0x07,
	T_AUTH = 0x09, /* this connection proven (1) or not (0): the update gate */
	/* This app's own, outside the link's shared range: a setup committed (an
	 * ownership change), as mg_setup_store_commit_device_key() reports it. */
	T_SETUP_COMMITTED = 0x40,
	T_PRESS = 0x41, /* a real press of the talk button (the update prompt) */
	T_INJECTED = 0x42, /* an injected press (a console word): never a confirm on release */
	/* Forget (the token proof's clear, the sealed clear, the reset hold):
	 * the link's proof ends, then the store is erased (mg_setup_factory_reset()). */
	T_FORGET = 0x43,
};

/* src/mg_ota.c: the setup store erased (mg_setup_store_erase() reports it). */
void mg_setup_store_erased(void);

#define MAX_DGRAM 600 /* MTU 517 + the type byte, with room */

struct link_ud {
	uint32_t id; /* the connection the request came on */
};
BUILD_ASSERT(sizeof(struct link_ud) <= CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE);

static int sock = -1;
static struct net_sockaddr_in peer;
static bool have_peer;
static uint16_t att_mtu = 23;
static atomic_t conn_id;    /* 0: no client */
static uint32_t next_id = 1;
static bool gatt_ready;
static bool held;
static int drop_next;
static bool kept_partial; /* left by a central that never subscribed */
static struct smp_transport smpt;
static K_MUTEX_DEFINE(tx_lock);

static void send_dgram(uint8_t type, const uint8_t *v, size_t n)
{
	uint8_t d[MAX_DGRAM];

	if (!have_peer || sock < 0 || n + 1 > sizeof(d)) {
		return;
	}
	d[0] = type;
	if (n) {
		memcpy(&d[1], v, n);
	}
	(void)zsock_sendto(sock, d, n + 1, 0, (struct net_sockaddr *)&peer, sizeof(peer));
}

static uint16_t link_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);
	return atomic_get(&conn_id) ? att_mtu - 3 : 0;
}

static int link_tx(struct net_buf *nb)
{
	const struct link_ud *ud = net_buf_user_data(nb);
	int rc = MGMT_ERR_EOK;
	uint32_t id = (uint32_t)atomic_get(&conn_id);

	if (id == 0 || ud->id != id) {
		/* The client that asked is gone (smp_bt drops these too). */
		rc = MGMT_ERR_ENOENT;
	} else {
		uint16_t chunk = att_mtu - 3;

		k_mutex_lock(&tx_lock, K_FOREVER);
		for (uint16_t off = 0; off < nb->len; off += chunk) {
			send_dgram(T_DATA, nb->data + off, MIN(chunk, nb->len - off));
		}
		k_mutex_unlock(&tx_lock);
	}
	smp_packet_free(nb);
	return rc;
}

static int link_ud_copy(struct net_buf *dst, const struct net_buf *src)
{
	memcpy(net_buf_user_data(dst), net_buf_user_data(src), sizeof(struct link_ud));
	return MGMT_ERR_EOK;
}

static void link_ud_free(void *ud)
{
	ARG_UNUSED(ud);
}

static void drop_partial(void)
{
	if (smp_reassembly_expected(&smpt) >= 0) {
		(void)smp_reassembly_drop(&smpt);
	}
}

static void on_data(const uint8_t *v, size_t n)
{
	uint32_t id = (uint32_t)atomic_get(&conn_id);

	if (id == 0 || n == 0) {
		return;
	}
	kept_partial = false; /* the board's wraps decide from here */
	if (drop_next > 0) {
		drop_next--;
		printk("SIM: link: dropped a write (%u B) as asked\n", (unsigned int)n);
		return;
	}
	if (n > att_mtu - 3U || n > ATT_MAX_VALUE) {
		/* Android refuses a write over 512 B whatever the MTU (and ATT
		 * caps an attribute value there): count it as a failure. */
		printk("SIM: link: write of %u B is over MTU - 3 (%u) or %u: refused\n",
		       (unsigned int)n, att_mtu - 3U, ATT_MAX_VALUE);
		return;
	}
	bool started = smp_reassembly_expected(&smpt) >= 0;
	int ret = smp_reassembly_collect(&smpt, v, (uint16_t)n);

	if (ret < 0) {
		printk("SIM: link: reassembly refused a write (%d): request dropped\n", ret);
		drop_partial();
		return;
	}
	if (!started) {
		struct link_ud *ud = smp_reassembly_get_ud(&smpt);

		if (ud) {
			ud->id = id;
		}
	}
	if (ret == 0) {
		(void)smp_reassembly_complete(&smpt, false);
	}
}

static bool slot_version(uint8_t area, char *out, size_t len)
{
	struct mcuboot_img_header h;

	if (boot_read_bank_header(area, &h, sizeof(h)) != 0) {
		snprintf(out, len, "none");
		return false;
	}
	snprintf(out, len, "%u.%u.%u+%u", h.h.v1.sem_ver.major, h.h.v1.sem_ver.minor,
		 h.h.v1.sem_ver.revision, h.h.v1.sem_ver.build_num);
	return true;
}

/* 0x07: what the running gadget says about itself, for checks. */
static void send_status(void)
{
	static const char *const swaps[] = {"?", "none", "test", "perm", "revert", "fail"};
	char running[24], staged[24], j[256];
	int swap = mcuboot_swap_type();
	bool has_staged = slot_version(FIXED_PARTITION_ID(slot1_partition), staged, sizeof(staged));

	(void)slot_version(FIXED_PARTITION_ID(slot0_partition), running, sizeof(running));
	int n = snprintf(j, sizeof(j),
			 "{\"running\":\"%s\",\"confirmed\":%s,\"swap\":\"%s\",\"pending\":%s,"
			 "\"staged\":%s,\"staged_version\":\"%s\",\"healthy\":%s,\"boot\":%u,"
			 "\"safe\":%s,\"smp_blocks\":%d}",
			 running, boot_is_img_confirmed() ? "true" : "false",
			 swap >= 0 && swap < (int)ARRAY_SIZE(swaps) ? swaps[swap] : "?",
			 swap != BOOT_SWAP_TYPE_NONE ? "true" : "false", has_staged ? "true" : "false",
			 staged, sim_healthy ? "true" : "false", sim_boot, sim_safe_mode ? "true" : "false",
			 mg_smp_mem_blocks());

	send_dgram(T_STATUS, (const uint8_t *)j, (size_t)MIN(n, (int)sizeof(j) - 1));
}

static void rx_fn(void *a, void *b, void *c)
{
	uint8_t d[MAX_DGRAM];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		struct net_sockaddr_in from;
		net_socklen_t fl = sizeof(from);
		ssize_t n = zsock_recvfrom(sock, d, sizeof(d), 0, (struct net_sockaddr *)&from, &fl);

		if (n < 1) {
			k_sleep(K_MSEC(10));
			continue;
		}
		if (have_peer && (from.sin_port != peer.sin_port ||
				  from.sin_addr.s_addr != peer.sin_addr.s_addr) &&
		    d[0] != T_CONNECT) {
			continue; /* only the client that connected speaks for the link */
		}
		peer = from;
		have_peer = true;
		switch (d[0]) {
		case T_DATA:
			on_data(&d[1], (size_t)n - 1);
			break;
		case T_CONNECT:
			if (held) {
				break; /* restarting: no new link */
			}
			att_mtu = n >= 3 ? (uint16_t)(d[1] | (d[2] << 8)) : 23;
			att_mtu = CLAMP(att_mtu, 23, MAX_DGRAM - 1);
			if (!kept_partial) {
				drop_partial();
			}
			drop_next = 0;
			atomic_set(&conn_id, (atomic_val_t)next_id++);
			sim_proven = sim_proven_at_connect; /* --proven: every connection */
			printk("SIM: link: connected (ATT MTU %u)\n", att_mtu);
			/* A working link: as the gadget's own MTU exchange answered
			 * (mg_ble.c), so an updated image may keep itself. */
			mg_ota_link_proven();
			if (gatt_ready) {
				send_dgram(T_READY, NULL, 0);
			}
			break;
		case T_DISCONNECT:
			if (atomic_get(&conn_id)) {
				printk("SIM: link: the client disconnected\n");
			}
			atomic_set(&conn_id, 0);
			sim_proven = false;
			/* smp_bt asks nothing at such a disconnect: neither does this. */
			kept_partial = n >= 2 && d[1] == 1;
			if (kept_partial) {
				printk("SIM: link: the central never subscribed: a partial request stays\n");
			} else {
				drop_partial();
			}
			break;
		case T_AUTH:
			/* As a token proof on this connection (or its lapse: a new
			 * challenge). */
			sim_proven = n >= 2 && d[1] != 0;
			printk("SIM: link: this connection %s\n", sim_proven ? "proven" : "not proven");
			break;
		case T_DROP_NEXT:
			drop_next = n >= 2 ? d[1] : 1;
			break;
		case T_SETUP_COMMITTED:
			printk("SIM: link: a setup committed\n");
			mg_ota_setup_committed();
			break;
		case T_FORGET:
			printk("SIM: link: Forget\n");
			sim_proven = false;
			mg_setup_store_erased();
			break;
		case T_PRESS:
			printk("SIM: link: a press: %s\n",
			       mg_ota_prompt_take(true, false) ? "confirmed the update" : "no prompt");
			break;
		case T_INJECTED:
			printk("SIM: link: an injected press: %s\n",
			       mg_ota_prompt_take(false, !sim_release) ? "confirmed the update"
								       : "not a confirm");
			break;
		case T_STATUS:
			send_status();
			break;
		default:
			break;
		}
	}
}

static K_THREAD_STACK_DEFINE(link_rx_stack, 8192);
static struct k_thread rx_thread;

int link_start(uint16_t port)
{
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
	};

	smpt.functions.output = link_tx;
	smpt.functions.get_mtu = link_get_mtu;
	smpt.functions.ud_copy = link_ud_copy;
	smpt.functions.ud_free = link_ud_free;
	if (smp_transport_init(&smpt) != 0) {
		return -EIO;
	}
	sock = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}
	/* The fixture's port on the host's loopback only: its test hooks (proof
	 * state, a setup's commit) come from the local relay or client, never
	 * another host. */
	(void)zsock_inet_pton(NET_AF_INET, "127.0.0.1", &addr.sin_addr);
	if (zsock_bind(sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
		printk("SIM: link: can't bind UDP port %u (%d)\n", port, errno);
		return -errno;
	}
	k_thread_create(&rx_thread, link_rx_stack, K_THREAD_STACK_SIZEOF(link_rx_stack), rx_fn, NULL, NULL,
			NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
	k_thread_name_set(&rx_thread, "link_rx");
	printk("SIM: link: listening on UDP port %u\n", port);
	return 0;
}

void link_set_ready(bool ready)
{
	gatt_ready = ready;
	if (ready && atomic_get(&conn_id)) {
		send_dgram(T_READY, NULL, 0);
	}
}

void link_hold_for_reset(void)
{
	held = true;
	if (atomic_get(&conn_id)) {
		printk("SIM: link: dropping the client before the reset\n");
		send_dgram(T_DISCONNECT, NULL, 0);
	}
	atomic_set(&conn_id, 0);
	sim_proven = false;
	kept_partial = false;
	drop_partial();
}

uint32_t mg_transport_conn_gen(void)
{
	return (uint32_t)atomic_get(&conn_id);
}

void mg_transport_rx_request(void)
{
}

void mg_transport_peer_wrote(void)
{
}

bool mg_transport_link_up(void)
{
	return atomic_get(&conn_id) != 0;
}

bool mg_transport_link_seen_take(unsigned int who)
{
	ARG_UNUSED(who);
	return false;
}
