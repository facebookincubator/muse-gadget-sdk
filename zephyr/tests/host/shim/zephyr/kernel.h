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

/* Host shim: just enough of the Zephyr kernel API for the portable modules.
 * Delayed work runs only when a test calls host_run_due() (fake clock). */

#ifndef HOST_SHIM_KERNEL_H_
#define HOST_SHIM_KERNEL_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/sys/util.h>

typedef struct {
	int64_t ms;
} k_timeout_t;
#define K_MSEC(x)   ((k_timeout_t){.ms = (x)})
#define K_SECONDS(x) ((k_timeout_t){.ms = (x) * 1000})
#define K_FOREVER   ((k_timeout_t){.ms = -1})
#define K_NO_WAIT   ((k_timeout_t){.ms = 0})

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);
struct k_work {
	k_work_handler_t handler;
};
struct k_work_delayable {
	struct k_work work;
	bool pending;
	int64_t due;
};
struct k_work_q {
	int unused;
};

void k_work_init_delayable(struct k_work_delayable *dw, k_work_handler_t handler);
int k_work_reschedule(struct k_work_delayable *dw, k_timeout_t delay);
int k_work_reschedule_for_queue(struct k_work_q *q, struct k_work_delayable *dw,
				k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *dw);
int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);

/* Work items: a submitted item runs at once (the queue's thread, here the
 * caller); one without a handler is counted and never run, as an item that
 * was never initialized would fault on the target. */
#define K_WORK_DEFINE(name, fn) struct k_work name = {.handler = (fn)}
static inline void k_work_init(struct k_work *w, k_work_handler_t handler)
{
	w->handler = handler;
}
int k_work_submit_to_queue(struct k_work_q *q, struct k_work *w);
extern int host_null_work_submits;

/* Message queues: a fixed ring of fixed-size messages. */
struct k_msgq {
	size_t msg_size;
	uint32_t max_msgs;
	uint32_t head, used;
	char *buf;
};
#define K_MSGQ_DEFINE(name, size, max, align)                                                     \
	char host_msgq_buf_##name[(size) * (max)];                                                \
	struct k_msgq name = {(size), (max), 0, 0, host_msgq_buf_##name}
int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);

/* Atomics: single-threaded here. */
typedef long atomic_t;
typedef long atomic_val_t;
#define ATOMIC_INIT(v) (v)
static inline atomic_val_t atomic_get(const atomic_t *a)
{
	return *a;
}
static inline atomic_val_t atomic_set(atomic_t *a, atomic_val_t v)
{
	atomic_val_t old = *a;

	*a = v;
	return old;
}
static inline atomic_val_t atomic_inc(atomic_t *a)
{
	return (*a)++;
}

struct k_mutex {
	int unused;
};
#define K_MUTEX_DEFINE(name) struct k_mutex name
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t)
{
	(void)m;
	(void)t;
	return 0;
}
static inline int k_mutex_unlock(struct k_mutex *m)
{
	(void)m;
	return 0;
}

/* Test side. */
extern int64_t host_now_ms;
/* Advances the clock by ms, running every delayed work that falls due. */
void host_advance(int64_t ms);

#endif
