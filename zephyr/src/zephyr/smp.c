/*
 * Copyright Runtime.io 2018. All rights reserved.
 * Copyright (c) 2021-2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Muse Gadgets: a copy of Zephyr v4.4.2
 * subsys/mgmt/mcumgr/transport/src/smp.c (last upstream change 4f98eede4b8),
 * built in its place (../../CMakeLists.txt), so that update support takes no
 * base memory and each request carries its connection. Five changes, each
 * marked "Muse:", everything else is upstream's:
 *
 * 1. Buffers: each net_buf (header, user data and data in one block) comes
 *    from mg_smp_mem_get() when a request or a response needs it and goes
 *    back through the pool's destroy hook. At most
 *    CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT are out at once (what buf_count
 *    tells clients); the pool is only the registry entry net_buf needs.
 * 2. Work queue: started with the first request, its stack from
 *    mg_smp_mem_get(), and stopped and freed once no request or buffer has
 *    been about for CONFIG_MG_SMP_SESSION_IDLE_MS (then
 *    mg_smp_session_ended() releases an idle upload's image writer).
 * 3. Request source: smp_rx_req() stamps the connection the request's first
 *    fragment came on (mg_smp_rx_source_gen(), recorded by src/mg_ota.c's
 *    reassembly wrap; mg_transport_conn_gen() where there's no reassembly)
 *    into the last 4 bytes of the request's user data, and the request's handlers
 *    run with it as mg_smp_request_gen(), so a request still queued or
 *    dequeued when its link dropped can't borrow a later connection's
 *    token proof (src/mg_ota.c decides on it).
 * 4. Request bytes: while a request's handlers run, mg_smp_request_buf()
 *    is its net_buf (its data at the current packet's body, the header just
 *    pulled before it), so src/mg_ota.c's command hook reads an update
 *    write's own fields (an upload's offset, length, sha and data, a
 *    change's body) before img_mgmt acts on it.
 * 5. Nesting limit: a packet holding a request that isn't whole CBOR inside
 *    its own length, or whose CBOR nests deeper than MG_SMP_MAX_DEPTH
 *    (../mg_smp_depth.h), is dropped before any handler decodes it, so
 *    zcbor's recursive skip can't overflow the work queue's stack.
 *
 * Every difference from upstream sits in a block marked with its change
 * (the string.h include, for 1 and 3, too), and nothing else is changed.
 * Rebasing: take upstream's file, put the marked blocks back.
 */

#include <assert.h>
/* Muse: changes 1 and 3 (memset(), memcpy()). */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include <mgmt/mcumgr/transport/smp_reassembly.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mcumgr_smp, CONFIG_MCUMGR_TRANSPORT_LOG_LEVEL);

/* Built in place of Zephyr's own: written for one Zephyr version. */
#include "../mg_zephyr_version.h"

MG_ZEPHYR_WRAPS_CHECK();

/* Muse: change 5. */
#include "../mg_smp_depth.h"

/* To be able to unit test some callers some functions need to be
 * demoted to allow overriding them.
 */
#ifdef CONFIG_ZTEST
#define WEAK __weak
#else
#define WEAK
#endif

/* Muse: change 3 (each request's source connection). */
uint32_t mg_transport_conn_gen(void);
uint32_t mg_smp_rx_source_gen(void);
BUILD_ASSERT(CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE >=
		     CONFIG_MCUMGR_TRANSPORT_NETBUF_MIN_USER_DATA_SIZE + sizeof(uint32_t),
	     "the request's source generation needs 4 bytes after the transport's user data");
#define REQ_GEN_OFF (CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE - sizeof(uint32_t))
static uint32_t req_gen;

uint32_t mg_smp_request_gen(void)
{
	return req_gen;
}

/* Muse: change 4 (the request's bytes while its handlers run). */
static struct net_buf *mg_req_nb;

struct net_buf *mg_smp_request_buf(void)
{
	return mg_req_nb;
}

/* src/mg_ota.c's reassembly wrap overrides it with the first fragment's. */
__weak uint32_t mg_smp_rx_source_gen(void)
{
	return mg_transport_conn_gen();
}

/* Muse: change 2 (the work queue lives only while SMP is in use; in place of
 * upstream's static stack and k_work_q). */
void *mg_smp_mem_get(size_t align, size_t size);
void mg_smp_mem_put(void *block);
void mg_smp_session_ended(void);

struct smp_session {
	struct k_work_q queue;
	K_KERNEL_STACK_MEMBER(stack, CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_STACK_SIZE);
};

static struct smp_session *smp_session;
static K_MUTEX_DEFINE(smp_session_lock);
static atomic_t smp_bufs_out;

static void smp_session_end(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(smp_session_end_work, smp_session_end);

#ifdef CONFIG_SMP_CLIENT
static sys_slist_t smp_transport_clients = SYS_SLIST_STATIC_INIT(&smp_transport_clients);
#endif

static const struct k_work_queue_config smp_work_queue_config = {
	.name = "mcumgr smp"
};

/* Muse: change 2 (in place of upstream's NET_BUF_POOL_DEFINE() and
 * smp_packet_alloc(), which change 1 replaces below). */
/* The work queue, started if it isn't running; NULL without memory. */
static struct k_work_q *smp_session_queue(void)
{
	struct smp_session *s;

	k_mutex_lock(&smp_session_lock, K_FOREVER);
	s = smp_session;
	if (s == NULL) {
		s = mg_smp_mem_get(__alignof__(struct smp_session), sizeof(*s));
		if (s != NULL) {
			k_work_queue_init(&s->queue);
			k_work_queue_start(&s->queue, s->stack, K_KERNEL_STACK_SIZEOF(s->stack),
					   CONFIG_MCUMGR_TRANSPORT_WORKQUEUE_THREAD_PRIO,
					   &smp_work_queue_config);
			smp_session = s;
		} else {
			LOG_ERR("no memory for the SMP work queue");
		}
	}
	k_mutex_unlock(&smp_session_lock);
	(void)k_work_reschedule(&smp_session_end_work, K_MSEC(CONFIG_MG_SMP_SESSION_IDLE_MS));
	return s != NULL ? &s->queue : NULL;
}

/* System work queue: once idle, stop the queue's thread and free it all. */
static void smp_session_end(struct k_work *work)
{
	struct smp_session *s;

	ARG_UNUSED(work);
	k_mutex_lock(&smp_session_lock, K_FOREVER);
	s = smp_session;
	if (s != NULL && atomic_get(&smp_bufs_out) != 0) {
		/* A request or a response is still about. */
		(void)k_work_reschedule(&smp_session_end_work,
					K_MSEC(CONFIG_MG_SMP_SESSION_IDLE_MS));
		s = NULL;
	} else if (s != NULL) {
		(void)k_work_queue_drain(&s->queue, true);
		if (k_work_queue_stop(&s->queue, K_MSEC(1000)) == 0) {
			smp_session = NULL;
			mg_smp_session_ended(); /* the image writer, mid-upload */
		} else {
			(void)k_work_queue_unplug(&s->queue);
			(void)k_work_reschedule(&smp_session_end_work,
						K_MSEC(CONFIG_MG_SMP_SESSION_IDLE_MS));
			s = NULL;
		}
	}
	k_mutex_unlock(&smp_session_lock);
	if (s != NULL) {
		mg_smp_mem_put(s);
	}
}
/* Muse: end of change 2's functions. */

/* Muse: change 1 (buffers allocated when used). */
static void smp_buf_destroy(struct net_buf *buf);

static uint8_t *smp_buf_data_alloc(struct net_buf *buf, size_t *size, k_timeout_t timeout)
{
	/* Never called: smp_packet_alloc() builds whole buffers. */
	ARG_UNUSED(buf);
	ARG_UNUSED(timeout);
	*size = 0;
	return NULL;
}

static void smp_buf_data_unref(struct net_buf *buf, uint8_t *data)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(data);
}

static const struct net_buf_data_cb smp_buf_data_cb = {
	.alloc = smp_buf_data_alloc,
	.unref = smp_buf_data_unref,
};

static const struct net_buf_data_alloc smp_buf_data = {
	.cb = &smp_buf_data_cb,
};

static STRUCT_SECTION_ITERABLE(net_buf_pool, pkt_pool) =
	NET_BUF_POOL_INITIALIZER(pkt_pool, &smp_buf_data, NULL,
				 CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT,
				 CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE, smp_buf_destroy);

TYPE_SECTION_START_EXTERN(struct net_buf_pool, net_buf_pool);

#define SMP_BUF_UD_SIZE ROUND_UP(CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE, \
				 __alignof__(struct net_buf))

struct net_buf *smp_packet_alloc(void)
{
	struct net_buf *buf;

	if (atomic_inc(&smp_bufs_out) >= CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT) {
		atomic_dec(&smp_bufs_out);
		return NULL;
	}
	buf = mg_smp_mem_get(__alignof__(struct net_buf),
			     sizeof(*buf) + SMP_BUF_UD_SIZE + CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE);
	if (buf == NULL) {
		atomic_dec(&smp_bufs_out);
		return NULL;
	}
	memset(buf, 0, sizeof(*buf) + SMP_BUF_UD_SIZE);
	buf->pool_id = &pkt_pool - TYPE_SECTION_START(net_buf_pool); /* as net_buf does */
	buf->user_data_size = CONFIG_MCUMGR_TRANSPORT_NETBUF_USER_DATA_SIZE;
	buf->flags = 0; /* the data is in this block; unref goes to smp_buf_data_unref() (none) */
	buf->ref = 1;
	buf->__buf = (uint8_t *)buf + sizeof(*buf) + SMP_BUF_UD_SIZE;
	buf->size = CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
	buf->data = buf->__buf;
	buf->len = 0;
#if defined(CONFIG_NET_BUF_POOL_USAGE)
	atomic_dec(&pkt_pool.avail_count);
#endif
	return buf;
}

static void smp_buf_destroy(struct net_buf *buf)
{
	mg_smp_mem_put(buf);
	if (atomic_dec(&smp_bufs_out) == 1) {
		(void)k_work_reschedule(&smp_session_end_work,
					K_MSEC(CONFIG_MG_SMP_SESSION_IDLE_MS));
	}
}
/* Muse: end of change 1. */

void smp_packet_free(struct net_buf *nb)
{
	net_buf_unref(nb);
}

/**
 * @brief Allocates a request buffer.
 *
 * @param arg		The streamer providing the callback.
 * @param priv		The streamer private data.
 *
 * @return	Newly-allocated buffer on success
 *		NULL on failure.
 */
struct net_buf *smp_alloc_req(void *arg, void *priv)
{
	struct net_buf *req_nb;
	struct smp_transport *smpt = arg;

	req_nb = smp_packet_alloc();
	if (req_nb == NULL) {
		return NULL;
	}

	if (smpt->functions.ud_init) {
		smpt->functions.ud_init(req_nb, priv);
	}

	return req_nb;
}

/**
 * @brief Allocates a response buffer.
 *
 * If a source buf is provided, its user data is copied into the new buffer.
 *
 * @param req		An optional source buffer to copy user data from.
 * @param arg		The streamer providing the callback.
 *
 * @return	Newly-allocated buffer on success
 *		NULL on failure.
 */
void *smp_alloc_rsp(const void *req, void *arg)
{
	const struct net_buf *req_nb;
	struct net_buf *rsp_nb;
	struct smp_transport *smpt = arg;

	req_nb = req;

	rsp_nb = smp_packet_alloc();
	if (rsp_nb == NULL) {
		return NULL;
	}

	if (smpt->functions.ud_copy) {
		smpt->functions.ud_copy(rsp_nb, req_nb);
	} else {
		memcpy(net_buf_user_data(rsp_nb),
		       net_buf_user_data((void *)req_nb),
		       req_nb->user_data_size);
	}

	return rsp_nb;
}

void smp_free_buf(void *buf, void *arg)
{
	struct smp_transport *smpt = arg;

	if (!buf) {
		return;
	}

	if (smpt->functions.ud_free) {
		smpt->functions.ud_free(net_buf_user_data((struct net_buf *)buf));
	}

	smp_packet_free(buf);
}

/**
 * Processes a single SMP packet and sends the corresponding response(s).
 */
static int
smp_process_packet(struct smp_transport *smpt, struct net_buf *nb)
{
	struct cbor_nb_reader reader;
	struct cbor_nb_writer writer;
	struct smp_streamer streamer;
	int rc;

	streamer = (struct smp_streamer) {
		.reader = &reader,
		.writer = &writer,
		.smpt = smpt,
	};

	/* Muse: change 5: every request whole CBOR inside its length, none
	 * nesting deeper than MG_SMP_MAX_DEPTH, so zcbor's skip can't recurse
	 * off the work queue's stack. */
	if (!mg_smp_depth_ok(nb->data, nb->len)) {
		LOG_WRN("an SMP request not whole CBOR or nesting deeper than %d: dropped",
			MG_SMP_MAX_DEPTH);
		smp_free_buf(nb, smpt);
		return MGMT_ERR_EINVAL;
	}
	/* Muse: change 3 (the request's connection while its handlers run). */
	memcpy(&req_gen, (uint8_t *)net_buf_user_data(nb) + REQ_GEN_OFF, sizeof(req_gen));
	mg_req_nb = nb; /* Muse: change 4. */
	rc = smp_process_request_packet(&streamer, nb);
	req_gen = 0; /* Muse: change 3. */
	mg_req_nb = NULL; /* Muse: change 4. */
	return rc;
}

/**
 * Processes all received SNP request packets.
 */
static void
smp_handle_reqs(struct k_work *work)
{
	struct smp_transport *smpt;
	struct net_buf *nb;

	smpt = (void *)work;

	/* Read and handle received messages */
	while ((nb = k_fifo_get(&smpt->fifo, K_NO_WAIT)) != NULL) {
		smp_process_packet(smpt, nb);
	}
}

int smp_transport_init(struct smp_transport *smpt)
{
	__ASSERT((smpt->functions.output != NULL),
		 "Required transport output function pointer cannot be NULL");

	if (smpt->functions.output == NULL) {
		return -EINVAL;
	}

#ifdef CONFIG_MCUMGR_TRANSPORT_REASSEMBLY
	smp_reassembly_init(smpt);
#endif

	k_work_init(&smpt->work, smp_handle_reqs);
	k_fifo_init(&smpt->fifo);

	return 0;
}

#ifdef CONFIG_SMP_CLIENT
struct smp_transport *smp_client_transport_get(int smpt_type)
{
	struct smp_client_transport_entry *entry;

	SYS_SLIST_FOR_EACH_CONTAINER(&smp_transport_clients, entry, node) {
		if (entry->smpt_type == smpt_type) {
			return entry->smpt;
		}
	}

	return NULL;
}

void smp_client_transport_register(struct smp_client_transport_entry *entry)
{
	if (smp_client_transport_get(entry->smpt_type)) {
		/* Already in list */
		return;
	}

	sys_slist_append(&smp_transport_clients, &entry->node);

}

#endif /* CONFIG_SMP_CLIENT */

/**
 * @brief Enqueues an incoming SMP request packet for processing.
 *
 * This function always consumes the supplied net_buf.
 *
 * @param smpt                  The transport to use to send the corresponding
 *                                  response(s).
 * @param nb                    The request packet to process.
 */
WEAK void
smp_rx_req(struct smp_transport *smpt, struct net_buf *nb)
{
	/* Muse: change 2. */
	struct k_work_q *queue;
	/* Muse: change 3. */
	uint32_t gen = mg_smp_rx_source_gen();

	memcpy((uint8_t *)net_buf_user_data(nb) + REQ_GEN_OFF, &gen, sizeof(gen));
	k_fifo_put(&smpt->fifo, nb);
	/* Muse: change 2. */
	queue = smp_session_queue();
	if (queue == NULL) {
		smp_rx_clear(smpt);
		return;
	}
	k_work_submit_to_queue(queue, &smpt->work);
}

#ifdef CONFIG_SMP_CLIENT
struct k_work_q *smp_get_wq(void)
{
	/* Muse: change 2. */
	return smp_session_queue();
}
#endif

void smp_rx_remove_invalid(struct smp_transport *zst, void *arg)
{
	struct net_buf *nb;
	struct k_fifo temp_fifo;

	if (zst->functions.query_valid_check == NULL) {
		/* No check check function registered, abort check */
		return;
	}

	/* Cancel current work-queue if ongoing */
	if (k_work_busy_get(&zst->work) & (K_WORK_RUNNING | K_WORK_QUEUED)) {
		k_work_cancel(&zst->work);
	}

	/* Run callback function and remove all buffers that are no longer needed. Store those
	 * that are in a temporary FIFO
	 */
	k_fifo_init(&temp_fifo);

	while ((nb = k_fifo_get(&zst->fifo, K_NO_WAIT)) != NULL) {
		if (!zst->functions.query_valid_check(nb, arg)) {
			smp_free_buf(nb, zst);
		} else {
			k_fifo_put(&temp_fifo, nb);
		}
	}

	/* Re-insert the remaining queued operations into the original FIFO */
	while ((nb = k_fifo_get(&temp_fifo, K_NO_WAIT)) != NULL) {
		k_fifo_put(&zst->fifo, nb);
	}

	/* If at least one entry remains, queue the workqueue for running */
	if (!k_fifo_is_empty(&zst->fifo)) {
		/* Muse: change 2. */
		struct k_work_q *queue = smp_session_queue();

		if (queue != NULL) {
			k_work_submit_to_queue(queue, &zst->work);
		}
	}
}

void smp_rx_clear(struct smp_transport *zst)
{
	struct net_buf *nb;

	/* Cancel current work-queue if ongoing */
	if (k_work_busy_get(&zst->work) & (K_WORK_RUNNING | K_WORK_QUEUED)) {
		k_work_cancel(&zst->work);
	}

	/* Drain the FIFO of all entries without re-adding any */
	while ((nb = k_fifo_get(&zst->fifo, K_NO_WAIT)) != NULL) {
		smp_free_buf(nb, zst);
	}
}

/* Muse: change 2 (no smp_init(): the queue starts with the first request). */
