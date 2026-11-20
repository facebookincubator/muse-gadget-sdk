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
enum { NORMAL, INLINE, REFUSED, OVERFLOW, RESET, CANCEL, TIMEOUT, ALLOC_FAIL, STOPPED, RANGE_EOF };
static int mode, tick, live, opens, cancels;
static int64_t clock_us;
static bool stop_now;
static avatar_request_cb callback;
static void *callback_ctx;
static void finish(int status,const char *text,bool end) {
    avatar_request_cb cb=callback;void *ctx=callback_ctx;
    assert(cb);
    if(end||status<0) {callback=NULL;callback_ctx=NULL;}
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
    if(mode==TIMEOUT) {clock_us+=11000000;return;}
    if(mode==RESET) {finish(-1,NULL,false);return;}
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
            if(scenario==CANCEL||scenario==TIMEOUT) {
                assert(live==1&&cancels==1&&callback);
                /* Cancellation returns before transport relinquishes ctx. */
                finish(scenario==CANCEL?-1:200,scenario==CANCEL?NULL:"cd",true);
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
    puts("avatar requests: chunked/inline completion, refusal, overflow, reset, delayed cancellation, timeout, allocation failure passed");
}
