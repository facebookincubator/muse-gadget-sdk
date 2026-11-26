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
static bool on_battery = true;   /* muse_state's: false on USB (or with no battery reading) */
bool muse_state_on_battery(void) { return on_battery; }
/* The real power, for the power-off's gate (muse_power_off_ok): on battery as on_battery says. */
muse_power_t muse_state_power(void) { return (muse_power_t){ .battery_pct = on_battery ? 50 : -1, .usb = !on_battery }; }
static int pokes;   /* activity: a press, or a USB plug-in on a lit screen */
void muse_state_poke(void) { ++pokes; }
void muse_state_set_level(float v) { (void)v; }
void muse_state_set_progress(float v) { (void)v; }
void muse_state_set_mode(muse_mode_t v) { mode=v; }
muse_mode_t muse_state_mode(float *v) { if(v) *v=0; return mode; }
static float idle_s = 1000;
float muse_state_idle_secs(void) { return idle_s; }
static bool own_turn;   /* The turn is the gadget's own (its talk press) */
void muse_state_set_own_turn(bool v) { own_turn=v; }
bool muse_state_own_turn(void) { return own_turn; }
static bool reply_hold;   /* A client's stream_audio reaching the speaker, or subtitles' band on screen */
bool muse_voice_reply_holds_panel(void) { return reply_hold; }
void muse_state_set_caption(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); vsnprintf(caption,sizeof(caption),fmt,ap); va_end(ap);
}
void muse_state_set_hint(const char *text) { snprintf(caption,sizeof(caption),"%s",text); }
bool muse_state_hint_pending(void) { return false; }
bool muse_state_caption(char *p,size_t n,uint32_t *v) { (void)v; snprintf(p,n,"%s",caption); return true; }
bool muse_voice_in_call(void) { return call; }
bool muse_voice_ptt_enabled(void) { return CONFIG_MUSE_BOARD_M5STACK_STOPWATCH && CONFIG_MUSE_REALTIME_VOICE && !notes_mode; }
bool muse_voice_resting(void) { return false; }
static int talk_releases;
void muse_voice_talk_released(void) { ++talk_releases; }
static int talk_presses;
void muse_voice_talk_pressed(void) { ++talk_presses; }   /* the barge-in pause at touch-down */
muse_talk_mode_t muse_settings_talk_mode(void) { return MUSE_TALK_BOTH; }
bool muse_ui_has_hold_region(void) { return false; }
void muse_voice_shutdown(int ms) { assert(ms==3000); ++shutdown; call=false; }
void muse_voice_cancel_shutdown(void) { assert(powered_off); }
bool muse_wifi_connected(void) { return true; }
void muse_wifi_apply(void) {}
bool muse_console_host(void) { return true; }
bool muse_ui_dark(void) { return false; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
void vTaskDelay(int ms) { clock_us+=ms*1000; }
TickType_t xTaskGetTickCount(void) { return clock_us/1000; }
int64_t esp_timer_get_time(void) { return clock_us; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "failed"; }
bool muse_settings_ble_on(void) { return ble; }
void muse_settings_set_ble_on(bool v) { ble=v; ++toggled; }
void muse_ble_phone_setup(bool v) { muse_settings_set_ble_on(v); }
int muse_settings_sleep_s(void) { return 120; }
static bool sleep_when_charged = true;   /* Display > Sleep when charged: Always (true, the default) or Never */
bool muse_settings_sleep_when_charged(void) { return sleep_when_charged; }
void muse_ble_status(muse_ble_status_t *p) { memset(p,0,sizeof(*p)); }
muse_link_state_t muse_link_state(void) { return MUSE_LINK_ONLINE; }
bool muse_link_talk_press(void) { return pairing; }
bool muse_link_confirm_press(int64_t released_us, uint32_t down_seq) { (void)released_us; (void)down_seq; return pairing; }
uint32_t muse_link_prompt_seq(void) { return 0; }   /* the press-before-the-prompt rule: test_pairing_fresh_press */
bool muse_menu_is_open(void) { return menu; }
/* The passcode lock: unlocked unless a scenario locks it. lapse_due: "After 5 min" lapsed in the dark, applied at
 * the next press (muse_passcode_locked_at_press) or the UI's tick. */
static bool locked, lapse_due;
static int keypads;
bool muse_passcode_locked(void) { return locked; }
bool muse_passcode_locked_at_press(void) { locked |= lapse_due; lapse_due = false; return locked; }
void muse_passcode_request_keypad(muse_pass_why_t why) { assert(why==MUSE_PASS_WHY_TALK); ++keypads; }
void muse_menu_key(muse_menu_key_t key) { selected=key; }
static esp_err_t board_off(void) { assert(shutdown==1); ++powered_off; return ESP_FAIL; }
/* A two-button board with AUX (every board but the StopWatch), and the
 * StopWatch as it ships: talk, speaker and red POWER, each its own control. */
static esp_err_t board_mic(void) { return ESP_OK; }
static const muse_board_t board={.touch=true,.talk_button="yellow",.aux_button="blue",.power_off=board_off,
    .mic_init=board_mic};
static const muse_board_t stopwatch_board={.touch=true,.rim_controls=true,.talk_button="top-left",
    .speaker_button="top-right",.power_button="red bottom-left",.power_off=board_off,.mic_init=board_mic};
/* A board whose charger reports charge done, and one with the %
 * fallback (90 % takes, 85 % lets go); `board` hides Sleep when charged. */
static const muse_board_t done_board={.touch=true,.talk_button="yellow",.aux_button="blue",.power_off=board_off,
    .mic_init=board_mic,.charged=MUSE_CHARGED_DONE};
static const muse_board_t pct_board={.touch=true,.talk_button="yellow",.aux_button="blue",.power_off=board_off,
    .mic_init=board_mic,.charged=MUSE_CHARGED_PERCENT};
/* No speaker or microphone (the Waveshare 1.43, LCD-1.28): no voice task. */
static const muse_board_t display_board={.touch=true,.talk_button="boot",.power_off=board_off};
/* No touch: the aux button opens the menu (the AIPI). */
static const muse_board_t keys_board={.talk_button="bottom right",.aux_button="bottom left",.power_off=board_off,
    .mic_init=board_mic};
const muse_board_t *muse_board=&board;
static muse_power_t pw(bool usb, int pct, bool done) { return (muse_power_t){.usb=usb,.battery_pct=pct,.charge_done=done}; }
static void reading(bool usb, int pct, bool done) { muse_power_t p=pw(usb,pct,done); note_power(&p,true); }

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
        /* Locked: the press opens the keypad and is dropped (the pairing's
         * confirming press still comes first), never replayed once unlocked;
         * a fresh press talks. */
        pairing=false; locked=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!queue.n && keypads==1);
        pairing=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE); assert(!queue.n && keypads==1);
        pairing=false; locked=false; talk_button(0); assert(!queue.n);
        talk_button(MUSE_BTN_TALK_PRESS); assert(queue.n==1 && queue.items[0].type==MUSE_PTT_DOWN);
    } else if(scenario==24) {
        /* "After 5 min": the screen dark that long, before the UI's tick has applied the lock, a talk press wakes it
         * and opens the keypad; it never talks. */
        asleep=true; lapse_due=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!asleep && locked && !queue.n && keypads==1);
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
    } else if(scenario==22) {
        /* Display-only: nothing reads the queue, so a talk press never posts.
         * It still wakes the screen, selects in the menu and confirms pairing. */
        muse_board=&display_board;
        asleep=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!asleep && !queue.n && !s_talk_down);
        for(int i=0;i<40;i++) { talk_button(MUSE_BTN_TALK_PRESS); talk_button(MUSE_BTN_TALK_RELEASE); }
        assert(!queue.n && !s_talk_down && !atomic_load(&s_call_pending));
        menu=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(!queue.n && selected==MUSE_MENU_SELECT);
        menu=false; pairing=true; talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE); assert(!queue.n);
#if CONFIG_MUSE_VOICE_NOTE_TOGGLE
    } else if(scenario==23) {
        /* A toggled note records with talk let go: no button hides it. */
        muse_input_note_toggled(true);
        tap(); ticks(DOUBLE_TICKS*2); assert(!asleep && !slept);              /* aux: no sleep */
        dedicated_buttons(MUSE_BTN_POWER_PRESS); dedicated_buttons(MUSE_BTN_POWER_RELEASE);
        assert(!asleep && !slept);                                           /* power: no sleep */
        muse_board=&keys_board; selected=-1;
        menu_button(true,true); menu_button(false,true); assert(selected==-1);   /* no menu */
        talk_button(MUSE_BTN_TALK_PRESS|MUSE_BTN_TALK_RELEASE);
        assert(selected==-1 && queue.n==2 && queue.items[0].type==MUSE_PTT_DOWN);   /* stops the note */
        muse_input_note_toggled(false);
        muse_board=&board; tap(); ticks(DOUBLE_TICKS); assert(asleep && slept==1);   /* sleeps again */
#endif
    } else if(scenario==9) {
        call=true; check_sleep(); assert(!asleep); call=false; check_sleep(); assert(asleep);
        /* On USB the screen times out as on battery (no USB keep-alive). */
        asleep=false; on_battery=false; check_sleep(); assert(asleep); on_battery=true;
    } else if(scenario==20) {
        /* Sleep when charged, through note_power() and check_sleep()
         * (sleep after 120 s, the last touch long ago), on a board whose
         * charger reports charge done. */
        clock_us=1000000000; idle_s=1000; own_turn=false; mode=MUSE_MODE_IDLE;
        muse_board=&done_board;
        /* USB x charged x setting: only USB + charged + Never holds. */
        for(int usb=0;usb<2;usb++) for(int done=0;done<2;done++) for(int never=0;never<2;never++) {
            on_battery=!usb; sleep_when_charged=!never;
            reading(false,50,false);   /* unplugged first: each case is a plug-in of its own */
            reading(usb,50,done); asleep=false; check_sleep();
            bool held=usb && done && never;
            assert(muse_input_charged_hold()==held && asleep==!held);
        }
        /* Never, charged on USB: a lit screen stays lit however long. */
        on_battery=false; sleep_when_charged=false; reading(true,50,true); asleep=false; slept=0;
        for(int s=0;s<600;s++) { check_sleep(); assert(!asleep); clock_us+=1000000; }
        /* The charger tops up after termination (done drops): the latch holds. */
        for(int s=0;s<300;s++) { reading(true,97,s%60<30 ? false : true); check_sleep(); assert(!asleep); clock_us+=1000000; }
        /* Unplugged: it lets go, and the usual timeout runs from there. */
        on_battery=true; reading(false,97,false); assert(!muse_input_charged_hold());
        clock_us+=1000000; check_sleep(); assert(!asleep);
        clock_us+=118000000; check_sleep(); assert(!asleep);              /* 119 s after */
        clock_us+=2000000; check_sleep(); assert(asleep && slept==1);     /* 121 s */
        /* Plugged in again: not held until the charger's next done (the
         * plug-in itself is a wake, and the usual timeout runs). */
        on_battery=false; reading(true,99,false); assert(!asleep && !muse_input_charged_hold());
        clock_us+=121000000; idle_s=121; check_sleep(); assert(asleep);
        idle_s=1000;
        /* Dark when it becomes charged stays dark: Never only holds a lit screen. */
        reading(true,99,true); assert(asleep && muse_input_charged_hold());
        for(int s=0;s<30;s++) { check_sleep(); assert(asleep); clock_us+=1000000; }
        /* "Charged" is tracked whatever the setting: only
         * unplug clears it. Never -> Always during a top-up: no hold (the usual
         * timeout), the latch kept; back at Never it holds at once. */
        reading(true,99,false); assert(muse_input_charged_hold());
        sleep_when_charged=true; check_sleep(); assert(!muse_input_charged_hold());
        reading(true,99,false); assert(!muse_input_charged_hold());
        sleep_when_charged=false; check_sleep(); assert(muse_input_charged_hold());
        reading(true,99,false); assert(muse_input_charged_hold());
        /* Charged at Always, the charger topping up, then Never: a lit screen
         * is held at once, with no reading in between. */
        on_battery=true; reading(false,99,false); on_battery=false;
        sleep_when_charged=true; reading(true,99,false); reading(true,100,true); assert(!muse_input_charged_hold());
        reading(true,99,false); assert(!muse_input_charged_hold());
        clock_us+=121000000; asleep=false; slept=0; check_sleep(); assert(asleep && slept==1);   /* Always: the usual timeout */
        asleep=false; sleep_when_charged=false;
        for(int s=0;s<300;s++) { check_sleep(); assert(!asleep && muse_input_charged_hold()); clock_us+=1000000; }
        /* Unplugged at Always: cleared; plugged again, no hold at Never until the next done. */
        sleep_when_charged=true; on_battery=true; reading(false,99,false); on_battery=false; reading(true,99,false);
        sleep_when_charged=false; check_sleep(); assert(!muse_input_charged_hold());
        reading(true,99,true); assert(muse_input_charged_hold());
        /* The charger's word, never the percentage: 100 % without done isn't charged. */
        reading(false,100,false); reading(true,100,false); assert(!muse_input_charged_hold());
        reading(true,0,true); assert(muse_input_charged_hold());
        /* A failed read isn't an unplug: the latch stays, but an unknown
         * reading holds nothing; a good one with USB
         * holds again at once, mid top-up, with no plug-in wake. */
        muse_power_t p=pw(true,50,true); note_power(&p,false); assert(!muse_input_charged_hold());
        asleep=true; reading(true,50,false); assert(muse_input_charged_hold() && asleep);   /* no wake on recovery */
        /* Failed reads while unplugged can't keep a lit screen on: the usual
         * timeout runs; the first good reading without USB clears the latch. */
        asleep=false; slept=0; note_power(&p,false);
        for(int s=0;s<121;s++) { note_power(&p,false); check_sleep(); clock_us+=1000000; }
        check_sleep(); assert(asleep && slept==1);
        asleep=false;
        on_battery=true; reading(false,50,false); on_battery=false; reading(true,50,false);
        assert(!muse_input_charged_hold());
        note_power(&p,false); assert(!muse_input_charged_hold());   /* nor does one make it charged */
        reading(true,50,true); assert(muse_input_charged_hold());
        /* Unplugged (done can't hold without USB), or a bench's as-if-battery. */
        on_battery=true; reading(false,100,true); assert(!muse_input_charged_hold());
        reading(true,100,true); assert(!muse_input_charged_hold());   /* on_battery still true: as if on battery */
        /* A plug-in is a wake: a dark screen on battery lights when USB arrives... */
        on_battery=true; reading(false,60,false); asleep=true; slept=0;
        on_battery=false; reading(true,60,false); assert(!asleep);
        /* ...a second reading on USB is no plug-in, and one on a lit screen restarts its timer. */
        asleep=true; reading(true,60,false); assert(asleep);
        reading(false,60,false); asleep=false; int before=pokes; reading(true,60,false); assert(pokes==before+1 && !asleep);
        /* A failed read in between keeps USB's state: no plug-in when it was in all along. */
        asleep=true; note_power(&p,false); reading(true,60,false); assert(asleep);
        /* The % fallback: 90 % takes, below 85 % lets go (hysteresis), unknown is not charged. */
        muse_board=&pct_board; on_battery=false; sleep_when_charged=false;
        int pcts[]={89,90,86,85,84,89,90,100,-1,95};
        bool want[]={0,1,1,1,0,0,1,1,0,1};
        for(int i=0;i<10;i++) { reading(true,pcts[i],false); assert(muse_input_charged_hold()==want[i]); }
        reading(false,95,false); assert(!muse_input_charged_hold());   /* unplugged */
        /* A board that can't tell: the setting is hidden and never holds. */
        muse_board=&board; reading(true,100,true); assert(!muse_input_charged_hold());
        muse_board=&done_board; sleep_when_charged=true; reading(true,100,true); assert(!muse_input_charged_hold());
    } else if(scenario==21) {
        /* Auto-sleep (#281): fresh activity must not sleep, the gadget's own
         * turn in an active mode must not sleep, and a stale clock in IDLE
         * still sleeps. */
        idle_s=10; check_sleep(); assert(!asleep);   /* just poked (e.g. by a turn) */
        idle_s=1000; own_turn=true;
        mode=MUSE_MODE_SPEAKING; check_sleep(); assert(!asleep);
        mode=MUSE_MODE_THINKING; check_sleep(); assert(!asleep);
        mode=MUSE_MODE_LISTENING; check_sleep(); assert(!asleep);
        own_turn=false; mode=MUSE_MODE_IDLE; clock_us+=121000000;
        check_sleep(); assert(asleep);   /* truly idle: sleeps */
    } else {
        fprintf(stderr, "unknown input scenario %d\n", scenario);
        return 2;
    }
    return 0;
}
