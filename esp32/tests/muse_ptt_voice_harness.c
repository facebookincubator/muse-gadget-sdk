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
/* Includes the entire, unmodified production translation unit. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include "muse_voice.c"
#define TAG INPUT_TAG
#define s_queue input_queue
#include "muse_input.c"
#undef s_queue
#undef TAG

struct test_queue { muse_input_event_t items[64]; int n; } queue;
static int scenario, frame, starts, ends, allocations, notes, writes, delivered, tail_reads;
static int64_t clock_us;
static int talk_edges;
static bool talk_values[64];
static int64_t talk_times[64];
static int64_t accepted_at[2048];
static void overflow_input(muse_ptt_t type)
{
    while(queue.n<16) post(MUSE_PTT_DOWN,false);
    post(type,false);
    assert(queue.n==16 && muse_input_call_epoch()==1);
}
static bool held, ready, ended_event, ready_event, hold_event, resume_event, notes_mode;
static bool fail_read, fail_accept, hold_status, reply_done, terminate_task;
static bool need_end, sleep_state, input_context;
static int mic_at[128], speaker_at[128];
static float level_at[128], level;
static muse_voice_view_t views[128];
static muse_mode_t mode;
static char caption[400];
static jmp_buf finish;
static int injected_pending;   /* 1: an injected press is held back, delivered by deliver_injected() */
static muse_input_event_t injected;
static void deliver_injected(void) {
    if (injected_pending==1) { injected_pending=2; queue.items[queue.n++]=injected; }
}

static void push(muse_ptt_t type) { muse_input_event_t e = {.type=type, .epoch=muse_input_call_epoch()}; xQueueSend(&queue, &e, 0); }
BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t t) {
    (void)t;
    if (q->n == (scenario>=30 && scenario<=34 ? 16 : 64)) return 0;
    q->items[q->n++] = *(const muse_input_event_t *)p;
    injected = *(const muse_input_event_t *)p;
    return pdTRUE;
}
BaseType_t xQueueSendToFront(QueueHandle_t q, const void *p, TickType_t t) {
    (void)t; assert(q->n < 64); memmove(q->items+1,q->items,q->n++*sizeof(q->items[0]));
    q->items[0]=*(const muse_input_event_t *)p; return pdTRUE;
}
/* A talk press the input task posts at a chosen moment: the first time the
 * voice task finds the queue empty (inject_armed), i.e. between its check and
 * whatever it does next. */
static bool inject_armed;
static void inject_if_empty(QueueHandle_t q) {
    if (inject_armed && !q->n) { inject_armed=false; post(MUSE_PTT_DOWN,false); --q->n; injected_pending=1; }
}
BaseType_t xQueuePeek(QueueHandle_t q, void *p, TickType_t t) {
    (void)t; inject_if_empty(q); if (!q->n) return 0; *(muse_input_event_t *)p=q->items[0]; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t t) {
    if (!xQueuePeek(q,p,t)) { deliver_injected(); return 0; }
    --q->n;
    memmove(q->items,q->items+1,q->n*sizeof(q->items[0]));
    return pdTRUE;
}
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(int ms) { clock_us += (int64_t)ms*1000; }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; ++allocations; return malloc(n); }
void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p,n); }
void muse_state_set_mode(muse_mode_t m) {
    mode=m;
    if (frame<128) { views[frame]=muse_voice_call_view(); level_at[frame]=level; }
}
muse_mode_t muse_state_mode(float *s) { if(s) *s=0; return mode; }
void muse_state_set_level(float v) {
    level=v;
    if(frame<128) level_at[frame]=v;
}
void muse_state_set_progress(float v) { (void)v; }
void muse_state_set_caption(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(caption,sizeof(caption),fmt,ap); va_end(ap);
}
void muse_state_poke(void) {}
void muse_state_nudge(void) {}
void muse_state_make_happy(void) {}
bool muse_state_asleep(void) { return sleep_state; }
bool muse_state_on_battery(void) { return false; }
void muse_state_wait_awake(uint32_t ms) { vTaskDelay(ms); }
bool muse_settings_voice_notes(void) { return notes_mode; }
int muse_settings_volume(void) { return 20; }
int muse_settings_mic_gain(void) { return 30; }
bool muse_wifi_connected(void) { return ready; }
void muse_wifi_apply(void) {}
void muse_wifi_power(muse_wifi_power_t p) { (void)p; }
bool muse_hatch_ready(void) { return ready; }
void muse_hatch_status(muse_hatch_status_t *s) { s->state=ready?MUSE_HATCH_REACHABLE:MUSE_HATCH_OFFLINE; }
void muse_hatch_set_resting(bool v) { (void)v; }
void muse_audio_power(bool v) { (void)v; }
void muse_audio_selftest(void) {}
void muse_audio_chirp(int v) { (void)v; }
void muse_audio_loopback_test(int v) { (void)v; }
size_t muse_hatch_mp3_selftest(int16_t **p) { *p=NULL; return 0; }
float muse_audio_dbfs(const int16_t *p,size_t n) { (void)n; return (float)p[0]/100-50; }
float muse_audio_level(const int16_t *p,size_t n) { assert(n); return (float)p[0]/1000; }

/* Legacy path is a tripwire, not another implementation of the note engine. */
void muse_hatch_turn_begin(void) { ++notes; if(scenario>=20) longjmp(finish,2); assert(!"PTT opened a note"); }
void muse_hatch_turn_audio(const int16_t *p,size_t n) { (void)p;(void)n; }
size_t muse_hatch_turn_audio_wait(const int16_t *p,size_t n,int ms) { (void)p;(void)ms;return n; }
void muse_hatch_turn_end(void) {}
void muse_hatch_turn_cancel(void) {}
muse_hatch_ev_t muse_hatch_turn_event(char *p,size_t n) { (void)p;(void)n;return MUSE_HATCH_EV_NONE; }
bool muse_hatch_turn_caption(size_t x,char *p,size_t n) { (void)x;(void)p;(void)n;return false; }
size_t muse_hatch_turn_read(int16_t *p,size_t n,int ms) { (void)p;(void)n;(void)ms;return 0; }

#if CONFIG_MUSE_REALTIME_VOICE
static int pending_at_ready = -1;
bool muse_voice_call_ready(void) { pending_at_ready = (atomic_load(&s_call_pending)!=0); return ready; }
void muse_voice_call_start(void) { ++starts; held=false; }
void muse_voice_call_start_ptt(void) {
    ++starts; held=true;
    if(scenario==33) overflow_input(MUSE_PTT_UP);
}
void muse_voice_call_set_talking(bool v) {
    assert(!input_context);
    assert(talk_edges<64);
    talk_values[talk_edges]=v; talk_times[talk_edges++]=clock_us;
    held=v;
    if(v) reply_done=false;
}
void muse_voice_call_end(void) {
    assert(!input_context);
    ++ends; need_end=true; held=false;
}
void muse_voice_call_ptt_status(muse_voice_call_ptt_status_t *s) {
    *s=(muse_voice_call_ptt_status_t){.connecting=!ready_event,.live=ready_event,.talking=held,
        .capture=held&&!need_end&&!hold_status,.hold=hold_status,
        .sending=delivered>0 && frame<(scenario==1?18:14),.heard=scenario==1,
        .reply_done=reply_done};
}
bool muse_voice_call_audio(const int16_t *p,size_t n) {
    assert(n==MUSE_AUDIO_CHUNK);
    assert(p[0] == (scenario==17 ? 101+delivered : 100+frame));
    if (muse_voice_ptt_call()) assert(held&&!need_end&&!hold_status);
    if(fail_accept) return false;
    accepted_at[delivered++]=clock_us;
    if(frame<128) ++mic_at[frame];
    return true;
}
size_t muse_voice_call_read(int16_t *p,size_t n,bool *flushed) {
    *flushed=false;
    assert(!muse_voice_ptt_call() || !held);
    if(scenario==41 && ready_event && !held && tail_reads<150) {
        ++tail_reads;
        for(size_t i=0;i<n;i++) p[i]=25000;
        return n;
    }
    if (ended_event && scenario>=12 && scenario<=14 && (tail_reads<3 || scenario==14)) {
        ++tail_reads;
        for(size_t i=0;i<n;i++) p[i]=25000;
        return n;
    }
    if ((scenario==1 && frame==6) || (scenario==5 && frame==1)) {
        for(size_t i=0;i<n;i++) p[i]=25000;
        return n;
    }
    return 0;
}
muse_voice_call_ev_t muse_voice_call_event(char *p,size_t n) {
    if (!ready_event && frame>=2) { ready_event=true; return MUSE_VOICE_CALL_EV_READY; }
    if(scenario==41) {
        if(tail_reads==150 && !ended_event) {
            ended_event=true; snprintf(p,n,"MUSE ENDED THE CALL"); return MUSE_VOICE_CALL_EV_ENDED;
        }
        return MUSE_VOICE_CALL_EV_NONE;
    }
    if (scenario==1 && frame==11 && !hold_event) { hold_event=true; hold_status=true; return MUSE_VOICE_CALL_EV_HOLD; }
    if (scenario==1 && frame==12 && !resume_event) { resume_event=true; hold_status=false; return MUSE_VOICE_CALL_EV_RESUME; }
    if(scenario==26 && frame==4 && !hold_event) {
        hold_event=true; hold_status=true; return MUSE_VOICE_CALL_EV_HOLD;
    }
    if(scenario==26 && frame==6 && !resume_event) {
        resume_event=true; hold_status=false; return MUSE_VOICE_CALL_EV_RESUME;
    }
    if(scenario==27 && frame==4 && !ended_event) {
        ended_event=true; snprintf(p,n,"CALL ENDED"); return MUSE_VOICE_CALL_EV_ENDED;
    }
    if (scenario>=12 && scenario<=14 && frame==4 && !ended_event) {
        ended_event=true; snprintf(p,n,"CALL ENDED"); return MUSE_VOICE_CALL_EV_ENDED;
    }
    if (!ended_event && ((need_end && scenario!=7) || (frame>=(scenario==1?20:16) && scenario!=7) || (scenario==3 && frame==2))) {
        ended_event=true; snprintf(p,n,"CALL ENDED"); return MUSE_VOICE_CALL_EV_ENDED;
    }
    return MUSE_VOICE_CALL_EV_NONE;
}
#endif

esp_err_t muse_audio_read(int16_t *p,size_t n) {
    ++frame; clock_us+=scenario==41?0:scenario==22?1000:20000; assert(frame<1200);
    if(scenario==41 && frame==2) push(MUSE_PTT_UP);
    if(scenario>=22 && scenario<=28) {
        if(frame==2) push(MUSE_PTT_UP);
        if(scenario==23) fail_read=frame>=3;
        if(scenario==24 && frame==4) push(MUSE_PTT_END_CALL);
        if(scenario==25 && frame==4) {
            input_context=true;
            muse_voice_shutdown(0);
            assert(!ends);   /* only the signal until the voice loop regains control */
            input_context=false;
        }
        if(scenario==28 && frame==4) push(MUSE_PTT_DOWN);
        if(scenario==28 && frame==6) push(MUSE_PTT_UP);
    }
    if(scenario>=30 && scenario<=32 && frame==2) {
        overflow_input(scenario==30?MUSE_PTT_UP:scenario==31?MUSE_PTT_END_CALL:MUSE_PTT_DOWN);
    }
    if (terminate_task && frame>20) longjmp(finish,1);
    if (scenario==1) {
        if(frame==3 || frame==10) push(MUSE_PTT_UP);
        if(frame==8) push(MUSE_PTT_DOWN);
        if(frame==9) notes_mode=true; /* changing the setting cannot end/change the call */
        if(frame==11) push(MUSE_PTT_DOWN);
        if(frame==13) fail_read=true;
        if(frame==14) { fail_read=false; fail_accept=true; }
        if(frame==15) { fail_accept=false; push(MUSE_PTT_UP); }
        if(frame==18) reply_done=true;
        if(frame==19) push(MUSE_PTT_END_CALL);
    } else if(scenario==2 && frame==1) {
        push(MUSE_PTT_UP); push(MUSE_PTT_DOWN); push(MUSE_PTT_UP);
    } else if(scenario==4 && frame==2) atomic_store(&s_shutdown,true);
    else if(scenario==5 && frame==2) push(MUSE_PTT_DOWN);
    else if(scenario==6 && frame==2) push(MUSE_PTT_UP);
    else if(scenario==7 && frame==2) { push(MUSE_PTT_END_CALL); clock_us+=20000000; }
    else if(scenario>=12 && scenario<=14 && frame==3) push(MUSE_PTT_UP);
    else if(scenario==18 && frame==4) push(MUSE_PTT_UP);
    if(scenario==19) fail_read=frame==3;
    for(size_t i=0;i<n;i++) p[i]=(int16_t)(100+frame);
    return fail_read?ESP_FAIL:ESP_OK;
}
esp_err_t muse_audio_write(const int16_t *p,size_t n) {
    assert(n && (!muse_voice_ptt_call() || !held || !p[0]));
    if(scenario!=22) clock_us+=20000;
    if(scenario==13 && ended_event && tail_reads==1 && p[0]) push(MUSE_PTT_DOWN);
    ++writes; if(frame<128) speaker_at[frame]=p[0];
    return ESP_OK;
}

int main(int argc,char **argv) {
    assert(argc==2); scenario=atoi(argv[1]); s_queue=input_queue=&queue; ready=true;
    s_pre=calloc(PRE_CHUNKS,sizeof(*s_pre));
    if (scenario>=20 && scenario<=21) {
        notes_mode=scenario==21; terminate_task=true; push(MUSE_PTT_DOWN); push(MUSE_PTT_UP);
        int outcome=setjmp(finish);
        if (!outcome) voice_task(NULL);
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE && !notes_mode;
        assert(ptt ? (outcome==1 && starts==1 && notes==0 && allocations==0) : (outcome==2 && notes==1));
        return 0;
    }
#if CONFIG_MUSE_REALTIME_VOICE
    if(scenario==41) {
        run_call(true,0);
        assert(tail_reads==150 && ended_event && !ends && !held && !muse_voice_in_call());
        assert(clock_us>=3000000 && clock_us<5000000);   /* 3 s consumed before ENDED, not a 2 s tail */
        return 0;
    }
    if (scenario>=35 && scenario<=36) {
        /* The input task marks a push-to-talk press pending (blue then ends
         * the call it starts). A waking tap clears the mark, unless another
         * press is queued behind it. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        post(MUSE_PTT_DOWN,true); push(MUSE_PTT_UP);
        if(scenario==36) post(MUSE_PTT_DOWN,false);
        assert((atomic_load(&s_call_pending)!=0)==ptt);
        terminate_task=true;
        if(!setjmp(finish)) voice_task(NULL);
        if(scenario==35) assert(!(atomic_load(&s_call_pending)!=0) && !starts && pending_at_ready==-1);
        else assert(!(atomic_load(&s_call_pending)!=0) && starts==(ptt?1:0) && pending_at_ready==(ptt?1:-1));
        return 0;
    }
    if (scenario==38) {
        /* L4: the voice task goes to rest; once it has found the queue empty,
         * the input task posts a talk press (mark set, then sent). The press
         * keeps its mark, so blue hangs up its call rather than muting. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        post(MUSE_PTT_DOWN,false);            /* an older press, dropped by the rest */
        inject_armed=true;
        set_resting(true);
        assert(injected_pending==2 && queue.n==1 && queue.items[0].type==MUSE_PTT_DOWN);
        assert((atomic_load(&s_call_pending)!=0)==ptt);
        assert(!ptt || atomic_load(&s_call_pending)==queue.items[0].mark);
        return 0;
    }
    if (scenario==39) {
        /* The same race at a waking tap: a press posted once the voice task
         * has seen the tap's release keeps its mark, and starts its call. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        post(MUSE_PTT_DOWN,true); push(MUSE_PTT_UP);
        inject_armed=true;   /* the first empty queue after the release */
        terminate_task=true;
        if(!setjmp(finish)) voice_task(NULL);
        assert(injected_pending==2);
        assert(starts==(ptt?1:0) && pending_at_ready==(ptt?1:-1));
        return 0;
    }
    if (scenario==40) {
        /* A press taken up inside a call (a new hold) doesn't leave a mark
         * behind for after the call. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        post(MUSE_PTT_DOWN,false); push(MUSE_PTT_UP);
        post(MUSE_PTT_DOWN,false);   /* marked: posted before the call shows */
        terminate_task=true;
        if(!setjmp(finish)) voice_task(NULL);
        assert(!atomic_load(&s_call_pending) && starts==(ptt?1:0));
        return 0;
    }
    if (scenario==37) {
        /* Going to rest drops presses from before sleeping, and their mark,
         * but not a press that just woke Muse. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        post(MUSE_PTT_DOWN,false); assert((atomic_load(&s_call_pending)!=0)==ptt);
        set_resting(true); assert(!queue.n && !(atomic_load(&s_call_pending)!=0));
        set_resting(false);
        post(MUSE_PTT_DOWN,true); set_resting(true);
        assert(queue.n==1 && queue.items[0].wake && (atomic_load(&s_call_pending)!=0)==ptt);
        return 0;
    }
#endif
    if (scenario==8) {
        push(MUSE_PTT_UP); push(MUSE_PTT_DOWN);
        assert(!ptt_held_on_waking()); assert(queue.n==1 && queue.items[0].type==MUSE_PTT_DOWN);
        assert(!starts && !delivered); return 0;
    }
    if (scenario==9) {
        assert(ptt_held_on_waking()); assert(clock_us>=300000 && !delivered);
        return 0;
    }
#if CONFIG_MUSE_REALTIME_VOICE
    if(scenario==0) {
        push(MUSE_PTT_DOWN); push(MUSE_PTT_UP);
        bool delivered_note;
        assert(hatch_reply(&delivered_note));
        assert(queue.n==1 && queue.items[0].type==MUSE_PTT_UP);
        run_call(true,0);
        assert(starts==1 && delivered>0 && delivered<=6 && !held && !notes);
        return 0;
    }
    if(scenario==10) {
        atomic_store(&s_ptt_call,true);
        for(int v=MUSE_VOICE_VIEW_CONNECTING;v<=MUSE_VOICE_VIEW_ENDING;v++) {
            call_view(v); assert(muse_voice_call_label() && muse_voice_call_hint());
            assert((mode==MUSE_MODE_LISTENING)==(v==MUSE_VOICE_VIEW_LISTENING));
            assert((mode==MUSE_MODE_SPEAKING)==(v==MUSE_VOICE_VIEW_SPEAKING));
        }
        atomic_store(&s_call_view,MUSE_VOICE_VIEW_NONE);
        assert(!muse_voice_call_label() && !muse_voice_call_hint()); return 0;
    }
    if(scenario==11) {
        call_view(MUSE_VOICE_VIEW_CONNECTING);
        input_context=true;
        muse_voice_shutdown(100);
        assert(clock_us==100000 && atomic_load(&s_shutdown) && !ends && !talk_edges);
        input_context=false;
        call_ui_t c={.ptt=true,.live=true,.release_until=clock_us+120000,.release_frames=TAIL_FRAMES};
        held=true;
        call_input(&c);
        assert(ends==1 && talk_edges==1 && !held && !c.release_until && c.hung_up);
        call_input(&c);
        assert(ends==1 && talk_edges==1); /* repeated polls never repeat end */
        muse_voice_cancel_shutdown(); assert(!atomic_load(&s_shutdown)); return 0;
    }
    if(scenario>=30 && scenario<=33) {
        run_call(true,0);
        assert(ends==1 && !muse_voice_in_call() && !held && muse_input_call_epoch()==1);
        assert(delivered==(scenario==33?0:1));
        /* Nothing queued before overflow can restart capture, including DOWN. */
        muse_input_event_t stale={.type=MUSE_PTT_DOWN,.epoch=0};
        xQueueSend(&queue,&stale,0);
        assert(!got_event(MUSE_PTT_DOWN) && !queue.n);
        scenario=6; frame=0; clock_us=0; need_end=ready_event=ended_event=false;
        int before=delivered;
        post(MUSE_PTT_DOWN,false);
        assert(got_event(MUSE_PTT_DOWN) && s_press_input_epoch==1);
        run_call(true,0);
        assert(starts==2 && delivered>before && !muse_voice_in_call());
        return 0;
    }
    if(scenario==34) {
        overflow_input(MUSE_PTT_DOWN);
        run_call(true,0);
        assert(!starts && !delivered && !muse_voice_in_call() && mode==MUSE_MODE_IDLE);
        assert(!got_event(MUSE_PTT_DOWN));
        return 0;
    }
    if(scenario>=22 && scenario<=28) {
        run_call(true,0);
        assert(mic_at[2] && !level_at[2] && views[2]!=MUSE_VOICE_VIEW_LISTENING);
        assert(talk_edges>=1 && !talk_values[0]);
        if(scenario==22) {
            assert(delivered==7 && talk_times[0]==7000); /* six tail blocks, even at zero playback latency */
        } else if(scenario==23) {
            assert(delivered==2 && talk_times[0]==180000); /* failed reads cannot extend the deadline */
        } else if(scenario<=27) {
            assert(delivered==3 && talk_times[0]==140000 && !mic_at[4]);
            assert(scenario==24 || scenario==25 ? ends==1 : ends==0);
        } else {
            assert(talk_edges==3 && !talk_values[0] && talk_values[1] && !talk_values[2]);
            assert(talk_times[0]==talk_times[1] && !mic_at[4] && mic_at[5]);
            assert(!mic_at[9] && talk_times[2]==340000);
        }
        for(int i=0;i<delivered;i++) {
            if(scenario!=28) assert(accepted_at[i]<=talk_times[0]);
        }
        assert(!held && !muse_voice_in_call());
        return 0;
    }
    if(scenario>=17 && scenario<=19) {
        s_pre_fill=PRE_CHUNKS;
        for(int i=0;i<PRE_CHUNKS;i++) for(int j=0;j<MUSE_AUDIO_CHUNK;j++) s_pre[i].pcm[j]=-30000;
        bool wake_held=ptt_held_on_waking();
        if(scenario==18) {
            assert(!wake_held && !s_pre_fill && !starts && !delivered);
            return 0;
        }
        assert(wake_held && s_pre_fill>0 && s_pre_fill<PRE_CHUNKS);
        size_t n=s_pre_fill;
        for(size_t i=0;i<n;i++) {
            pre_get(i,s_chunk);
            assert(s_chunk[0]!=-30000 && (scenario!=19 || s_chunk[0]!=103));
        }
        if(scenario==17) {
            push(MUSE_PTT_UP); /* release after the threshold, before start */
            run_call(true,n);
            assert(delivered==(int)n && !held && starts==1 && !notes && !allocations);
        } else assert(n==(size_t)(frame-1));
        return 0;
    }
    if(scenario==16) {
        muse_voice_shutdown(0);
        run_call(true, 0);
        assert(!starts && !muse_voice_in_call());
        return 0;
    }
    run_call(scenario!=5, 0);
    assert(starts==1 && !muse_voice_in_call() && !muse_voice_ptt_call());
    assert(level==0 && !allocations && !notes);
    if(scenario==1) {
        assert(mic_at[1] && mic_at[2] && !level_at[1]); /* startup buffered, not live */
        assert(views[2]==MUSE_VOICE_VIEW_LISTENING && level_at[2]==0.102f);
        assert(mic_at[3] && views[3]==MUSE_VOICE_VIEW_THINKING && !level_at[3]);
        assert(mic_at[4] && mic_at[5] && !mic_at[6]); /* delayed last syllable, then deadline */
        assert(speaker_at[6]==25000 && views[6]==MUSE_VOICE_VIEW_SPEAKING && level_at[6]==25.0f);
        assert(!mic_at[8] && !speaker_at[8]); /* DOWN arrived during blocking read */
        assert(mic_at[9] && !speaker_at[9] && views[9]==MUSE_VOICE_VIEW_LISTENING && level_at[9]==0.109f);
        assert(mic_at[10] && !mic_at[11] && mic_at[12]);
        assert(!mic_at[13] && !mic_at[14] && !level_at[13] && !level_at[14]);
        assert(views[18]==MUSE_VOICE_VIEW_READY && ends==1);
        assert(talk_edges==5 && !talk_values[2] && talk_values[3] && talk_times[2]==talk_times[3]);
    } else if(scenario==2) {
        assert(!held && !mic_at[1] && delivered>0 && delivered<=6 && !ends); /* UP retained; only bounded new-press tail */
    } else if(scenario==3) {
        assert(!ends); /* remote error/end is terminal */
    } else if(scenario==4 || scenario==5 || scenario==7) {
        assert(ends==1);
    } else if(scenario==6) {
        assert(mic_at[1] && mic_at[2] && mic_at[3] && mic_at[4] && !mic_at[5] && delivered==4);
        assert(talk_edges==1 && !talk_values[0] && talk_times[0]==180000); /* startup UP tail */
    } else if(scenario>=12 && scenario<=14) {
        assert(tail_reads==(scenario==12?3:scenario==13?1:100));
        assert(!ends && mic_at[3] && !mic_at[4]);
    }
    if(scenario==15) {
        frame=0; ready_event=ended_event=false;
        scenario=6;
        run_call(true, 0);
        assert(starts==2 && !muse_voice_in_call() && !muse_voice_call_label());
    }
#endif
    free(s_pre);
    return 0;
}
