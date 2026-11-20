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
typedef struct {
    avatar_response_t response;
    atomic_uint refs;
    atomic_bool done;
    size_t cap;
    void (*free)(void *);
    uint8_t data[];
} pending_t;
void avatar_request_release(avatar_response_t *response) {
    if (!response) return;
    pending_t *p = (pending_t *)response;
    if (atomic_fetch_sub(&p->refs, 1) == 1) p->free(p);
}
static void reply(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    pending_t *p = ctx;
    avatar_response_t *r = &p->response;
    if (status) r->status = status;
    if (len > p->cap - r->len) r->overflow = true;
    if (!r->overflow && len) { memcpy(r->body + r->len, data, len); r->len += len; }
    if (end || status < 0) { atomic_store(&p->done, true); avatar_request_release(r); }
}
avatar_response_t *avatar_request_get_status(const char *path, const char *const *headers,
                                     size_t cap, uintptr_t generation, const avatar_request_ops_t *ops) {
    if (!path || !cap || cap > 8192 || ops->stop(generation)) return NULL;
    pending_t *p = ops->alloc(sizeof(*p) + cap);
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->cap = cap; p->free = ops->free; p->response.body = p->data;
    atomic_init(&p->refs, 2);
    atomic_init(&p->done, false);
    int64_t id = ops->open("GET", path, headers, true, reply, p);
    if (!id) { avatar_request_release(&p->response); avatar_request_release(&p->response); return NULL; }
    int64_t deadline = ops->now_us() + 10000000;
    while (!atomic_load(&p->done)) {
        if (ops->stop(generation) || ops->now_us() > deadline) {
            ops->cancel(id); avatar_request_release(&p->response); return NULL;
        }
        ops->yield();
    }
    avatar_response_t *r = &p->response;
    if (ops->stop(generation) || r->overflow || (r->status < 100 || r->status > 599)) {
        avatar_request_release(r); return NULL;
    }
    return r;
}

avatar_response_t *avatar_request_get(const char *path,const char *const *headers,size_t cap,
                                     uintptr_t generation,const avatar_request_ops_t *ops) {
    avatar_response_t *r=avatar_request_get_status(path,headers,cap,generation,ops);
    if (r && r->status!=200 && r->status!=206) { avatar_request_release(r); return NULL; }
    return r;
}
