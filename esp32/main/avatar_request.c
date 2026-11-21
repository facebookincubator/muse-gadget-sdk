/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "avatar_request.h"
#include <stdatomic.h>
#include <string.h>
/* The transport drops a cancelled stream without a last callback, so it can't
 * own the buffer. Its ctx names a slot and that slot's sequence number; the
 * waiter owns the buffer and moves the sequence on before freeing it, so a
 * late or post-cancel frame finds a stale sequence and is dropped. */
#define SLOTS 4u
#define SEQ_MASK 0x3fffffffu
typedef struct {
    avatar_response_t response;
    atomic_bool done;
    bool ended;
    size_t cap;
    void (*free)(void *);
    uint8_t data[];
} pending_t;
/* word = seq << 1, plus 1 while a callback writes into p. */
static struct { atomic_uint word; _Atomic(pending_t *) p; } s_slot[SLOTS];
void avatar_request_release(avatar_response_t *response) {
    if (response) ((pending_t *)response)->free(response);
}
static void reply(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    uintptr_t v = (uintptr_t)ctx;
    unsigned idle = (unsigned)(v >> 2) << 1;
    if (!atomic_compare_exchange_strong(&s_slot[v % SLOTS].word, &idle, idle | 1)) return;
    pending_t *p = atomic_load(&s_slot[v % SLOTS].p);
    avatar_response_t *r = &p->response;
    if (!atomic_load(&p->done)) {
        if (status) r->status = status;
        if (len > p->cap - r->len) r->overflow = true;
        if (!r->overflow && len) { memcpy(r->body + r->len, data, len); r->len += len; }
        /* A negative status fails the request even if the stream stays open. */
        if (end || status < 0) { p->ended = end; atomic_store(&p->done, true); }
    }
    atomic_store(&s_slot[v % SLOTS].word, idle);
}
avatar_response_t *avatar_request_get_status(const char *path, const char *const *headers,
                                     size_t cap, uintptr_t generation, const avatar_request_ops_t *ops) {
    if (!path || !cap || cap > 8192 || ops->stop(generation)) return NULL;
    pending_t *p = ops->alloc(sizeof(*p) + cap);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->cap = cap; p->free = ops->free; p->response.body = p->data;
    atomic_init(&p->done, false);
    unsigned i = 0;
    for (pending_t *none = NULL; i < SLOTS; i++, none = NULL)
        if (atomic_compare_exchange_strong(&s_slot[i].p, &none, p)) break;
    if (i == SLOTS) { p->free(p); return NULL; }
    unsigned seq = atomic_load(&s_slot[i].word) >> 1;
    int64_t id = ops->open("GET", path, headers, true, reply, (void *)(uintptr_t)(seq << 2 | i));
    int64_t deadline = ops->now_us() + 10000000;
    bool stopped = false;
    while (id && !atomic_load(&p->done)) {
        if (ops->stop(generation) || ops->now_us() > deadline) { stopped = true; break; }
        ops->yield();
    }
    /* Waits out a callback mid-copy; after this no frame reaches p. */
    for (unsigned idle = seq << 1;
         !atomic_compare_exchange_strong(&s_slot[i].word, &idle, ((seq + 1) & SEQ_MASK) << 1); idle = seq << 1)
        ops->yield();
    atomic_store(&s_slot[i].p, NULL);
    if (id && !p->ended) ops->cancel(id);
    avatar_response_t *r = &p->response;
    if (!id || stopped || !p->ended || ops->stop(generation) || r->overflow || r->status < 100 || r->status > 599) {
        p->free(p); return NULL;
    }
    return r;
}

avatar_response_t *avatar_request_get(const char *path,const char *const *headers,size_t cap,
                                     uintptr_t generation,const avatar_request_ops_t *ops) {
    avatar_response_t *r=avatar_request_get_status(path,headers,cap,generation,ops);
    if (r && r->status!=200 && r->status!=206) { avatar_request_release(r); return NULL; }
    return r;
}
