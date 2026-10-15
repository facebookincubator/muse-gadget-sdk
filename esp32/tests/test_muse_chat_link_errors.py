# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Compile the entire low-memory chat implementation against a fake Link transport."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ChatLinkErrors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        source = (ROOT / 'components/muse/muse_chat_link.c').read_text()
        # Keep all production declarations/functions together. Only platform
        # includes are replaced; the public and shared-helper APIs stay real.
        source = re.sub(r'^#include .*$', '', source, flags=re.MULTILINE)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_compat.h"
#include "cJSON.h"
#include "muse_chat.h"
#include "muse_chat_priv.h"
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGI(tag,...) ((void)(tag))
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int SemaphoreHandle_t;
static int xSemaphoreCreateMutex(void) { return 1; }
static void xSemaphoreTake(int lock, int wait) { (void)lock; (void)wait; }
static void xSemaphoreGive(int lock) { (void)lock; }
static void vTaskDelay(int ticks) { (void)ticks; }
typedef struct { unsigned count, size; unsigned char data[8][80]; } fake_queue_t;
typedef fake_queue_t *QueueHandle_t;
static fake_queue_t queue;
static QueueHandle_t xQueueCreate(unsigned count, unsigned size) {
    assert(count == 8 && size <= 80); queue.size=size; return &queue;
}
static void xQueueReset(QueueHandle_t q) { q->count=0; }
static int xQueueSend(QueueHandle_t q, const void *p, int wait) {
    (void)wait; if(q->count==8) return 0;
    memcpy(q->data[q->count++],p,q->size); return 1;
}
static int xQueueReceive(QueueHandle_t q, void *p, int wait) {
    (void)wait; if(!q->count) return 0; memcpy(p,q->data[0],q->size);
    --q->count; memmove(q->data[0],q->data[1],q->count*sizeof(q->data[0])); return 1;
}
static int64_t now=10000000;
static int64_t esp_timer_get_time(void) { return now; }
static uint32_t esp_random(void) { return 42; }
static bool muse_link_hatch_linked(void) { return true; }
static bool muse_wifi_connected(void) { return true; }
static bool muse_link_req_ready(void) { return true; }
#define MUSE_LINK_REQ_TOO_LARGE (-2)   /* muse_link.h */
typedef void (*frame_fn)(void *,int,const uint8_t *,size_t,bool);
static struct { frame_fn cb; void *ctx; int id; } requests[2];
static int next_id, allocation_attempts, fail_allocation, fail_send;
static size_t largest_reallocation;
static int operations[20], operation_count;
static void operation(int n) { if(operation_count<20) operations[operation_count++]=n; }
static void *test_malloc(size_t n) {
    allocation_attempts++;
    if(fail_allocation) { fail_allocation--; return NULL; }
    return malloc(n);
}
static void *test_realloc(void *p,size_t n) {
    allocation_attempts++;
    if(n>largest_reallocation) largest_reallocation=n;
    if(fail_allocation) { fail_allocation--; return NULL; }
    return realloc(p,n);
}
static int64_t muse_link_req_open(const char *verb,const char *path,const char *const *headers,
                                 bool end,frame_fn cb,void *ctx) {
    assert(!strcmp(verb,"POST") && !end);
    int slot=!strcmp(path,"/chat/subscribe");
    assert(slot || !strcmp(path,"/chat/stream"));
    if(slot) {
        bool ndjson=false;
        for(int i=0;headers[i];i+=2) if(!strcmp(headers[i],"Accept")) ndjson=!strcmp(headers[i+1],"application/x-ndjson");
        assert(ndjson);
    }
    requests[slot].cb=cb; requests[slot].ctx=ctx; requests[slot].id=++next_id;
    operation(slot ? 3 : 1); return next_id;
}
static bool muse_link_req_send(int64_t id,const void *data,size_t len,bool end,int wait) {
    (void)wait;
    if(fail_send) { fail_send--; return false; }
    if(id==requests[1].id) { assert(end && len==2 && !memcmp(data,"{}",2)); operation(4); }
    else {
        assert(id==requests[0].id);
        if(end) operation(5);
        else if(len>20 && ((const char*)data)[0]=='{') {
            assert(strstr(data,"\"output_modality\":\"text\"")); operation(2);
        }
    }
    return true;
}
static void muse_link_req_cancel(int64_t id) { (void)id; }
void muse_hatch_wav_header(uint8_t p[MUSE_HATCH_WAV_HEADER],uint32_t rate) {
    assert(rate==16000); memset(p,0,MUSE_HATCH_WAV_HEADER);
}
size_t muse_hatch_base64(const uint8_t *p,size_t n,char *out) {
    (void)p; size_t len=(n+2)/3*4; memset(out,'A',len); return len;
}
void muse_hatch_tail_words(const char *text,char *out,size_t cap) { strlcpy(out,text,cap); }
bool muse_hatch_caption_at(const char *text,size_t at,char *out,size_t cap) {
    (void)at; strlcpy(out,text,cap); return text[0]!=0;
}
#define malloc test_malloc
#define realloc test_realloc
''' + source + r'''
#undef malloc
#undef realloc
static void feed(int slot,int status,const char *p,size_t n,bool end) {
    requests[slot].cb(requests[slot].ctx,status,(const uint8_t*)p,n,end);
}
static void sub(const char *p) { feed(RX_SUB,200,p,strlen(p),false); }
static void bytewise(const char *p) {
    for(size_t i=0;i<strlen(p);i++) feed(RX_SUB,200,p+i,1,false);
}
static void ack(const char *body) { feed(RX_NOTE,200,body,strlen(body),true); pump(); }
static void note_ack(void) { ack("{\"result\":{\"message_id\":\"note\",\"reply_to_message_id\":\"parent\"}}"); }
static void begin(void) {
    muse_hatch_turn_begin(); assert(s_turn.phase==T_TALKING);
    pump(); assert(s_turn.phase==T_TALKING);
}
static void release(void) { muse_hatch_turn_end(); assert(s_turn.phase==T_ACK); }
static void final(const char *id,const char *parent,const char *text) {
    char line[2048];
    snprintf(line,sizeof(line),"{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"%s\",\"reply_to_message_id\":\"%s\",\"display_text\":\"%s\"}}\n",id,parent,text);
    sub(line);
}
static void assert_replied(const char *text) {
    pump(); assert(s_turn.phase==T_REPLY && s_turn.replied); assert(!strcmp(s_turn.text,text));
}
static void streaming(void) {
    begin(); assert(operation_count==2 && operations[0]==1 && operations[1]==2);
    assert(!s_stream[RX_SUB]); release();
    assert(operation_count==5 && operations[2]==3 && operations[3]==4 && operations[4]==5);
    sub("{\"type\":\"event\",\"seq\":100,\"event\":\"agent.status\",\"payload\":{\"status\":\"idle\"}}\n");
    const char *events=
        "{\"type\":\"subscribed\"}\n"
        "{\"type\":\"event\",\"seq\":101,\"event\":\"delta.text_append\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"message_id\":\"reply\",\"parent_message_id\":\"note\",\"display_text\":null,\"content\":null,\"text\":\"Hello \"}}\n"
        "{\"type\":\"event\",\"seq\":102,\"event\":\"delta.text_append\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"type\":\"text\",\"message_id\":\"reply\",\"text\":\"world\"}}\n"
        "{\"type\":\"event\",\"seq\":103,\"event\":\"delta.message_done\",\"payload\":{\"seq\":1,\"event_name\":\"message.assistant\",\"message_id\":\"reply\"}}\n";
    bytewise(events); assert(s_pending_count==1 && !s_turn.replied);
    assert(!strcmp(s_pending[0].text,"Hello world"));
    final("reply","note","Hello world"); assert(s_pending_count==1);
    note_ack(); assert_replied("Hello world");
    final("reply","note","Hello world"); assert_replied("Hello world");
    char text[72]; assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_SENT);
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_REPLY);
    now+=20000000; pump(); assert(s_turn.phase==T_IDLE);
}
static void correlation(void) {
    begin(); release();
    for(int i=0;i<5;i++) { char id[10]; snprintf(id,sizeof(id),"other%d",i); final(id,"elsewhere","Other chat"); }
    assert(s_pending_count==2); /* bounded provisional candidates, no fatal flag */
    final("reply","note","Early reply"); note_ack(); assert_replied("Early reply");
    for(int i=0;i<5;i++) { char id[10]; snprintf(id,sizeof(id),"other%d",i); final(id,"elsewhere","Other chat"); }
    assert(!s_pending_count); assert_replied("Early reply");
    final("second","parent","Second"); assert_replied("Early reply Second");
    final("unparented","","Live"); assert_replied("Early reply Second Live");
    muse_hatch_turn_cancel(); begin(); release();
    final("evicted","note","Too early"); final("other1","elsewhere","Other"); final("other2","elsewhere","Other");
    note_ack(); assert(!s_turn.replied && s_turn.phase==T_REPLY);
    now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"BUFFER LIMIT"));
    begin(); assert(!s_early_evicted); release(); note_ack();
    final("fresh","note","Recovered"); assert_replied("Recovered");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    final("first","note","First"); final("second","note","Second"); final("third","note","Third");
    assert(s_pending_count==2 && !s_early_evicted);
    assert(!strcmp(s_pending[0].msg,"second") && !strcmp(s_pending[1].msg,"third"));
    assert_replied("Second Third"); /* newest two finals between voice-task polls */
}

static void delta(const char *event,const char *id,const char *parent,const char *text) {
    char line[2048];
    snprintf(line,sizeof(line),"{\"type\":\"event\",\"event\":\"delta.%s\",\"payload\":{\"message_id\":\"%s\",\"parent_message_id\":\"%s\",\"text\":\"%s\"}}\n",event,id,parent,text);
    sub(line);
}
static void rejected_deltas(void) {
    begin(); release(); note_ack();
    delta("message_start","other","elsewhere","");
    delta("text_append","other","","Wrong reply");
    delta("message_done","other","","");
    final("other","","Wrong final");
    pump(); assert(!s_pending_count && !s_turn.replied && s_turn.phase==T_REPLY);
    assert(!s_delta.text[0]);
    delta("message_start","reply","note","");
    delta("text_append","reply","","Our reply");
    delta("message_done","reply","","");
    assert_replied("Our reply");
}
static void rejected_before_ack(void) {
    begin(); release();
    delta("message_start","other","elsewhere","");
    note_ack();
    delta("text_append","other","","Wrong reply");
    delta("message_done","other","","");
    pump(); assert(!s_pending_count && !s_turn.replied);
    final("reply","note","Our reply"); assert_replied("Our reply");
    muse_hatch_turn_cancel(); begin(); release();
    final("other","elsewhere","Early wrong final"); note_ack();
    final("other","","Wrong final repeated");
    pump(); assert(!s_pending_count && !s_turn.replied);
}

static void bounded_rejections(void) {
    begin(); release(); note_ack();
    delta("message_start","reply","note","");
    char id[20];
    for(int i=0;i<9;i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id,sizeof(id),"other%d",i);
        delta("message_start",id,"elsewhere","");
    }
    final("other0","","Wrong final"); final(id,"","Overflow final");
    pump(); assert(!s_pending_count && !s_turn.replied);
    delta("text_append","reply","","Our reply");
    delta("message_done","reply","","");
    assert_replied("Our reply");
    final("second","note","Second"); assert_replied("Our reply Second");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    final("other0","","New turn"); assert_replied("New turn"); /* reset remembered IDs and overflow */
}
static void oversized(void) {
    begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"seq\":1,\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"reply\",\"parent_message_id\":\"note\",\"text\":\"Retained text\"}}\n");
    sub("{\"metadata\":{\"nested\":[{\"text\":\"");
    for(int i=0;i<5000;i++) sub("x");
    bytewise("\\\"escaped\\\\text\"}]},\"payload\":{\"message_id\":\"reply\"},\"type\":\"event\",\"seq\":2,\"event\":\"delta.message_done\"}\n");
    assert(largest_reallocation<=ROW_MAX); assert_replied("Retained text");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"long\",\"display_text\":\"");
    for(int i=0;i<5000;i++) sub("x");
    sub("\",\"reply_to_message_id\":\"note\"}}\n");
    pump(); assert(s_turn.replied && strlen(s_turn.text)==TEXT_MAX-1);
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    for(int i=0;i<ROW_MAX+1;i++) sub("x");
    assert(s_skipped_big && s_rx[RX_SUB].overflow && largest_reallocation<=ROW_MAX);
    sub("\n{\"type\":\"subscribed\"}\r\n");
    assert(!s_rx[RX_SUB].body && !s_rx[RX_SUB].overflow);
    final("after","note","After oversized line"); assert_replied("After oversized line");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    fail_allocation=1; sub("{\"type\":"); sub("\"event\"}\n");
    assert(s_skipped_big && !s_rx[RX_SUB].body);
    final("oom","note","After allocation failure"); assert_replied("After allocation failure");
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"last\",\"display_text\":\"Terminal line\"}}");
    feed(RX_SUB,200,NULL,0,true); assert_replied("Terminal line");
}
static void unicode_and_fields(void) {
    begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"message_id\":\"envelope\",\"payload\":{\"type\":\"ignored\",\"message_id\":\"reply\",\"id\":\"fallback\",\"reply_to_message_id\":\"note\",\"parent_message_id\":\"elsewhere\",\"display_text\":\"Hi \\uD83D\\uDE00 café \\\"yes\\\"\\nnext\",\"content\":\"wrong\",\"text\":\"wrong\",\"metadata\":{\"display_text\":\"also wrong\"}},\"message_id\":\"late-envelope\"}\n");
    assert_replied("Hi 😀 café \"yes\"\nnext");
    assert(!strcmp(s_turn.seen[0],"reply"));
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    sub("{\"type\":\"event\",\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"long\",\"text\":\"");
    for(int i=0;i<TEXT_MAX-3;i++) sub("a");
    sub("\"}}\n");
    bytewise("{\"type\":\"event\",\"event\":\"delta.text_append\",\"payload\":{\"message_id\":\"long\",\"text\":\"😀tail\"}}\n");
    sub("{\"type\":\"event\",\"event\":\"delta.message_done\",\"payload\":{\"message_id\":\"long\"}}\n");
    pump(); assert(s_turn.replied && strlen(s_turn.text)==TEXT_MAX-3);
    muse_hatch_turn_cancel(); begin(); release(); note_ack();
    bytewise("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"bad\",\"text\":\"\\uD800oops\"}}\n");
    assert(!s_pending_count); final("good","note","Recovered"); assert_replied("Recovered");
}
static void retry_and_stale(void) {
    fail_allocation=1; muse_hatch_turn_begin(); assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"MEMORY"));
    begin(); release();
    char big[ACK_MAX+1]; memset(big,'x',sizeof(big)); feed(RX_NOTE,200,big,sizeof(big),true);
    pump(); assert(s_turn.phase==T_IDLE && s_turn.error[0]);
    begin(); release();
    sub("{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"text\":\"unfinished");
    frame_fn old=requests[RX_SUB].cb; void *old_ctx=requests[RX_SUB].ctx;
    muse_hatch_turn_cancel(); begin();
    old(old_ctx,403,NULL,0,true); pump(); assert(s_turn.phase==T_TALKING);
    fail_send=1; int16_t pcm[1024]={0}; muse_hatch_turn_audio(pcm,1024);
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"KEEP UP"));
    begin(); release(); note_ack(); final("reply","note","Retry works"); assert_replied("Retry works");
}
static void denied_and_ack(void) {
    int statuses[]={403,401,404,-1,-2}; const char *errors[]={"ACCESS DENIED (403)","AUTH REQUIRED (401)","HTTP 404","LOST CONNECTION","REPLY TOO LONG"};
    for(unsigned i=0;i<5;i++) {
        begin(); release(); feed(RX_SUB,statuses[i],NULL,0,true); pump();
        assert(s_turn.phase==T_IDLE && strstr(s_turn.error,errors[i]));
    }
    begin(); release(); feed(RX_NOTE,-2,NULL,0,true); pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"REPLY TOO LONG"));
    begin(); release(); ack("{}"); assert(s_turn.phase==T_IDLE && s_turn.error[0]);
    begin(); release(); feed(RX_SUB,200,NULL,0,true); note_ack();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"LOST CONNECTION"));
    begin(); release(); note_ack(); now+=REPLY_TIMEOUT_US+1; pump();
    assert(s_turn.phase==T_IDLE && strstr(s_turn.error,"NO REPLY"));
    begin(); release(); note_ack(); final("retry","note","Recovered"); assert_replied("Recovered");
}
static void transcript_and_lines(void) {
    begin(); release();
    int before=allocation_attempts;
    sub("{\"type\":\"event\",\"event\":\"message.user\",\"payload\":{\"message_id\":\"note\",\"display_text\":\"Hello\\n[file:audio/wav note]\"}}\r\n"
        "{\"type\":\"event\",\"event\":\"message.assistant\",\"payload\":{\"message_id\":\"reply\",\"reply_to_message_id\":\"note\",\"display_text\":\"Hi\"}}\n");
    assert(allocation_attempts==before); /* complete lines parse in Link's buffer */
    note_ack(); assert_replied("Hi");
    char text[72]; assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_SENT);
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_HEARD && !strcmp(text,"Hello"));
    assert(muse_hatch_turn_event(text,sizeof(text))==MUSE_HATCH_EV_REPLY);
}
static void text_modality(void) {
    cJSON *text=cJSON_Parse(MUSE_HATCH_NOTE_HEAD MUSE_HATCH_NOTE_TAIL);
    assert(text);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(text,"output_modality")),"text"));
    cJSON_Delete(text);
}
int main(int argc,char **argv) {
    assert(argc==2); muse_hatch_start();
    switch(atoi(argv[1])) {
    case 0: streaming(); break;
    case 1: correlation(); break;
    case 2: oversized(); break;
    case 3: unicode_and_fields(); break;
    case 4: retry_and_stale(); break;
    case 5: denied_and_ack(); break;
    case 6: text_modality(); break;
    case 7: transcript_and_lines(); break;
    case 8: rejected_deltas(); break;
    case 9: rejected_before_ack(); break;
    case 10: bounded_rejections(); break;
    default: return 2;
    }
    muse_hatch_turn_cancel();
    return 0;
}
'''
        c = Path(cls.tmp.name) / 'chat.c'
        c.write_text(code)
        cls.binary = c.with_suffix('')
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        compiled = subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
            '-I', str(cjson), '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
            str(c), str(cjson / 'cJSON.c'), '-o', str(cls.binary)
        ], capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, number):
        subprocess.run([str(self.binary), str(number)], check=True)

    def test_fragmented_early_reply_ack_order_and_deduplication(self):
        self.run_case(0)

    def test_unrelated_events_do_not_fill_reply_queue(self):
        self.run_case(1)

    def test_large_final_metadata_and_caption_use_bounded_receive_memory(self):
        self.run_case(2)

    def test_field_precedence_escaping_and_utf8_boundaries(self):
        self.run_case(3)

    def test_retry_after_oom_overflow_send_failure_and_cancelled_frames(self):
        self.run_case(4)

    def test_authorization_failures_closed_stream_and_timeout_recover(self):
        self.run_case(5)

    def test_shared_voice_note_format_requests_text_replies(self):
        self.run_case(6)

    def test_transcript_and_multiple_complete_lines_need_no_receive_allocation(self):
        self.run_case(7)

    def test_rejected_message_deltas_do_not_complete_turn(self):
        self.run_case(8)

    def test_ack_rejection_remembers_pending_and_partial_message_ids(self):
        self.run_case(9)

    def test_rejection_capacity_preserves_correlation_and_resets(self):
        self.run_case(10)
