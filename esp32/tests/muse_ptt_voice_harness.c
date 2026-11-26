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
#include "muse_mic_ring.h"
#include "muse_turn_dot.h"
#include <setjmp.h>
#include <stdarg.h>
/* Count muse_voice's turn-ended notices: the header is included first, so
 * its declaration is untouched, then calls in muse_voice.c land here. */
#include "muse_wakeword_glue.h"
static int turn_ended_calls;
#define muse_wakeword_turn_ended() (void)(++turn_ended_calls)
#include "muse_voice.c"
#define TAG INPUT_TAG
#define s_queue input_queue
#include "muse_input.c"
#undef s_queue
#undef TAG

#if CONFIG_MUSE_WAKEWORD_ENABLE
/* The glue's hooks as no-ops: the wake word scenarios post synthetic
 * edges and seeds themselves. */
esp_err_t muse_wakeword_init(QueueHandle_t q) { (void)q; return ESP_OK; }
void muse_wakeword_on_audio(const int16_t *pcm, size_t frames) { (void)pcm; (void)frames; }
static int note_ptt_calls;
void muse_wakeword_note_ptt(bool down) { (void)down; note_ptt_calls++; }
void muse_wakeword_set_enabled(bool on) { (void)on; }
bool muse_wakeword_enabled(void) { return false; }
/* Scenarios 100 and 101 turn listening off (the disable generation moves). */
static bool turn_wanted=true;
static uint32_t wanted_disables;
bool muse_wakeword_turn_wanted(uint32_t disables) { return turn_wanted && disables==wanted_disables; }
bool muse_wakeword_listening(void) { return false; }
void muse_wakeword_set_playback(bool on) { (void)on; }
void muse_wakeword_set_ptt_turn(bool active) { (void)active; }
const char *muse_wakeword_runner_kind(void) { return "fake"; }
/* The recorder's backlog: scenario 95 hands over the chunks the voice task
 * "had already tapped" past the seed, with sample values that continue it. */
static int16_t backlog_pcm[3][MUSE_AUDIO_CHUNK];
static size_t backlog_n, backlog_takes;
static bool backlog_lost;   /* scenario 97: overload overwrote part of it */
bool muse_wakeword_take_backlog(const int16_t *chunks[], size_t max, size_t *count)
{
    backlog_takes++;
    size_t n = backlog_n < max ? backlog_n : max;
    for (size_t i = 0; i < n; i++) chunks[i] = backlog_pcm[i];
    backlog_n = 0;   /* once per turn */
    *count = n;
    return !backlog_lost;
}
void muse_wakeword_status(muse_wakeword_status_t *out) { *out = (muse_wakeword_status_t){ .runner = "fake" }; }
#endif

void muse_f2_arm(int restart_s) { (void)restart_s; }
void muse_f2_print_result(void) {}
struct test_queue { muse_input_event_t items[64]; int n; } queue;
static int scenario, frame, starts, ends, allocations, notes, writes, delivered, tail_reads;
static int64_t clock_us;
/* main's scenarios carried here are numbered 100 + theirs (141-149, 164-169, 185-215). */
static float ring_values[32];   /* the reply's progress ring, scripted (143-147, #334) */
static int ring_count, reply_step;
static int64_t ring_times[32];
static bool completion_press_injected;
static size_t note_frames;      /* audio a note gave Muse */
static int64_t note_begin_us = -1;
/* 160-163: the note's audio, in order, against what the mic gave. */
static int note_release_frame, note_read_fail_frame;
static size_t note_burst, note_max_burst;   /* uploaded between two mic reads */
static bool verify_pcm;
static int16_t *expected_pcm, *uploaded_pcm;
static size_t expected_frames, direct_blocks, pending_chunks;
#if MUSE_LOW_MEM
static muse_adpcm_t expected_encoder;
#endif
static int talk_edges;
static int up_at_frame;   /* >0: the talk release arrives at this audio frame (a hold past 250 ms) */
static int down_at_frame, down_up_after;   /* >0: a new press at this audio frame, released down_up_after frames on */
static int terminate_after = 20;   /* audio frames before a terminate_task run stops */
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
static bool fail_read, fail_accept, hold_status, reply_done, working, terminate_task;
static bool need_end, sleep_state, input_context;
static int mic_at[128], speaker_at[128];
static float level_at[128], level;
static muse_voice_view_t views[128];
static muse_mode_t mode;
static int mode_sets;
static int64_t first_mode_us = -1;
static char caption[400];
static jmp_buf finish;
static int injected_pending;   /* 1: an injected press is held back, delivered by deliver_injected() */
static muse_input_event_t injected;
static void deliver_injected(void) {
    if (injected_pending==1) { injected_pending=2; queue.items[queue.n++]=injected; }
}

static void push(muse_ptt_t type) { muse_input_event_t e = {.type=type, .epoch=muse_input_call_epoch()}; xQueueSend(&queue, &e, 0); }
/* The first read after a dark-panel press takes this long (resting codecs
 * opening), and the talk key is let go at this time (posted as the input task
 * would, with its own time, even in the middle of a read). */
static int64_t first_read_us, up_at_us;
static int fail_until_frame;   /* reads fail (the codecs not open yet) up to this frame */
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
void vTaskDelay(int ms) {
    clock_us += (int64_t)ms*1000;
    if (scenario==147 && !completion_press_injected && ring_count &&
        ring_values[ring_count-1]==1 && clock_us-ring_times[ring_count-1]>=50000) {
        completion_press_injected=true;
        push(MUSE_PTT_DOWN);
    }
}
static int rec_allocs;
void *heap_caps_malloc(size_t n, unsigned caps) {
    (void)caps; ++allocations;
    if(scenario==188 && n==MAX_FRAMES*sizeof(int16_t)) { assert(!s_rec); ++rec_allocs; }   /* the last one was freed */
    return malloc(n);
}
void *heap_caps_realloc(void *p, size_t n, unsigned caps) { (void)caps; return realloc(p,n); }
static int pokes;
static int64_t poked_us, idle_clock_at_idle = -1;
void muse_state_set_mode(muse_mode_t m) {
    mode=m; ++mode_sets;
    if (first_mode_us<0) first_mode_us=clock_us;
    if (m==MUSE_MODE_IDLE) idle_clock_at_idle = clock_us - poked_us;   /* what check_sleep() would see */
    if (frame<128) { views[frame]=muse_voice_call_view(); level_at[frame]=level; }
}
muse_mode_t muse_state_mode(float *s) { if(s) *s=0; return mode; }
void muse_state_set_level(float v) {
    level=v;
    if(frame<128) level_at[frame]=v;
}
static bool ring_script;
void muse_state_set_progress(float v) {
    if (ring_script) {
        assert(ring_count<32);
        ring_values[ring_count]=v;
        ring_times[ring_count++]=clock_us;
    }
}
void muse_state_set_caption(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(caption,sizeof(caption),fmt,ap); va_end(ap);
}
void muse_state_set_hint(const char *text) { snprintf(caption,sizeof(caption),"%s",text); }
void muse_state_poke(void) { ++pokes; poked_us=clock_us; }
void muse_state_nudge(void) {}
void muse_state_make_happy(void) {}
bool muse_state_asleep(void) { return sleep_state; }
static int wakes;
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }   /* muse_input_wake nudging the input task */
void muse_state_set_asleep(bool v) { if (sleep_state && !v) ++wakes; sleep_state=v; }
static bool own_turn;
void muse_state_set_own_turn(bool v) { own_turn=v; }
bool muse_state_own_turn(void) { return own_turn; }
bool muse_state_on_battery(void) { return false; }
void muse_state_wait_awake(uint32_t ms) { vTaskDelay(ms); }
bool muse_settings_voice_notes(void) { return notes_mode; }
bool muse_settings_haptics(void) { return false; }
static muse_board_t voice_board;   /* no motor */
const muse_board_t *muse_board = &voice_board;
int muse_settings_volume(void) { return 20; }
int muse_settings_mic_gain(void) { return 30; }
int muse_audio_mic_gain(void) { return muse_settings_mic_gain(); }
bool muse_wifi_connected(void) { return ready; }
void muse_wifi_apply(void) {}
void muse_wifi_power(muse_wifi_power_t p) { (void)p; }
bool muse_hatch_ready(void) { return ready; }
void muse_hatch_status(muse_hatch_status_t *s) { s->state=ready?MUSE_HATCH_REACHABLE:MUSE_HATCH_OFFLINE; }
void muse_hatch_set_resting(bool v) { (void)v; }
void muse_audio_power(bool v) { (void)v; }
void muse_audio_selftest(void) {}
void muse_audio_chirp(int v) { (void)v; }
void muse_audio_write_end(void) {}   /* the speech chain's tail (hatch_reply) */
void muse_audio_loopback_test(int v) { (void)v; }
size_t muse_hatch_mp3_selftest(int16_t **p) { *p=NULL; return 0; }
float muse_audio_dbfs(const int16_t *p,size_t n) { (void)n; return (float)p[0]/100-50; }
float muse_audio_level(const int16_t *p,size_t n) { assert(n); return (float)p[0]/1000; }

/* Scripted turn events (resend scenarios), arriving once the note has all
 * gone (muse_hatch_turn_end): each ERROR carries its failure. */
static struct { muse_hatch_ev_t ev; muse_chat_fail_t fail; const char *text; } ev_script[32];
static int ev_script_n, ev_script_at, ev_script_gate;
static muse_chat_fail_t last_fail;
static int turn_ends, turn_cancels;
static void script(muse_hatch_ev_t ev, muse_chat_fail_t fail, const char *text) {
    assert(ev_script_n<32);
    ev_script_gate=turn_ends;
    ev_script[ev_script_n].ev=ev; ev_script[ev_script_n].fail=fail; ev_script[ev_script_n++].text=text;
}
muse_chat_fail_t muse_chat_turn_failure(void) { return last_fail; }
/* Legacy path is a tripwire, not another implementation of the note engine.
 * A toggled note (164, 166) records on, to its end (turn_end: outcome 4); 185
 * stops where the note opens (outcome 3), and so does a voice-loop scenario
 * that sets stop_at_note. The wake word and resend unit scenarios (167-169,
 * 190-215) record or send their notes. */
static bool toggle_records(void) { return scenario==164 || scenario==166; }
static bool stop_at_note;
static bool unit_note(void) { return (scenario>=167 && scenario<=169) || (scenario>=190 && scenario<=215); }
void muse_hatch_turn_begin(void) {
    ++notes; note_begin_us=clock_us; last_fail=MUSE_CHAT_FAIL_NONE;
    if(scenario==185 || stop_at_note) longjmp(finish,3);
    if(toggle_records() || (scenario>=160 && scenario<=163) || unit_note()) return;
    if(scenario>=20) longjmp(finish,2);
    assert(!"PTT opened a note");
}
void muse_hatch_turn_audio(const int16_t *p,size_t n) {
    if(verify_pcm) {
        assert(note_frames+n<=MAX_FRAMES);
        memcpy(uploaded_pcm+note_frames,p,n*sizeof(*p));
    }
    note_frames+=n; note_burst+=n;
    if(note_burst>note_max_burst) note_max_burst=note_burst;
}
size_t muse_hatch_turn_audio_wait(const int16_t *p,size_t n,int ms) {
    (void)ms; muse_hatch_turn_audio(p,n); return n;
}
void muse_hatch_turn_end(void) { ++turn_ends; if(toggle_records()) longjmp(finish,4); }
void muse_hatch_turn_cancel(void) { ++turn_cancels; }
bool muse_hatch_incoming(void) { return false; }
/* One scripted way for hatch_reply() to end, arriving after a long turn (142). */
static muse_hatch_ev_t hatch_end = MUSE_HATCH_EV_NONE;
static const char *hatch_end_text = "";
muse_hatch_ev_t muse_hatch_turn_event(char *p,size_t n) {
    if(ev_script_at<ev_script_n && turn_ends>ev_script_gate) {
        snprintf(p,n,"%s",ev_script[ev_script_at].text);
        if(ev_script[ev_script_at].ev==MUSE_HATCH_EV_ERROR) last_fail=ev_script[ev_script_at].fail;
        return ev_script[ev_script_at++].ev;
    }
    muse_hatch_ev_t ev=hatch_end;
    if(ev==MUSE_HATCH_EV_NONE) return ev;
    hatch_end=MUSE_HATCH_EV_NONE;
    clock_us += 200*1000000LL;   /* well past any sleep timeout since the last poke */
    snprintf(p,n,"%s",hatch_end_text);
    return ev;
}
bool muse_hatch_turn_caption(size_t x,char *p,size_t n) { (void)x;(void)p;(void)n;return false; }
float muse_turn_progress(size_t played,int *message) {
    (void)played;
    if (!ring_script) { *message=-1; return 0; }
    *message=reply_step<=3 ? 0 : 1;
    static const float fractions[]={0,.2f,.1f,1.f,.1f,.4f,.4f};
    return fractions[reply_step];
}
size_t muse_hatch_turn_read(int16_t *p,size_t n,int ms) {
    (void)ms;
    if (!ring_script) return 0;
    if (reply_step++<5) { for(size_t i=0;i<n;i++) p[i]=100; return n; }
    if (scenario==146) push(MUSE_PTT_DOWN);
    else {
        hatch_end=scenario==145 ? MUSE_HATCH_EV_ERROR : MUSE_HATCH_EV_DONE;
        hatch_end_text=scenario==144 ? "REPLY INCOMPLETE" : scenario==145 ? "LOST CONNECTION TO MUSE" : "";
    }
    return 0;
}

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
bool muse_voice_call_working(void) { return working; }
void muse_voice_call_ptt_status(muse_voice_call_ptt_status_t *s) {
    *s=(muse_voice_call_ptt_status_t){.connecting=!ready_event,.live=ready_event,.talking=held,
        .capture=held&&!need_end&&!hold_status,.hold=hold_status,
        .sending=delivered>0 && frame<(scenario==1?18:14),.heard=scenario==1,
        .reply_done=reply_done,.working=working};
}
bool muse_voice_call_audio(const int16_t *p,size_t n) {
    assert(n==MUSE_AUDIO_CHUNK);
    if (scenario<41 || scenario>43) assert(p[0] == (scenario==17 ? 101+delivered : 100+frame));   /* 41-43: the pre-roll's earlier frames */
    if (muse_voice_ptt_call()) assert(held&&!need_end&&!hold_status);
    if(fail_accept) return false;
    accepted_at[delivered++]=clock_us;
    if(frame<128) ++mic_at[frame];
    return true;
}
size_t muse_voice_call_read(int16_t *p,size_t n,bool *flushed) {
    *flushed=false;
    assert(!muse_voice_ptt_call() || !held);
    if(scenario==141 && ready_event && !held && tail_reads<150) {
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
size_t muse_voice_call_read_paced(int16_t *p,size_t n,bool *flushed) {
    return muse_voice_call_read(p,n,flushed);
}
muse_voice_call_ev_t muse_voice_call_event(char *p,size_t n) {
    if (!ready_event && frame>=2) { ready_event=true; return MUSE_VOICE_CALL_EV_READY; }
    if(scenario==141) {
        if(tail_reads==150 && !ended_event) {
            ended_event=true; snprintf(p,n,"MUSE ENDED THE CALL"); return MUSE_VOICE_CALL_EV_ENDED;
        }
        return MUSE_VOICE_CALL_EV_NONE;
    }
    if (scenario==1 && frame==11 && !hold_event) { hold_event=true; hold_status=true; return MUSE_VOICE_CALL_EV_HOLD; }
    if (scenario==1 && frame==12 && !resume_event) { resume_event=true; hold_status=false; return MUSE_VOICE_CALL_EV_RESUME; }
    if(scenario==49 && frame==4 && !resume_event) {
        resume_event=true; snprintf(p,n,"Hi!"); return MUSE_VOICE_CALL_EV_REPLY;
    }
    if(scenario==49 && frame==6 && !hold_event) {
        hold_event=true; snprintf(p,n,"Next"); return MUSE_VOICE_CALL_EV_HEARD;
    }
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
    if (!ended_event && ((need_end && scenario!=7) || (frame>=(scenario==1?22:16) && scenario!=7) || (scenario==3 && frame==2))) {
        ended_event=true; snprintf(p,n,"CALL ENDED"); return MUSE_VOICE_CALL_EV_ENDED;
    }
    return MUSE_VOICE_CALL_EV_NONE;
}
#endif

#if CONFIG_MUSE_GADGET_BLE_AUDIO
static void reply_tick(void);
#else
static void reply_tick(void) {}
#endif
static int synthetic_up_frame;   /* 192: the hands-free release after a barge-in's settle */
esp_err_t muse_audio_read(int16_t *p,size_t n) {
    int64_t before=clock_us;
    reply_tick();   /* the reply's 20 ms on the speaker, unless held */
    ++frame; clock_us+=scenario==141?0:scenario==22?1000:20000; assert(frame<1200);
    if (frame==1 && first_read_us) clock_us=before+first_read_us;
    if(scenario==141 && frame==2) push(MUSE_PTT_UP);
#if CONFIG_MUSE_WAKEWORD_ENABLE
    if(scenario==188 && (frame==2 || frame==5 || frame==8)) {   /* a hands-free turn whose backlog overload lost */
        int16_t seed[MUSE_AUDIO_CHUNK]={0};
        muse_voice_seed_preroll(seed,MUSE_AUDIO_CHUNK);
        backlog_n=3; backlog_lost=true;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
    }
    if(scenario==187 && frame==5) {
        muse_input_event_t u={.type=MUSE_PTT_UP,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&u,0);
    }
    if(unit_note() && scenario<210) {
        if(frame==note_release_frame) push(scenario==169 && NOTE_TOGGLE ? MUSE_PTT_DOWN : MUSE_PTT_UP);
        if(scenario==196 && frame==3) push(MUSE_PTT_DOWN);   /* a finger lands on the hands-free note */
        if(scenario==200 && frame==3) {   /* listening turned off, and straight back on */
            wanted_disables++;
        }
        if(scenario==203 && frame==2) push(MUSE_PTT_DOWN);   /* a finger takes the hands-free note over */
        if(scenario==203 && frame==4) turn_wanted=false;     /* then listening is turned off */
        if(scenario==203 && frame==8) push(MUSE_PTT_UP);     /* and the finger lets go */
        if(scenario==198 && frame==3) {   /* a press the queue's overflow made stale */
            muse_input_event_t d={.type=MUSE_PTT_DOWN,.epoch=muse_input_call_epoch()-1,.at_us=clock_us};
            xQueueSend(&queue,&d,0);
        }
        if((scenario==167 && frame==40) || (scenario==195 && frame==4) || (scenario==196 && frame==4) ||
           (scenario==198 && frame==10) || (scenario==192 && frame==synthetic_up_frame)) {   /* the wake word's endpointing */
            muse_input_event_t u={.type=MUSE_PTT_UP,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
            xQueueSend(&queue,&u,0);
        }
        for(size_t i=0;i<n;i++) p[i]=scenario==195 ? (int16_t)(1000+(4+frame)*MUSE_AUDIO_CHUNK%20000+i) :
            (int16_t)((int)((i*113+frame*97)%50000)-25000);
        if (terminate_task && frame>terminate_after) longjmp(finish,1);
        return ESP_OK;
    }
#endif
    if(scenario>=164 && scenario<=166 && frame==2) post(MUSE_PTT_UP,false);   /* the press that starts it is let go */
    if(scenario==164 && frame==40) post(MUSE_PTT_DOWN,false);                /* the press that stops it */
    if(scenario==165 && frame==10) post(MUSE_PTT_DOWN,false);                /* too soon: 180 ms captured */
    if(scenario==185 || scenario==186) {
        /* A waking tap, then a press that starts a note and is let go. */
        if(frame==5 || frame==12) post(MUSE_PTT_UP,false);
        if(frame==10) post(MUSE_PTT_DOWN,false);
        if(scenario==186 && frame==20) post(MUSE_PTT_DOWN,false);   /* stops it at 200 ms */
    }
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
    if((scenario==164 || scenario==185 || scenario==186) && frame==18)
        assert(atomic_load(&s_note_toggled));   /* the buttons treat talk as held */
#endif
    if(scenario>=160 && scenario<=163) {
        note_burst=0;
        if(frame==note_release_frame) push(MUSE_PTT_UP);
        for(size_t i=0;i<n;i++) p[i]=(int16_t)((int)((i*113+frame*97)%50000)-25000);
        if(verify_pcm && frame!=note_read_fail_frame) {
            assert(expected_frames+n<=MAX_FRAMES);
#if MUSE_LOW_MEM
            if(pending_chunks) {
                /* Queued behind the pre-roll: through the ring's ADPCM, in order. */
                uint8_t codes[MUSE_AUDIO_CHUNK/2];
                muse_adpcm_t decoder=expected_encoder;
                muse_adpcm_encode_block(&expected_encoder,p,n,codes);
                muse_adpcm_decode_block(&decoder,codes,n,expected_pcm+expected_frames);
                pending_chunks=pending_chunks+1>PRE_BURST_CHUNKS ? pending_chunks+1-PRE_BURST_CHUNKS : 0;
            } else
#endif
            {
                memcpy(expected_pcm+expected_frames,p,n*sizeof(*p));
                direct_blocks++;
            }
            expected_frames+=n;
        }
        return frame==note_read_fail_frame?ESP_FAIL:ESP_OK;
    }
    if (up_at_us && before<up_at_us && up_at_us<=clock_us) {
        int64_t now=clock_us; clock_us=up_at_us; post(MUSE_PTT_UP,false); clock_us=now; up_at_us=0;
    }
    if (up_at_frame && frame==up_at_frame) { up_at_frame=0; post(MUSE_PTT_UP,false); }
    if (down_at_frame && frame==down_at_frame) { down_at_frame=0; post(MUSE_PTT_DOWN,false); up_at_frame=frame+down_up_after; }
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
    if (terminate_task && frame>terminate_after) longjmp(finish,1);
    if (scenario==1) {
        if(frame==3 || frame==10) push(MUSE_PTT_UP);
        if(frame==8) push(MUSE_PTT_DOWN);
        if(frame==9) notes_mode=true; /* changing the setting cannot end/change the call */
        if(frame==11) push(MUSE_PTT_DOWN);
        if(frame==13) fail_read=true;
        if(frame==14) { fail_read=false; fail_accept=true; }
        if(frame==15) { fail_accept=false; push(MUSE_PTT_UP); }
        if(frame==18) { reply_done=true; working=true; }   /* the reply ends; Muse's work goes on */
        if(frame==19) working=false;
        if(frame==21) push(MUSE_PTT_END_CALL);
    } else if(scenario==2 && frame==1) {
        push(MUSE_PTT_UP); push(MUSE_PTT_DOWN); push(MUSE_PTT_UP);
    } else if(scenario==4 && frame==2) atomic_store(&s_shutdown,true);
    else if(scenario==5 && frame==2) push(MUSE_PTT_DOWN);
    else if(scenario==149) {   /* hands-free: Muse works on a turn, then is done */
        if(frame==4) working=true;
        if(frame==6) working=false;
        if(frame==8) push(MUSE_PTT_END_CALL);
    }
    else if(scenario==6 && frame==2) push(MUSE_PTT_UP);
    else if(scenario==7 && frame==2) { push(MUSE_PTT_END_CALL); clock_us+=20000000; }
    else if(scenario>=12 && scenario<=14 && frame==3) push(MUSE_PTT_UP);
    else if(scenario==18 && frame==4) push(MUSE_PTT_UP);
    if(scenario==19) fail_read=frame==3;
    for(size_t i=0;i<n;i++) p[i]=(int16_t)(100+frame);
    if (fail_until_frame && frame<=fail_until_frame) return ESP_FAIL;   /* closed: muse_audio says ESP_ERR_INVALID_STATE */
    return fail_read?ESP_FAIL:ESP_OK;
}
esp_err_t muse_audio_write(const int16_t *p,size_t n) {
    assert(n && (!muse_voice_ptt_call() || !held || !p[0]));
    if(scenario!=22) clock_us+=20000;
    if(scenario==13 && ended_event && tail_reads==1 && p[0]) push(MUSE_PTT_DOWN);
    ++writes; if(frame<128) speaker_at[frame]=p[0];
    return ESP_OK;
}

#if CONFIG_MUSE_GADGET_BLE_AUDIO
bool mg_ble_pair_auto_take(void) { return false; }   /* the bench auto-confirm: never armed here */
void mg_ble_link_user(const char *why) { (void)why; }   /* link modes (test_mg_linkpol runs them) */
void mg_ble_link_activity(void) {}
int mg_ble_reply_stats(bool json, char *out, size_t n) { return snprintf(out, n, "%s", json ? "{}" : ""); }
/*
 * musegadgets BLE (mg_voice.h), faked around the real route decision
 * (mg_route.c): which way a press goes, BLE to the phone or Muse's realtime
 * voice over Wi-Fi, under each routing policy.
 */
#include "mg_route.h"
static mg_policy_t route_policy = MG_POLICY_AUTO;
static bool client_ready, client_linked, queue_on;   /* client_linked: its BLE link up, not ready */
static int mg_begins, mg_ends, mg_frames, gestures_down, gestures_up;
static mg_route_t mg_route_seen = MG_ROUTE_NONE, mg_begun = MG_ROUTE_NONE;
mg_route_t mg_voice_route(bool wifi_ready) {
    mg_route_inputs_t in = { .wifi_ready = wifi_ready, .client_connected = client_ready || client_linked,
                             .client_ready = client_ready,
                             .ptt_enabled = client_ready, .queue_available = queue_on, .queue_enabled = queue_on };
    return mg_route_seen = mg_route_decide(route_policy, &in);
}
/* What the client sees, in order: D button_down, U button_up, E the capture's end ([stop_mic]). */
static char mg_order[32];
static int mg_audio_fail_at;   /* the client stops the capture at this audio frame (0: never) */
static void mg_order_add(char c) { size_t n = strlen(mg_order); if (n + 1 < sizeof(mg_order)) { mg_order[n] = c; mg_order[n + 1] = 0; } }
static int mg_frames_at_up = -1;   /* audio frames the client had when button_up went */
static int mg_turn_frames[4], mg_turn_post[4], mg_turn_end_read[4], mg_turn_first_read[4];   /* per turn (scenario 73) */
bool mg_voice_gesture(bool down) {
    if (down) ++gestures_down; else { ++gestures_up; mg_frames_at_up = mg_frames; }
    mg_order_add(down ? 'D' : 'U');
    return client_ready;
}
bool mg_voice_begin(mg_route_t route, bool from_client) {
    (void)from_client;
    if (mg_begins < 4) mg_turn_first_read[mg_begins] = frame;
    ++mg_begins; mg_begun = route; return true;
}
/* The mic ring is up while anything goes to the client, and down by the end. */
bool mg_voice_audio(const int16_t *pcm, size_t frames) {
    (void)pcm; assert(muse_mic_ring_on()); mg_frames += (int)frames;
    return !(mg_audio_fail_at && frame >= mg_audio_fail_at);
}
bool mg_ble_haptic_talk_cue(uint16_t ms) { (void)ms; return false; }   /* no haptics core: the board buzzes */
static long reply_pos;   /* the barge-in model below (a tentative definition) */
void mg_voice_end(bool keep) {
    (void)keep; assert(!muse_mic_ring_on());
    if (mg_ends < 4) {
        mg_turn_frames[mg_ends] = mg_frames;
        mg_turn_post[mg_ends] = mg_frames - mg_frames_at_up;
        mg_turn_end_read[mg_ends] = frame;
    }
    ++mg_ends; mg_order_add('E');
}
bool mg_voice_capture_requested(void) { return false; }
void mg_voice_claim(bool claim) { (void)claim; }
/* The bench guard (ptt.minrise) off: its protocol is test_mg_ble's mg_proto_harness test_minrise. */
bool mg_voice_hold(void) { return false; }
bool mg_voice_turn_dropped(void) { return false; }
/* The held live turn: not modeled here (its routing and protocol are test_mg_ble's). */
bool mg_voice_held_begin(void) { return false; }
bool mg_voice_turn_queued(void) { return false; }
/*
 * The barge-in echo rule: a client's reply,
 * modeled as a sample counter that the speaker advances 320 samples (20 ms)
 * a read while it plays, i.e. neither the touch-down's pause nor a turn
 * holds it. The pause and the turn's claim are timed on the fake clock.
 */
static bool reply_on, barge_on, turn_on;
static long reply_pos;                       /* the next reply sample the speaker plays */
static long reply_pos_at_pause = -1;
static int64_t barge_on_us = -1, barge_off_us = -1, claim_us = -1;
static int barge_ons, barge_offs, claims, reply_drops;
void mg_voice_barge_pause(bool on) {
    if (on) { ++barge_ons; barge_on_us = clock_us; reply_pos_at_pause = reply_pos; }
    else if (barge_on) { ++barge_offs; barge_off_us = clock_us; }
    barge_on = on;
}
void mg_voice_claim_turn(bool on) {
    if (on && !turn_on) {
        ++claims; claim_us = clock_us;
        if (reply_on) { reply_on = false; ++reply_drops; }   /* stop_streaming drop (a call's flush: the same here) */
    }
    turn_on = on;
    barge_on = false;   /* the turn decides (mg_proto_claim_turn) */
}
static void reply_tick(void) { if (reply_on && !barge_on && !turn_on) reply_pos += 320; }
static bool client_playing;   /* a client's stream_audio open on the speaker */
bool mg_voice_playing(void) { return client_playing; }
bool mg_voice_audible(void) { return client_playing; }
bool mg_voice_client_reports_state(void) { return true; }
bool mg_voice_client_linked(void) { return client_ready || client_linked; }
static bool set_up = true;   /* a device token stored */
bool muse_link_token_stored(void) { return set_up; }
void mg_voice_face(const char *what) { (void)what; }

/* One press (marked as a call press where push-to-talk calls are on) through
 * the voice task; returns where it went. */
static mg_route_t route_press(mg_policy_t policy, bool client, bool wifi, bool queue) {
    route_policy = policy; client_ready = client; ready = wifi; queue_on = queue;
    /* Held 400 ms: past the hold threshold, the release while it talks. */
    post(MUSE_PTT_DOWN, false); up_at_frame = frame + 20;
    terminate_after = frame + 60;
    terminate_task = true;
    if (!setjmp(finish)) voice_task(NULL);
    assert(!atomic_load(&s_call_pending));   /* no route leaves a call mark behind for the speaker button */
    return mg_route_seen;
}
#endif

int main(int argc,char **argv) {
    assert(argc==2); scenario=atoi(argv[1]); s_queue=input_queue=&queue; ready=true;
    s_pre=calloc(PRE_CAP_CHUNKS,sizeof(*s_pre));
#if CONFIG_MUSE_RESEND_NOTES
    if(scenario>=210 && scenario<=215) {
        /* Notes that didn't send (CONFIG_MUSE_RESEND_NOTES): this build keeps
         * HELD_MAX 3 in 1024 KB. */
        assert(HELD_MAX==3 && HELD_BYTES==1024*1024);
        clock_us=1000000;
#define SECS(s) ((size_t)(s)*MUSE_AUDIO_RATE)
#define REC(secs, live) do { s_rec=calloc(MAX_FRAMES,sizeof(*s_rec)); s_rec_n=SECS(secs); \
                             s_sent=(live)?s_rec_n:0; s_live=s_tried=(live); } while(0)
        if(scenario==210) {
            /* Each new note fits by dropping the oldest, by count and by bytes. */
            static const int secs[]={8,4,4,6,15,10,15};
            static const int kept[][3]={{8},{8,4},{8,4,4},{4,4,6},{4,6,15},{6,15,10},{10,15}};
            for(int i=0;i<7;i++) {
                REC(secs[i],false); s_tried=true;
                hold_rec(true);
                assert(!s_rec && !strcmp(caption,"Saved, will try again") && mode==MUSE_MODE_IDLE);
                int n=0; while(n<3 && kept[i][n]) n++;
                assert(s_held_count==n);
                for(int k=0;k<n;k++) assert(s_held[k].frames==SECS(kept[i][k]));
            }
            /* The oldest is due 5 s after its first try failed. */
            assert(s_next_send_us==clock_us+5000000);
        }
        if(scenario==211) {
            /* Tries go on quietly, past the old limit of 8, until the note lands. */
            REC(5,true); hold_rec(true);
            assert(s_held_count==1 && s_held[0].tries==1 && s_next_send_us==clock_us+5000000);
            snprintf(caption,sizeof(caption),"IDLE"); int sets=mode_sets;
            push(MUSE_PTT_DOWN);   /* a press goes first: the note stays, untried */
            assert(!send_held(false) && s_held_count==1 && s_held[0].tries==1 && queue.n==1);
            queue.n=0;
            static const int64_t waits[]={15,30,60,60,60,60,60,60,60,60};
            for(int i=0;i<10;i++) {
                script(MUSE_HATCH_EV_ERROR,MUSE_CHAT_FAIL_RETRY,"CAN'T REACH MUSE");
                assert(!send_held(i%2));
                assert(s_held_count==1 && s_held[0].tries==i+2);
                assert(s_next_send_us==clock_us+waits[i]*1000000);
            }
            assert(!strcmp(caption,"IDLE") && mode_sets==sets);   /* nothing on screen */
            script(MUSE_HATCH_EV_SENT,MUSE_CHAT_FAIL_NONE,"");
            script(MUSE_HATCH_EV_DONE,MUSE_CHAT_FAIL_NONE,"");
            assert(!send_held(false));
            assert(!s_held_count && !s_next_send_us && !strcmp(caption,"Saved note sent"));
            assert(mode==MUSE_MODE_THINKING);   /* the reply played, as any turn's */
        }
        if(scenario==212) {
            /* Refused, or up whole and unacknowledged: dropped, and the screen says so. */
            static const struct { muse_chat_fail_t fail; bool asleep; const char *caption; } ways[]={
                {MUSE_CHAT_FAIL_REFUSED,false,"MUSE DIDN'T TAKE IT"},
                {MUSE_CHAT_FAIL_UNKNOWN,true,MAY_HAVE_IT},
            };
            for(int i=0;i<2;i++) {
                REC(5,true); hold_rec(true);
                script(MUSE_HATCH_EV_ERROR,ways[i].fail,"MUSE DIDN'T TAKE IT");
                assert(!send_held(ways[i].asleep));
                assert(!s_held_count && !strcmp(caption,ways[i].caption));
            }
        }
        if(scenario==213) {
            /* All of it went up and no word came: Muse may have it. */
            REC(5,true); hold_rec(true);
            int64_t t0=clock_us;
            assert(!send_held(false));
            assert(clock_us-t0>=ACK_WAIT_US && !s_held_count && !strcmp(caption,MAY_HAVE_IT));
        }
        if(scenario==214) {
            /* After the release: kept only if Muse can't have it. */
            static const struct { bool live; muse_chat_fail_t fail; int held; const char *caption; } ways[]={
                {true,MUSE_CHAT_FAIL_UNKNOWN,0,MAY_HAVE_IT},
                {true,MUSE_CHAT_FAIL_REFUSED,0,"MUSE DIDN'T TAKE IT"},
                {true,MUSE_CHAT_FAIL_RETRY,1,"Saved, will try again"},
                {false,MUSE_CHAT_FAIL_NONE,2,"Saved, sends when online"},
            };
            for(int i=0;i<4;i++) {
                REC(5,ways[i].live);
                if(ways[i].live) script(MUSE_HATCH_EV_ERROR,ways[i].fail,"LOST CONNECTION TO MUSE");
                assert(!finish_note());
                assert(!s_rec && s_held_count==ways[i].held && !strcmp(caption,ways[i].caption));
            }
        }
        if(scenario==215) {
            /* A full queue doesn't refuse a press: the oldest makes room later. */
            for(int i=0;i<3;i++) { REC(2,false); hold_rec(false); }
            ready=false;
            assert(s_held_count==3 && can_record() && s_rec);
            assert(strcmp(caption,"Notes still waiting to send"));
            s_rec_n=SECS(3); hold_rec(false);
            assert(s_held_count==3 && s_held[2].frames==SECS(3));
        }
#undef REC
#undef SECS
        while(s_held_count) drop_oldest();
        free(s_pre); return 0;
    }
#endif
#if CONFIG_MUSE_WAKEWORD_ENABLE
    /* The wake word's turns. A hands-free press passes no gate (the wake
     * detection qualified it), so the unit scenarios go straight to record(). */
    if(scenario==190) {
        /* A wake word press that interrupts a reply stays synthetic, and a
         * real press after it does not inherit that. */
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch()};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        push(MUSE_PTT_DOWN);
        assert(got_event(MUSE_PTT_DOWN) && !s_press_synthetic);
        free(s_pre); return 0;
    }
    if(scenario==194) {
        /* The only audio from before a detection that can leave the device:
         * the seed, at most 1 s, in whole chunks. */
        static int16_t two_s[2*MUSE_AUDIO_RATE+123];
        for(size_t i=0;i<sizeof(two_s)/sizeof(two_s[0]);i++) two_s[i]=(int16_t)(i%1000+1);
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        muse_voice_seed_preroll(two_s,sizeof(two_s)/sizeof(two_s[0]));
        assert(SEED_FRAMES==MUSE_AUDIO_RATE && s_seed_frames==SEED_FRAMES);
        muse_voice_seed_preroll(two_s,1000);
        assert(s_seed_frames==1000/MUSE_AUDIO_CHUNK*MUSE_AUDIO_CHUNK);
        muse_voice_seed_preroll(NULL,0);
        assert(s_seed_frames==0);
        free(s_seed); s_seed=NULL; free(s_pre); return 0;
    }
    if(scenario==191) {
        /* A wake word press, through the voice task: its seed already holds
         * the pre-roll and the speech start, so nothing in the ring goes
         * with it (here, a ring a held press left). */
        notes_mode=true; clock_us=1000000;
        for(int i=0;i<PRE_CHUNKS;i++) {
            for(int j=0;j<MUSE_AUDIO_CHUNK;j++) s_chunk[j]=-30000;
            pre_put();
        }
        assert(s_pre_fill>0);
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        stop_at_note=true; terminate_task=true; terminate_after=60;
        int outcome=setjmp(finish);
        if(!outcome) voice_task(NULL);
        assert(outcome==3 && s_press_synthetic && !s_pre_fill);   /* the note opened with no ring */
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==192) {
        /* A hands-free turn is admitted at once: no hold to measure, so the
         * voice task reads nothing between the press and record() (where the
         * wake core hears it). After a barge-in only Muse's own tail is skipped. */
        notes_mode=true; clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        stop_at_note=true; terminate_task=true; terminate_after=60;
        int outcome=setjmp(finish);
        if(!outcome) voice_task(NULL);
        assert(outcome==3 && s_press_synthetic && frame==1);   /* the loop's one idle read before it */
        drop_rec();
        stop_at_note=false; terminate_task=false;
        int f0=frame;
        synthetic_up_frame=f0+SETTLE_CHUNKS+3;
        size_t held;
        char why[96];
        assert(record(true,true,&held,why,sizeof(why)));
        assert(note_frames==(size_t)(frame-f0-SETTLE_CHUNKS)*MUSE_AUDIO_CHUNK);   /* every read after the settle, none of it */
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==193) {
        /* A real press while a hands-free note starts takes it over: one
         * note, ended by the finger's release, nothing dropped. */
        clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        push(MUSE_PTT_DOWN);   /* the finger lands right away */
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        note_release_frame=60;
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && notes==1);
        assert(frame>=60 && frame<=60+7 && !turn_ended_calls);
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==198) {
        /* A press the input queue's overflow made stale lands on a
         * hands-free note: it takes nothing over and the wake word doesn't
         * hear it, so the wake word's release still ends the note. */
        clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && notes==1);
        assert(frame>10 && frame<=10+7 && s_press_synthetic && !note_ptt_calls);
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==188) {
        /* Overload rejects three hands-free turns in a row, through the
         * voice loop itself: each rejection frees the recording buffer
         * can_record() allocated (heap_caps_malloc asserts the last one is
         * gone before the next), and nothing is sent. */
        notes_mode=true; clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        terminate_task=true; terminate_after=14;
        int outcome=setjmp(finish);
        if(!outcome) voice_task(NULL);
        assert(outcome==1 && backlog_takes==3 && turn_ended_calls==3 && !notes && !note_frames && !s_rec);
#if HOLD_NOTES
        assert(rec_allocs==3);
#endif
        free(s_seed); s_seed=NULL; free(s_pre); return 0;
    }
    if(scenario==187) {
        /* A wake word press, released (listening disabled) 100 ms later,
         * through the voice task: admitted at once, so its synthetic release
         * ends a short note; never tap-to-call, and no hint. */
        notes_mode=true; clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        stop_at_note=true; terminate_task=true; terminate_after=60;
        int outcome=setjmp(finish);
        if(!outcome) voice_task(NULL);
        assert(outcome==3 && notes==1 && !starts);
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==197) {
        /* Overload overwrote part of the audio after the seed: the note
         * can't be continuous, so the turn ends before anything goes. */
        clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        int16_t seed[2*MUSE_AUDIO_CHUNK]={0};
        muse_voice_seed_preroll(seed,2*MUSE_AUDIO_CHUNK);
        backlog_n=3; backlog_lost=true;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        size_t held;
        char why[96]="x";
        assert(!record(true,false,&held,why,sizeof(why)) && !why[0]);
        assert(backlog_takes==1 && !notes && !note_frames && !s_rec_n && !s_seed_frames && turn_ended_calls==1 && !frame);
        free(s_seed); s_seed=NULL; drop_rec(); free(s_pre); return 0;
    }
    if(scenario==196) {
        /* Endpointing that raced a finger: the wake task read its input
         * before the press and posts a synthetic release after it. The
         * finger owns the note now, so only its release ends it. */
        clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        note_release_frame=60;
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && notes==1);
        assert(frame>=60 && frame<=60+7 && !s_press_synthetic);
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==200) {
        /* Listening turned off (then on again) while a hands-free note
         * records: it is dropped, never sent or kept, whatever the flag says
         * by the time anyone looks. */
        clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        int16_t seed[MUSE_AUDIO_CHUNK]={0};
        muse_voice_seed_preroll(seed,MUSE_AUDIO_CHUNK);
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
        size_t held;
        char why[96];
        int ended_before=turn_ended_calls;
        assert(!record(true,false,&held,why,sizeof(why)));
        assert(why[0]=='\0' && s_rec==NULL && turn_ends==0);
        assert(turn_cancels==(notes>0));   /* a streamed start is abandoned */
        assert(turn_ended_calls==ended_before+1);   /* the glue hears the turn is over */
        assert(frame<=4);   /* it stops at once, not at a release or the cap */
        free(s_seed); s_seed=NULL; free(s_pre); return 0;
    }
    if(scenario==201) {
        /* Listening turned off after admission: no turn opens, nothing goes. */
        clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        int16_t seed[3*MUSE_AUDIO_CHUNK]={0};
        muse_voice_seed_preroll(seed,3*MUSE_AUDIO_CHUNK);
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        turn_wanted=false;
        s_rec=NULL;   /* stream as taken, so every chunk sent is visible */
        size_t held;
        char why[96];
        assert(!record(true,false,&held,why,sizeof(why)) && why[0]=='\0');
        assert(notes==0 && note_frames==0 && turn_cancels==0 && frame==0);
        free(s_seed); s_seed=NULL; free(s_pre); return 0;
    }
    if(scenario==204) {
#if HOLD_NOTES
        /* A hands-free note recorded faster than it uploaded, then listening
         * turned off before finish_note(): nothing more goes, no commit, and
         * nothing is kept to send later. */
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        s_rec=calloc(10*MUSE_AUDIO_CHUNK,sizeof(*s_rec));
        s_rec_n=10*MUSE_AUDIO_CHUNK; s_sent=2*MUSE_AUDIO_CHUNK; s_live=true;
        turn_wanted=false;
        finish_note();
        assert(note_frames==0 && turn_ends==0 && turn_cancels==1 && s_rec==NULL && s_held_count==0);
#endif
        free(s_pre); return 0;
    }
    if(scenario==203) {
        /* A finger took the hands-free note over, then listening was turned
         * off: the note is the button's now, and goes when the finger lets go. */
        clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        int16_t seed[MUSE_AUDIO_CHUNK]={0};
        muse_voice_seed_preroll(seed,MUSE_AUDIO_CHUNK);
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)));
        assert(!s_press_synthetic && turn_cancels==0 && s_rec!=NULL && s_rec_n>=8*MUSE_AUDIO_CHUNK);
        free(s_seed); s_seed=NULL; drop_rec(); free(s_pre); return 0;
    }
    if(scenario==195) {
        /* The wake task trailed the voice task when it seeded the turn: the
         * seed (chunks 0-1) is followed by the backlog the voice task had
         * already tapped (chunks 2-4), then live reads (chunk 5 on). The
         * note is one unbroken sequence, every chunk once. Chunk k holds
         * 1000 + k*320 + i (mod 20000 per chunk start). */
        clock_us=1000000;
        s_seed=malloc(SEED_FRAMES*sizeof(*s_seed));
        int16_t seed[2*MUSE_AUDIO_CHUNK];
        for(int k=0;k<2;k++) for(int i=0;i<MUSE_AUDIO_CHUNK;i++) seed[k*MUSE_AUDIO_CHUNK+i]=(int16_t)(1000+k*MUSE_AUDIO_CHUNK%20000+i);
        muse_voice_seed_preroll(seed,2*MUSE_AUDIO_CHUNK);
        for(int k=0;k<3;k++) for(int i=0;i<MUSE_AUDIO_CHUNK;i++) backlog_pcm[k][i]=(int16_t)(1000+(2+k)*MUSE_AUDIO_CHUNK%20000+i);
        backlog_n=3;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && backlog_takes==1);
        size_t chunks=s_rec_n/MUSE_AUDIO_CHUNK;
        assert(chunks>=6);
        for(size_t k=0;k<chunks;k++)
            for(int i=0;i<MUSE_AUDIO_CHUNK;i++)
                assert(s_rec[k*MUSE_AUDIO_CHUNK+i]==(int16_t)(1000+k*MUSE_AUDIO_CHUNK%20000+i));
        free(s_seed); s_seed=NULL; drop_rec(); free(s_pre); return 0;
    }
    if(scenario==167) {
        /* Toggle builds: a wake word note still ends at its synthetic
         * release, not at the 15 s cap waiting for a second press. */
        assert(NOTE_TOGGLE);
        clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && notes==1);
        assert(frame>40 && frame<=40+7 && note_frames<MAX_FRAMES);
        assert(!turn_ended_calls);   /* the glue's own release ended it */
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==169) {
        /* Stray wake word edges during a button note change nothing: a
         * synthetic release doesn't end it or make it a tap, and a synthetic
         * press doesn't cancel it (the glue is told its capture is over). */
        clock_us=1000000;
        post(MUSE_PTT_DOWN,false);
        assert(got_event(MUSE_PTT_DOWN) && !s_press_synthetic);
        muse_input_event_t up={.type=MUSE_PTT_UP,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        muse_input_event_t dn={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&up,0);
        xQueueSend(&queue,&dn,0);
        note_release_frame=60;
#if NOTE_TOGGLE
        assert(toggle_gate(false)==TOGGLE_RECORD);   /* the toggled note's opening */
#else
        assert(hold_gate(false));                    /* held past the threshold */
#endif
        assert(turn_ended_calls==1 && !queue.n);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        xQueueSend(&queue,&up,0);
        size_t held;
        char why[96];
        assert(record(false,false,&held,why,sizeof(why)) && notes==1);
        assert(frame>=60 && frame<=60+7);   /* the real release (toggle: press) ended it */
        drop_rec(); free(s_pre); return 0;
    }
    if(scenario==168) {
        /* A hands-free note that runs to the recorder's cap: the glue is told,
         * so its capture closes and the next wake is heard. */
        clock_us=1000000;
        muse_input_event_t e={.type=MUSE_PTT_DOWN,.synthetic=true,.epoch=muse_input_call_epoch(),.at_us=clock_us};
        xQueueSend(&queue,&e,0);
        assert(got_event(MUSE_PTT_DOWN) && s_press_synthetic);
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        size_t held;
        char why[96];
        assert(record(true,false,&held,why,sizeof(why)) && notes==1);
        assert(turn_ended_calls==1);
        drop_rec(); free(s_pre); return 0;
    }
#endif
    if (scenario>=164 && scenario<=186) {
        /* CONFIG_MUSE_VOICE_NOTE_TOGGLE (#297), through the voice task: a press
         * starts a note, letting go doesn't stop it, the next press sends it.
         * Nothing goes to Muse before 500 ms of audio. 164: stopped at 800 ms;
         * 165: stopped at 180 ms, too soon; 166: never stopped, the 15 s cap;
         * 185: a waking tap only wakes, the next press records 500 ms before
         * the note opens though it's let go at once; 186: that press stopped
         * at 200 ms. */
        assert(NOTE_TOGGLE);
        notes_mode=true; clock_us=1000000;
        terminate_task=true; terminate_after=scenario==166 ? 1100 : 60;
        post(MUSE_PTT_DOWN,scenario>=185);
        int outcome=setjmp(finish);
        if(!outcome) voice_task(NULL);
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
        bool toggled=atomic_load(&s_note_toggled);
#else
        bool toggled=false;
#endif
        if(scenario==164 || scenario==166) {
            assert(outcome==4 && notes==1 && !starts);
            assert(note_begin_us-1000000>=500000);   /* nothing went before 500 ms of audio */
            if(scenario==164) {
                /* The release at 40 ms didn't stop it; the press at 800 ms did, with the tail. */
                assert(note_frames>=40*MUSE_AUDIO_CHUNK && note_frames<=(40+7)*MUSE_AUDIO_CHUNK && !queue.n);
            } else assert(note_frames==MAX_FRAMES);
        } else if(scenario==185) {
            assert(outcome==3 && notes==1 && !starts);
            assert(first_mode_us==1200000 && clock_us>=1700000);   /* the waking tap changed nothing */
        } else {
            assert(outcome==1 && !notes && !note_frames && !s_rec && !s_pre_fill && !level && !toggled);
            assert(!strcmp(caption,"Record a longer note") && mode==MUSE_MODE_IDLE && !muse_mic_ring_on());
        }
        free(s_pre); return 0;
    }
    if (scenario>=160 && scenario<=163) {
        /* A note's audio goes up in order (#311): the pre-roll, then the fresh
         * reads; on a low-memory board the pre-roll goes in bursts between
         * reads (no stall behind it), the reads queued behind it through the
         * ring's ADPCM, then direct PCM. 160: steady direct PCM after draining;
         * 161: let go at once, the tail with backlog left; 162: the 15 s cap;
         * 163: a read fails with backlog left (it's still sent, and the press
         * is a mic error, never a tap). *held: up to the release, never its tail. */
        clock_us=1000000;
        for(int i=0;i<PRE_CHUNKS;i++) {
            for(int j=0;j<MUSE_AUDIO_CHUNK;j++) s_chunk[j]=(int16_t)(i*1000+j);
            pre_put();
        }
        note_release_frame=scenario==160?100:scenario==161?1:scenario==162?100000:0;
        if(scenario==163) note_read_fail_frame=2;
        expected_pcm=calloc(MAX_FRAMES,sizeof(*expected_pcm));
        uploaded_pcm=calloc(MAX_FRAMES,sizeof(*uploaded_pcm));
        assert(expected_pcm && uploaded_pcm);
        for(size_t i=0;i<s_pre_fill;i++) {
            pre_get(i,expected_pcm+expected_frames);
            expected_frames+=MUSE_AUDIO_CHUNK;
        }
        size_t prefix=s_pre_fill*MUSE_AUDIO_CHUNK;
#if MUSE_LOW_MEM
        pending_chunks=s_pre_fill>PRE_BURST_CHUNKS ? s_pre_fill-PRE_BURST_CHUNKS : 0;
        expected_encoder=s_pre_enc;
#endif
        verify_pcm=true;
#if HOLD_NOTES
        s_rec=malloc(MAX_FRAMES*sizeof(*s_rec));
#endif
        size_t held;
        char why[96];
        assert(record(false,false,&held,why,sizeof(why)) && notes==1);
        if(note_frames!=expected_frames) fprintf(stderr,"%d: sent %zu, expected %zu\n",scenario,note_frames,expected_frames);
        assert(note_frames==expected_frames);
        assert(!memcmp(uploaded_pcm,expected_pcm,note_frames*sizeof(*uploaded_pcm)));
        size_t tail=TAIL_FRAMES/MUSE_AUDIO_CHUNK*MUSE_AUDIO_CHUNK;
        if(scenario==160) assert(held==100*MUSE_AUDIO_CHUNK && note_frames==prefix+held+tail && direct_blocks>0);
        if(scenario==161) assert(held==MUSE_AUDIO_CHUNK && note_frames==prefix+held+tail);
        if(scenario==162) assert(note_frames==MAX_FRAMES && held==MAX_FRAMES-prefix);
        if(scenario==163) assert(note_frames==prefix+MUSE_AUDIO_CHUNK && s_capture_failed);
        else assert(!s_capture_failed);
#if MUSE_LOW_MEM
        if(scenario!=163) assert(note_max_burst<=PRE_BURST_CHUNKS*MUSE_AUDIO_CHUNK);
#endif
        drop_rec(); free(expected_pcm); free(uploaded_pcm); free(s_pre);
        return 0;
    }
    if (scenario==142) {
        /* An active turn counts as activity for auto-sleep (#281): however a
         * long turn ends (the reply done, a capped REPLY INCOMPLETE, an
         * error), the idle clock is fresh when the mode becomes IDLE, so the
         * caption gets a full sleep timeout instead of blanking at once. */
        static const struct { muse_hatch_ev_t ev; const char *text; } ways[] = {
            {MUSE_HATCH_EV_DONE, ""},
            {MUSE_HATCH_EV_DONE, "REPLY INCOMPLETE"},
            {MUSE_HATCH_EV_ERROR, "NO REPLY FROM MUSE"},
        };
        for (size_t i=0; i<sizeof(ways)/sizeof(ways[0]); i++) {
            hatch_end=ways[i].ev; hatch_end_text=ways[i].text;
            idle_clock_at_idle=-1;
            bool done_delivered;
            assert(!hatch_reply(&done_delivered, "Sending voice note"));
            assert(done_delivered==(ways[i].ev==MUSE_HATCH_EV_DONE));
            if (!ways[i].text[0]) {
                /* A finished reply stays THINKING; voice_task then goes idle. */
                assert(mode==MUSE_MODE_THINKING && idle_clock_at_idle==-1);
                go_idle("");
            }
            assert(mode==MUSE_MODE_IDLE && idle_clock_at_idle==0);
            assert(!strcmp(caption, ways[i].text));
        }
        return 0;
    }
    if (scenario>=143 && scenario<=147) {
        /* The reply's progress ring (#334): forward only across messages,
         * full only when the whole turn completed, cleared by an incomplete
         * reply (144), an error (145) or a press (146); the full ring stays up
         * 300 ms, and a press still interrupts it (147). */
        ring_script=true;
        bool received;
        assert(hatch_reply(&received, "Sending voice note")==(scenario==146 || scenario==147));
        assert(ring_values[0]==0); /* Clear the previous recording's value. */
        assert(ring_values[1]==.2f && ring_values[2]==.2f); /* Revised estimate can't rewind. */
        assert(ring_values[3]==.99f && ring_values[4]==.99f); /* Next message keeps the ring near completion. */
        if (scenario==143) {
            assert(ring_values[ring_count-1]==1);
            for(int i=1;i<ring_count;i++) assert(ring_values[i]>=ring_values[i-1]);
            for(int i=0;i<ring_count-1;i++) assert(ring_values[i]<1);
            assert(clock_us-ring_times[ring_count-1]>=300000);
            assert(mode==MUSE_MODE_SPEAKING);
        } else if (scenario==147) {
            assert(completion_press_injected && ring_values[ring_count-1]==0);
            assert(clock_us-ring_times[ring_count-2]<300000);
        } else {
            assert(ring_values[ring_count-1]==0);
            for(int i=0;i<ring_count;i++) assert(ring_values[i]<1);
        }
        return 0;
    }
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (scenario==44 || scenario==45) {
        /* A client's start_mic while the codecs are still opening (muse_audio
         * retries a refused open once a second): the capture waits up to 1.5 s
         * of closed reads for them (44: open after 0.6 s, frames flow), then
         * ends (45: never open, nothing sent). */
        client_ready = true;
        fail_until_frame = scenario == 44 ? 30 : 100000;
        terminate_after = scenario == 44 ? 60 : 200;
        terminate_task = true;
        if (!setjmp(finish)) mg_turn(MG_ROUTE_BLE, true, false, false);
        if (scenario == 44) assert(mg_frames > 0);
        else assert(mg_frames == 0 && frame == MIC_OPEN_GRACE_READS);
        return 0;
    }
#endif
    if (scenario>=41 && scenario<=43) {
        /* A held talk key talks from a dark panel. The press wakes it and
         * counts HOLD_MS (250) from the press itself, however long the resting
         * codecs take to open on the first read: 41 held 400 ms (the first
         * read 200 ms) talks; 42 a 150 ms press only wakes; 43 let go at 350 ms
         * while the first read still takes 500 ms: held through the threshold,
         * it talks. One press, never a second. */
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        client_ready = true;
        route_policy = MG_POLICY_BLE_ONLY;
#endif
        clock_us = 1000000;
        int64_t press = clock_us;
        first_read_us = scenario == 43 ? 500000 : 200000;
        up_at_us = press + (scenario == 42 ? 150000 : scenario == 43 ? 350000 : 400000);
        post(MUSE_PTT_DOWN, true);   /* the press that woke the screen */
        terminate_task = true; terminate_after = 80;
        int outcome = setjmp(finish);
        if (!outcome) voice_task(NULL);
        bool talked = starts || notes || outcome == 2;
#if CONFIG_MUSE_GADGET_BLE_AUDIO
        talked = talked || mg_begins;
#endif
        if (talked != (scenario != 42)) fprintf(stderr, "%d: talked %d (starts %d notes %d outcome %d)\n", scenario, talked, starts, notes, outcome);
        assert(talked == (scenario != 42));
        return 0;
    }
    if (scenario>=20 && scenario<=21) {
        notes_mode=scenario==21; terminate_task=true; push(MUSE_PTT_DOWN);
        /* A note is held past the hold threshold; the realtime call starts at the press. */
        if (!(CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE && !notes_mode)) {
            up_at_frame = 20; terminate_after = 80;
        } else {
            push(MUSE_PTT_UP);
        }
        int outcome=setjmp(finish);
        if (!outcome) voice_task(NULL);
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE && !notes_mode;
        assert(ptt ? (outcome==1 && starts==1 && notes==0 && allocations==0) : (outcome==2 && notes==1));
        return 0;
    }
#if CONFIG_MUSE_REALTIME_VOICE
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
         * behind for after the call. Other boards' tap starts the call. */
        bool ptt=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        bool tap_call=CONFIG_MUSE_REALTIME_VOICE && !CONFIG_MUSE_BOARD_M5STACK_STOPWATCH;
        post(MUSE_PTT_DOWN,false); push(MUSE_PTT_UP);
        post(MUSE_PTT_DOWN,false);   /* marked: posted before the call shows */
        terminate_task=true;
        if(!setjmp(finish)) voice_task(NULL);
        assert(!atomic_load(&s_call_pending) && starts==((ptt||tap_call)?1:0));
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
#if CONFIG_MUSE_GADGET_BLE_AUDIO
    if (scenario==67 || scenario==68) {
        /* A turn the device ends while the key is still held: the 15 s cap
         * (67) or the client stopping the capture (68). button_up goes before
         * [stop_mic] (mgcommands: button up -> gesture, stop capture,
         * stop_mic), and the later release sends nothing more. */
        client_ready = true;
        route_policy = MG_POLICY_BLE_ONLY;
        if (scenario == 68) mg_audio_fail_at = 40;
        frame = 0; terminate_after = scenario == 67 ? 900 : 120;
        terminate_task = true;
        post(MUSE_PTT_DOWN, false);
        if (!setjmp(finish)) voice_task(NULL);
        if (strcmp(mg_order, "DUE") != 0) fprintf(stderr, "%d: order '%s'\n", scenario, mg_order);
        assert(!strcmp(mg_order, "DUE"));
        /* Device-ended: stopped at once, no post-roll: nothing after the up. */
        if (mg_frames != mg_frames_at_up) fprintf(stderr, "%d: %d frames after the up\n", scenario, mg_frames - mg_frames_at_up);
        assert(mg_frames == mg_frames_at_up);
        muse_voice_talk_released();   /* the finger lifts at last */
        assert(!strcmp(mg_order, "DUE") && gestures_up == 1);
        return 0;
    }
    if (scenario == 74 || scenario == 75) {
        /* The barge-in echo rule: a talk press
         * during a reply mutes it at touch-down, keeping its audio. 74: held
         * past the threshold: no reply sample plays from touch-down on, and
         * the turn is claimed at the threshold (the reply dropped there, as
         * before), never resumed. 75: a tap: the reply resumes at the tap
         * from the very sample it paused at, nothing claimed or lost. */
        client_ready = true; client_playing = true;
        route_policy = MG_POLICY_BLE_ONLY;
        reply_on = true;
        frame = 0; terminate_after = 60; terminate_task = true;
        for (int i = 0; i < 5; i++) { int16_t pcm[MUSE_AUDIO_CHUNK]; muse_audio_read(pcm, MUSE_AUDIO_CHUNK); }
        long before = reply_pos;
        assert(before == 5 * 320);   /* playing: 100 ms in */
        int64_t down_us = clock_us;
        post(MUSE_PTT_DOWN, false);
        /* At touch-down itself (the input task's post), before the voice task saw the press. */
        assert(barge_ons == 1 && barge_on && barge_on_us == down_us && reply_pos_at_pause == before);
        up_at_frame = scenario == 74 ? frame + 30 : frame + 5;   /* 600 ms (a hold) or 100 ms (a tap) */
        if (!setjmp(finish)) voice_task(NULL);
        if (scenario == 74) {
            if (claims != 1 || claim_us - down_us < HOLD_MS * 1000LL || claim_us - down_us > HOLD_MS * 1000LL + 40000)
                fprintf(stderr, "74: claims %d at +%lld us, resumes %d\n", claims, (long long)(claim_us - down_us), barge_offs);
            assert(claims == 1 && reply_drops == 1);
            assert(claim_us - down_us >= HOLD_MS * 1000LL && claim_us - down_us <= HOLD_MS * 1000LL + 40000);
            assert(barge_offs == 0);            /* never resumed: the turn took it */
            assert(reply_pos == before);        /* not one reply sample from touch-down to the claim (and none after) */
            assert(!strncmp(mg_order, "D", 1) && mg_begins == 1);
        } else {
            if (barge_offs != 1 || claims) fprintf(stderr, "75: resumes %d claims %d\n", barge_offs, claims);
            assert(barge_offs == 1 && claims == 0 && reply_drops == 0 && gestures_down == 0 && mg_begins == 0);
            /* Resumed at the tap (seen within a read of the release), from the same sample. */
            assert(barge_off_us - down_us >= 100000 && barge_off_us - down_us <= 140000);
            assert(reply_on && reply_pos > before);
            long played_after = reply_pos - before;
            long reads_after = (long)((clock_us - barge_off_us) / 20000);
            assert(played_after <= (reads_after + 1) * 320 && played_after >= (reads_after - 1) * 320);
        }
        return 0;
    }
    if (scenario == 72) {
        /* A release captures ~200 ms more (the post-roll) before [stop_mic],
         * the ring up all through it (mg_voice_audio asserts it; mg_voice_end that
         * it's down by the end), and the turn still ends with stop_mic. */
        client_ready = true;
        route_policy = MG_POLICY_BLE_ONLY;
        frame = 0; terminate_after = 120;
        terminate_task = true;
        post(MUSE_PTT_DOWN, false); up_at_frame = 30;
        if (!setjmp(finish)) voice_task(NULL);
        if (strcmp(mg_order, "DUE") != 0) fprintf(stderr, "72: order '%s'\n", mg_order);
        assert(!strcmp(mg_order, "DUE") && mg_ends == 1);
        int post_frames = mg_frames - mg_frames_at_up;
        if (post_frames < BLE_POSTROLL_FRAMES - MUSE_AUDIO_CHUNK || post_frames > BLE_POSTROLL_FRAMES + MUSE_AUDIO_CHUNK)
            fprintf(stderr, "72: %d frames after the release, want ~%d\n", post_frames, BLE_POSTROLL_FRAMES);
        assert(post_frames >= BLE_POSTROLL_FRAMES - MUSE_AUDIO_CHUNK && post_frames <= BLE_POSTROLL_FRAMES + MUSE_AUDIO_CHUNK);
        assert(!muse_mic_ring_on());
        printf("72: %d frames (%d ms) after the release, then stop_mic\n", post_frames, post_frames * 1000 / MUSE_AUDIO_RATE);
        return 0;
    }
    if (scenario == 73) {
        /* A new press 100 ms into the post-roll cuts it: turn 1 ends at
         * once (its frames, then stop_mic), turn 2 starts with no refused
         * press and the mic running on into its pre-roll: two complete
         * utterances, each with its own button_down and stop_mic. */
        client_ready = true;
        route_policy = MG_POLICY_BLE_ONLY;
        frame = 0; terminate_after = 160;
        terminate_task = true;
        post(MUSE_PTT_DOWN, false); up_at_frame = 30;
        down_at_frame = 35; down_up_after = 30;   /* 5 reads (100 ms) after the release */
        if (!setjmp(finish)) voice_task(NULL);
        if (strcmp(mg_order, "DUEDUE") != 0 || mg_begins != 2 || mg_ends != 2)
            fprintf(stderr, "73: order '%s' begins %d ends %d caption '%s'\n", mg_order, mg_begins, mg_ends, caption);
        assert(!strcmp(mg_order, "DUEDUE") && mg_begins == 2 && mg_ends == 2 && gestures_down == 2);
        int post1 = mg_turn_post[0];
        int turn2 = mg_turn_frames[1] - mg_turn_frames[0];
        /* Turn 1's post-roll: cut at about 100 ms (the press is seen within a read). */
        if (post1 < 4 * MUSE_AUDIO_CHUNK || post1 > 6 * MUSE_AUDIO_CHUNK)
            fprintf(stderr, "73: turn 1 post-roll %d frames\n", post1);
        assert(post1 >= 4 * MUSE_AUDIO_CHUNK && post1 <= 6 * MUSE_AUDIO_CHUNK);
        /* No gap in the mic: every read from turn 1's end to turn 2's start is
         * in turn 2's pre-roll (one read of slack for the turn's own handoff). */
        int gap_reads = mg_turn_first_read[1] - mg_turn_end_read[0];
        int pre2 = turn2 - (mg_turn_end_read[1] - mg_turn_first_read[1]) * MUSE_AUDIO_CHUNK;
        if (pre2 < (gap_reads - 1) * MUSE_AUDIO_CHUNK)
            fprintf(stderr, "73: %d reads between the turns, turn 2's pre-roll %d frames\n", gap_reads, pre2);
        assert(pre2 >= (gap_reads - 1) * MUSE_AUDIO_CHUNK);
        printf("73: turn 1 post-roll %d ms, cut; %d reads to turn 2, its pre-roll %d ms; DUEDUE\n",
               post1 * 1000 / MUSE_AUDIO_RATE, gap_reads, pre2 * 1000 / MUSE_AUDIO_RATE);
        return 0;
    }
    if (scenario>=60 && scenario<=66) {
        bool realtime_ptt = CONFIG_MUSE_REALTIME_VOICE && CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && !notes_mode;
        mg_route_t r = MG_ROUTE_NONE;
        switch (scenario) {
        case 60: r = route_press(MG_POLICY_AUTO, true, true, false); break;       /* both up: the phone */
        case 61: r = route_press(MG_POLICY_AUTO, false, true, false); break;      /* Wi-Fi only: realtime voice */
        case 62: r = route_press(MG_POLICY_WIFI_ONLY, true, true, false); break;  /* policy: Wi-Fi even with a phone */
        case 63: r = route_press(MG_POLICY_BLE_ONLY, true, true, false); break;   /* policy: the phone */
        case 64: r = route_press(MG_POLICY_BLE_ONLY, false, true, true); break;   /* no phone: saved for it */
        case 65: r = route_press(MG_POLICY_BLE_ONLY, false, true, false); break;  /* no phone, no queue: nowhere */
        case 66: {
            /* The hold contract: a tap sends no gesture at all; a hold
             * sends button_down once past 250 ms and button_up at the release;
             * blue's hang-up sends none. */
            client_ready = true;
            route_policy = MG_POLICY_BLE_ONLY;
            post(MUSE_PTT_DOWN, false); push(MUSE_PTT_UP);
            terminate_task = true;
            if (!setjmp(finish)) voice_task(NULL);
            if (!(gestures_down == 0 && gestures_up == 0 && !mg_begins && !strcmp(caption, "Hold to talk")))
                fprintf(stderr, "66: down %d up %d begins %d caption '%s'\n", gestures_down, gestures_up, mg_begins, caption);
            assert(gestures_down == 0 && gestures_up == 0 && !mg_begins && !strcmp(caption, "Hold to talk"));
            post(MUSE_PTT_END_CALL, false);
            assert(gestures_down == 0 && gestures_up == 0);
            frame = 0; terminate_after = 60;
            post(MUSE_PTT_DOWN, false); up_at_frame = 20;
            if (!setjmp(finish)) voice_task(NULL);
            assert(gestures_down == 1 && gestures_up == 1 && mg_begins == 1);
            return 0;
        }
        }
        bool phone = scenario == 60 || scenario == 63 || scenario == 64;
        bool nowhere = scenario == 65;
        assert(r == (scenario == 64 ? MG_ROUTE_QUEUE : nowhere ? MG_ROUTE_NONE : phone ? MG_ROUTE_BLE : MG_ROUTE_WIFI));
        if (phone) {
            /* To the phone (or its queue): the press's audio, and no realtime call. */
            assert(mg_begins == 1 && mg_ends == 1 && mg_begun == r && mg_frames > 0 && !starts && !notes);
        } else if (nowhere) {
            /* No link at all: open the app. */
            if (strcmp(caption, "Open Muse app")) fprintf(stderr, "%d: caption '%s'\n", scenario, caption);
            assert(!mg_begins && !starts && !notes && !strcmp(caption, "Open Muse app"));
        } else {
            /* Wi-Fi: Muse's realtime voice takes the press, as on a build without BLE. */
            assert(!mg_begins);
            assert(realtime_ptt ? (starts == 1 && !notes) : (!starts && notes == 1));
        }
        return 0;
    }
#endif
    if (scenario==8) {
        push(MUSE_PTT_UP); push(MUSE_PTT_DOWN);
        assert(!ptt_held_on_waking()); assert(queue.n==1 && queue.items[0].type==MUSE_PTT_DOWN);
        assert(!starts && !delivered); return 0;
    }
    if (scenario==9) {
        assert(ptt_held_on_waking()); assert(clock_us>=250000 && !delivered);   /* HOLD_MS (250 everywhere) */
        return 0;
    }
#if CONFIG_MUSE_REALTIME_VOICE
    if(scenario==0) {
        push(MUSE_PTT_DOWN); push(MUSE_PTT_UP);
        bool delivered_note;
        assert(hatch_reply(&delivered_note, "Sending voice note"));
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
            /* WORKING (#278): the thinking face, and no text label for a turn's state. */
            if(v==MUSE_VOICE_VIEW_WORKING) assert(mode==MUSE_MODE_THINKING && !strcmp(muse_voice_call_label(),""));
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
#if !MUSE_LOW_MEM
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
            /* The whole pre-roll first (in order: muse_voice_call_audio checks), then
             * at most the release's tail (HOLD_MS 250: the call starts 50 ms
             * sooner than at 300, so the tail can still be open). */
            assert(delivered>=(int)n && delivered<=(int)n+TAIL_FRAMES/MUSE_AUDIO_CHUNK);
            assert(!held && starts==1 && !notes && !allocations);
        } else assert(n==(size_t)(frame-1));
        return 0;
    }
#endif
    if(scenario==141) {
        /* The goodbye a remote end plays: 3 s of it before the delayed
         * ENDED event, not a 2 s tail. */
        run_call(true,0);
        assert(tail_reads==150 && ended_event && !ends && !held && !muse_voice_in_call());
        assert(clock_us>=3000000 && clock_us<5000000);
        return 0;
    }
    if(scenario==16) {
        muse_voice_shutdown(0);
        run_call(true, 0);
        assert(!starts && !muse_voice_in_call());
        return 0;
    }
    /* The scenarios the checks below cover: any other number (a typo, or a
     * case renumbered away) fails instead of passing on the generic checks. */
    if (!((scenario>=1 && scenario<=7) || (scenario>=12 && scenario<=15) || scenario==49 || scenario==149)) {
        fprintf(stderr, "unknown voice scenario %d\n", scenario);
        return 2;
    }
    run_call(scenario!=5 && scenario!=49 && scenario!=149, 0);
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
        assert(views[18]==MUSE_VOICE_VIEW_WORKING && views[19]==MUSE_VOICE_VIEW_READY && ends==1);
        assert(talk_edges==5 && !talk_values[2] && talk_values[3] && talk_times[2]==talk_times[3]);
    } else if(scenario==2) {
        assert(!held && !mic_at[1] && delivered>0 && delivered<=6 && !ends); /* UP retained; only bounded new-press tail */
    } else if(scenario==3) {
        assert(!ends); /* remote error/end is terminal */
    } else if(scenario==4 || scenario==5 || scenario==7) {
        assert(ends==1);
    } else if(scenario==49) {
        assert(views[4]==MUSE_VOICE_VIEW_SPEAKING && views[5]==MUSE_VOICE_VIEW_SPEAKING);
        assert(!speaker_at[4] && mic_at[4] && mic_at[5]);
        assert(views[6]==MUSE_VOICE_VIEW_LISTENING);
    } else if(scenario==149) {
        /* Hands-free (#278): WORKING while Muse is still on a turn, the mic open all along. */
        assert(views[3]==MUSE_VOICE_VIEW_LISTENING && views[4]==MUSE_VOICE_VIEW_WORKING);
        assert(views[5]==MUSE_VOICE_VIEW_WORKING && views[6]==MUSE_VOICE_VIEW_LISTENING && ends==1);
        assert(mic_at[4] && mic_at[5]);
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
