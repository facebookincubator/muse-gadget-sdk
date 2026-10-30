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
#include <assert.h>
#include <stdarg.h>
#include "muse_input.c"

struct test_queue { muse_input_event_t items[32]; int n; } queue;
static bool asleep, call, notes_mode, ble, menu, pairing, queue_full, speaker = true;
static int slept, toggled, powered_off, shutdown, selected, speaker_toggles;
static int64_t clock_us;
static char caption[96];
static muse_mode_t mode = MUSE_MODE_IDLE;
BaseType_t xQueueSend(QueueHandle_t q,const void *p,TickType_t t) {
    (void)t; if (queue_full) return 0;
    assert(q->n<32); q->items[q->n++]=*(const muse_input_event_t *)p; return pdTRUE;
}
bool muse_settings_speaker_on(void) { return speaker; }
void muse_settings_set_speaker_on(bool v) { speaker=v; ++speaker_toggles; }
void muse_state_set_asleep(bool v) { asleep=v; slept+=v; }
bool muse_state_asleep(void) { return asleep; }
bool muse_state_on_battery(void) { return false; }
void muse_state_poke(void) {}
void muse_state_set_level(float v) { (void)v; }
void muse_state_set_progress(float v) { (void)v; }
void muse_state_set_mode(muse_mode_t v) { mode=v; }
muse_mode_t muse_state_mode(float *v) { if(v) *v=0; return mode; }
float muse_state_idle_secs(void) { return 1000; }
void muse_state_set_caption(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(caption,sizeof(caption),fmt,ap); va_end(ap);
}
bool muse_state_caption(char *p,size_t n,uint32_t *v) { (void)v; snprintf(p,n,"%s",caption); return true; }
bool muse_voice_in_call(void) { return call; }
bool muse_voice_ptt_enabled(void) { return CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE && !notes_mode; }
bool muse_voice_resting(void) { return false; }
void muse_voice_shutdown(int ms) { assert(ms==3000); ++shutdown; call=false; }
void muse_voice_cancel_shutdown(void) { assert(powered_off); }
bool muse_wifi_connected(void) { return true; }
void muse_wifi_apply(void) {}
bool muse_console_host(void) { return true; }
bool muse_ui_dark(void) { return false; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
void vTaskDelay(int ms) { clock_us+=ms*1000; }
TickType_t xTaskGetTickCount(void) { return clock_us/1000; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "failed"; }
bool muse_settings_ble_on(void) { return ble; }
void muse_settings_set_ble_on(bool v) { ble=v; ++toggled; }
int muse_settings_sleep_s(void) { return 120; }
void muse_ble_status(muse_ble_status_t *p) { memset(p,0,sizeof(*p)); }
muse_link_state_t muse_link_state(void) { return MUSE_LINK_ONLINE; }
bool muse_link_talk_press(void) { return pairing; }
bool muse_menu_is_open(void) { return menu; }
void muse_menu_key(muse_menu_key_t key) { selected=key; }
static esp_err_t board_off(void) { assert(shutdown==1); ++powered_off; return ESP_FAIL; }
/* A two-button board with AUX (every board but the StopWatch), and the
 * StopWatch as it ships: talk, speaker and red POWER, each its own control. */
static const muse_board_t board={.touch=true,.talk_button="yellow",.aux_button="blue",.power_off=board_off};
static const muse_board_t stopwatch_board={.touch=true,.rim_controls=true,.talk_button="top-left",
    .speaker_button="top-right",.power_button="red bottom-left",.power_off=board_off};
const muse_board_t *muse_board=&board;

static void tap(void) { aux_button(true,true); aux_button(false,true); }
static void ticks(int n) { for(int i=0;i<n;i++) aux_button(false,false); }
/* The production poll path: dedicated_buttons() runs once per 10 ms poll. */
static void blue(void) { dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); dedicated_buttons(MUSE_BTN_SPEAKER_RELEASE); }
static void polls(int n) { for(int i=0;i<n;i++) dedicated_buttons(0); }
static int ends(void) { int n=0; for(int i=0;i<queue.n;i++) n+=queue.items[i].type==MUSE_PTT_END_CALL; return n; }
int main(int argc,char **argv) {
    assert(argc==2); int scenario=atoi(argv[1]); s_queue=&queue;
    bool stopwatch=CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE;
    if(scenario==1) {
        tap(); assert(!slept); ticks(DOUBLE_TICKS); assert(slept==1 && asleep);
        tap(); ticks(DOUBLE_TICKS); assert(!asleep && slept==1 && !toggled);
    } else if(scenario==2) {
        tap(); ticks(4); tap(); ticks(DOUBLE_TICKS); assert(toggled==1 && ble && !slept);
    } else if(scenario==3) {
        aux_button(true,true);
        for(int i=0;i<LONG_TICKS;i++) aux_button(true,false);
        assert(powered_off==1 && shutdown==1 && !slept);
    } else if(scenario==4) {
        /* In a call blue hangs up on the press: the speaker setting is untouched,
         * and a quick second press (a double tap) doesn't toggle it either. */
        muse_board=&stopwatch_board;
        call=true; dedicated_buttons(MUSE_BTN_SPEAKER_PRESS); call=false;
        assert(queue.n==1 && queue.items[0].type==MUSE_PTT_END_CALL && speaker_toggles==0);
        dedicated_buttons(MUSE_BTN_SPEAKER_RELEASE); polls(4); blue();
        assert(speaker_toggles==0 && queue.n==1);
        polls(DOUBLE_TICKS); blue(); assert(speaker_toggles==1 && !speaker && queue.n==1);   /* its own role again */
    } else if(scenario==5) {
        /* A talk press the voice task hasn't taken up yet: blue already ends
         * the call it would start; once it's settled, blue mutes again. */
        muse_board=&stopwatch_board;
        talk_button(MUSE_BTN_TALK_PRESS);
        assert(queue.n==1 && queue.items[0].type==MUSE_PTT_DOWN && (atomic_load(&s_call_pending)!=0)==(bool)stopwatch);
        blue();
        assert(stopwatch ? (ends()==1 && speaker_toggles==0) : (!ends() && speaker_toggles==1));
        muse_input_clear_call_pending(queue.items[0].mark); polls(DOUBLE_TICKS); blue();
        assert(speaker_toggles==(stopwatch ? 1 : 2) && ends()==(stopwatch ? 1 : 0));
    } else if(scenario==6) {
        /* A press that wakes the screen counts as a call press too. */
        muse_board=&stopwatch_board;
        asleep=true; talk_button(MUSE_BTN_TALK_PRESS);
        assert(!asleep && queue.n==1 && queue.items[0].wake);
        assert((atomic_load(&s_call_pending)!=0)==(bool)stopwatch);
        blue(); assert(stopwatch ? (ends()==1 && speaker_toggles==0) : speaker_toggles==1);
    } else if(scenario==7) {
        asleep=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!asleep && queue.n==2 && queue.items[0].wake && !queue.items[1].wake);
        queue.n=0; menu=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!queue.n && selected==MUSE_MENU_SELECT);
        menu=false; pairing=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE); assert(!queue.n);
    } else if(scenario==8) {
        /* The routing is StopWatch-with-calls only; other builds keep mute. */
        muse_board=&stopwatch_board;
        call=true; blue(); polls(DOUBLE_TICKS*2);
        assert(stopwatch ? (ends()==1 && speaker_toggles==0) : (!queue.n && speaker_toggles==1));
        /* Legacy AUX boards: a press in a call still sleeps (no call role). */
        muse_board=&board; queue.n=0;
        tap(); ticks(DOUBLE_TICKS*2); assert(!queue.n && slept==1);
    } else if(scenario==10) {
        /* A talk press within a call is a new hold, not a pending call. */
        muse_board=&stopwatch_board;
        call=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!(atomic_load(&s_call_pending)!=0));
        call=false; blue(); assert(speaker_toggles==1 && !ends());
    } else if(scenario==14) {
        /* Each pending press owns its mark: clearing an older one (the voice
         * task finishing with it) leaves a newer press's mark, so blue still
         * hangs up the call that one will start. */
        muse_board=&stopwatch_board;
        talk_button(MUSE_BTN_TALK_PRESS); talk_button(MUSE_BTN_TALK_RELEASE);
        uint32_t first=queue.items[0].mark;
        talk_button(MUSE_BTN_TALK_PRESS);
        uint32_t second=queue.items[2].mark;
        assert(stopwatch ? (first && second && first!=second) : (!first && !second));
        muse_input_clear_call_pending(first);
        assert((atomic_load(&s_call_pending)!=0)==(bool)stopwatch);
        blue(); assert(stopwatch ? (ends()==1 && !speaker_toggles) : speaker_toggles==1);
        muse_input_clear_call_pending(second); assert(!atomic_load(&s_call_pending));
        muse_input_clear_call_pending(0); assert(!atomic_load(&s_call_pending));
    } else if(scenario==11) {
        /* A hang-up that can't be queued still ends the call: the input epoch
         * moves, which the voice task treats as hung up, and nothing is pending. */
        muse_board=&stopwatch_board;
        uint32_t epoch=muse_input_call_epoch();
        call=true; queue_full=true; blue(); queue_full=false;
        assert(!queue.n && speaker_toggles==(stopwatch ? 0 : 1));
        assert(stopwatch ? muse_input_call_epoch()!=epoch : muse_input_call_epoch()==epoch);
        assert(!(atomic_load(&s_call_pending)!=0));
    } else if(scenario==12) {
        /* Screen asleep mid-call (red short press): blue wakes it and hangs up. */
        muse_board=&stopwatch_board;
        call=true;
        dedicated_buttons(MUSE_BTN_POWER_PRESS); dedicated_buttons(MUSE_BTN_POWER_RELEASE);
        assert(asleep && !queue.n);   /* the call goes on with the screen off */
        blue(); assert(stopwatch ? (!asleep && ends()==1 && speaker_toggles==0) : (!asleep && speaker_toggles==0));
    } else if(scenario==13) {
        /* Red held in a call: the talk hold is released and the call ended
         * (muse_voice_shutdown) before power goes. */
        muse_board=&stopwatch_board;
        call=true; talk_button(MUSE_BTN_TALK_PRESS); assert(s_talk_down);
        dedicated_buttons(MUSE_BTN_POWER_PRESS); dedicated_buttons(MUSE_BTN_POWER_LONG);
        assert(!s_talk_down && shutdown==1 && powered_off==1 && !call);
        assert(queue.items[queue.n-1].type==MUSE_PTT_UP);
    } else if(scenario==9) {
        call=true; check_sleep(); assert(!asleep); call=false; check_sleep(); assert(asleep);
    }
    return 0;
}
