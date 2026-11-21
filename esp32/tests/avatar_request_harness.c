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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
enum { NORMAL, INLINE, REFUSED, OVERFLOW, RESET, NEGATIVE, CANCEL, TIMEOUT, ALLOC_FAIL, STOPPED, RANGE_EOF };
static int mode, tick, live, opens, cancels;
static int64_t clock_us;
static bool stop_now;
static avatar_request_cb callback,stale;
static void *callback_ctx,*stale_ctx;
static void finish(int status,const char *text,bool end) {
    avatar_request_cb cb=callback;void *ctx=callback_ctx;
    assert(cb);
    /* Like the transport, only the end of the stream drops it. */
    if(end) {callback=NULL;callback_ctx=NULL;}
    cb(ctx,status,(const uint8_t *)text,text?strlen(text):0,end);
}
static int64_t open_request(const char *verb,const char *path,const char *const *headers,
                            bool end,avatar_request_cb cb,void *ctx) {
    (void)headers;assert(!strcmp(verb,"GET")&&!strcmp(path,"/identity")&&end);opens++;
    if(mode==REFUSED)return 0;
    callback=cb;callback_ctx=ctx;
    if(mode==INLINE)finish(200,"abcd",true);
    if(mode==RANGE_EOF)finish(416,NULL,true);
    return 42;
}
static void cancel(int64_t id) {assert(id==42);cancels++;}
static void *allocate(size_t n) {if(mode==ALLOC_FAIL)return NULL;live++;return malloc(n);}
static void release(void *p) {assert(p&&live);live--;free(p);}
static int64_t now(void) {return clock_us;}
static bool stop(uintptr_t gen) {assert(gen==7);return stop_now;}
static void yield(void) {
    tick++;clock_us+=20000;
    if(stale) {avatar_request_cb cb=stale;stale=NULL;cb(stale_ctx,200,(const uint8_t *)"zz",2,true);}
    if(mode==TIMEOUT) {clock_us+=11000000;return;}
    if(mode==RESET) {finish(-1,NULL,true);return;}
    if(mode==NEGATIVE) {finish(-5,"ab",false);return;}
    if(mode==OVERFLOW) {finish(200,"too many bytes",true);return;}
    if(tick==1) {finish(200,"ab",false);if(mode==CANCEL)stop_now=true;}
    else finish(0,"cd",true);
}
static avatar_response_t *request(void) {
    /* ops lives on this stack; a late callback must not retain it. */
    avatar_request_ops_t ops={open_request,cancel,allocate,release,now,yield,stop};
    return avatar_request_get("/identity",NULL,4,7,&ops);
}
static void reset(int scenario) {
    assert(!live&&!callback);mode=scenario;tick=opens=cancels=0;clock_us=0;stop_now=false;
}
int main(void) {
    for(int scenario=NORMAL;scenario<=STOPPED;scenario++) {
        reset(scenario);if(scenario==STOPPED)stop_now=true;
        avatar_response_t *r=request();
        if(scenario==NORMAL||scenario==INLINE) {
            assert(r&&r->len==4&&r->status==200&&!memcmp(r->body,"abcd",4));
            assert(live==1);avatar_request_release(r);
        } else {
            assert(!r);
            if(scenario==CANCEL||scenario==TIMEOUT||scenario==NEGATIVE) {
                /* Freed at once; the transport may still deliver a frame
                 * before it gets to the cancel, which must be dropped. */
                assert(!live&&cancels==1&&callback);
                finish(scenario==CANCEL?-1:0,scenario==CANCEL?NULL:"cd",true);
            }
        }
        assert(!live&&!callback);
    }
    reset(NORMAL);
    avatar_request_ops_t ops={open_request,cancel,allocate,release,now,yield,stop};
    assert(!avatar_request_get("/identity",NULL,0,7,&ops));
    assert(!avatar_request_get("/identity",NULL,8193,7,&ops));
    assert(!opens&&!live);
    reset(RANGE_EOF);
    avatar_response_t *eof=avatar_request_get_status("/identity",NULL,4,7,&ops);
    assert(eof && eof->status==416 && !eof->len);avatar_request_release(eof);
    reset(RANGE_EOF);assert(!request() && !live);
    /* A late frame of a cancelled request can't reach the next one. */
    reset(CANCEL);assert(!request() && !live);
    stale=callback;stale_ctx=callback_ctx;callback=NULL;callback_ctx=NULL;
    reset(NORMAL);
    avatar_response_t *next=request();
    assert(!stale&&next&&next->len==4&&!memcmp(next->body,"abcd",4));avatar_request_release(next);
    assert(!live&&!callback);
    puts("avatar requests: chunked/inline completion, refusal, overflow, reset, negative status, cancellation, timeout, late frames, allocation failure passed");
}
