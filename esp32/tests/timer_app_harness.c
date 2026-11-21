/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../components/muse_apps/sdk/gadget.h"

static int32_t props[80][256], parents[80], kinds[80], next_obj;
static char texts[80][192];
static uint32_t clock_ms;
static int awake, timer_on, timer_starts, fail_timer, exited;

int32_t sys_millis(void) { return (int32_t)clock_ms; }
int32_t sys_info(int32_t key) { return key == INFO_SCREEN_WIDTH || key == INFO_SCREEN_HEIGHT ? 466 : 0; }
int32_t sys_timer(int32_t id, int32_t ms, int32_t repeat)
{
    assert((id == 1 || id == 2) && ms >= 10 && (repeat == 0 || repeat == 1));
    if (fail_timer) return ERR_BUSY;
    timer_on = 1;
    timer_starts++;
    return 0;
}
int32_t sys_timer_stop(int32_t id) { assert(id == 1 || id == 2); if (id == 1) timer_on = 0; return 0; }
int32_t sys_frame_rate(int32_t fps) { assert(fps == 0); return 0; }
void sys_keep_awake(int32_t on) { awake = on; }
void sys_exit(void) { exited = 1; }
int32_t ui_create(int32_t kind, int32_t parent)
{
    assert(next_obj < 79);
    int32_t obj = ++next_obj;
    parents[obj] = parent;
    kinds[obj] = kind;
    return obj;
}
int32_t ui_set(int32_t obj, int32_t property, int32_t value)
{
    assert(obj >= 0 && obj < 80 && property > 0 && property < 256);
    props[obj][property] = value;
    return 0;
}
int32_t ui_get(int32_t obj, int32_t property) { return props[obj][property]; }
int32_t ui_set_text(int32_t obj, int32_t property, const char *text)
{
    assert(property == P_TEXT || property == P_OPTIONS);
    assert(strlen(text) < sizeof(texts[obj]));
    strcpy(texts[obj], text);
    return 0;
}
int32_t ui_align(int32_t obj, int32_t align, int32_t x, int32_t y)
{
    (void)align;
    props[obj][P_X] = x;
    props[obj][P_Y] = y;
    return 0;
}
int32_t ui_roller_select(int32_t obj, int32_t selected, int32_t animate) { assert(animate == 1); props[obj][P_SELECTED] = selected; return 0; }
int32_t ui_animate(int32_t obj,int32_t prop,int32_t to,int32_t ms,int32_t ease) { (void)obj;(void)prop;(void)to;(void)ms;(void)ease;return 0; }
int32_t ui_animate_stop(int32_t obj,int32_t prop) { (void)obj;(void)prop;return 0; }
int32_t cv_fill(int32_t c,int32_t col) { (void)c;(void)col;return 0; }
int32_t cv_arc(int32_t c,int32_t x,int32_t y,int32_t r,int32_t w,int32_t a,int32_t b,int32_t col) { (void)c;(void)x;(void)y;(void)r;(void)w;(void)a;(void)b;(void)col;return 0; }
int32_t cv_line(int32_t c,int32_t x,int32_t y,int32_t x2,int32_t y2,int32_t w,int32_t col) { (void)c;(void)x;(void)y;(void)x2;(void)y2;(void)w;(void)col;return 0; }
int32_t ui_on(int32_t obj, int32_t events) { (void)obj; (void)events; return 0; }
int32_t ui_style(int32_t obj, int32_t selector, int32_t property, int32_t value)
{
    (void)selector; props[obj][property] = value;
    return 0;
}
int32_t ui_set_parent(int32_t obj, int32_t parent) { parents[obj] = parent; return 0; }

/* Exercise the complete app callbacks and formatting, not a model of them. */
#include "../components/muse_apps/sdk/examples/timer.c"

static void fresh(void)
{
    memset(props, 0, sizeof(props));
    memset(texts, 0, sizeof(texts));
    next_obj = 0;
    clock_ms = 0;
    awake = timer_on = timer_starts = fail_timer = exited = 0;
    on_start();
}

static void pick(int h, int m, int s)
{
    props[wheels[0]][P_SELECTED] = h;
    props[wheels[1]][P_SELECTED] = m;
    props[wheels[2]][P_SELECTED] = s;
    on_event(EV_UI, wheels[2], UI_VALUE_CHANGED);
}

static void click(void) { on_event(EV_UI, action, UI_CLICKED); }
static void tick(uint32_t now) { clock_ms = now; on_event(EV_TIMER, CLOCK_TIMER, 0); }

int main(void)
{
    fresh();
    assert(props[action][P_DISABLED] && !awake && !timer_on);
    assert(kinds[wheels[0]] == W_ROLLER);
    assert(!strcmp(texts[wheels[0]] + strlen(texts[wheels[0]]) - 2, "23"));
    assert(!strcmp(texts[wheels[1]] + strlen(texts[wheels[1]]) - 2, "59"));
    click();
    assert(!timer_on && timer_starts == 0);
    pick(0, 60, 0);
    assert(props[action][P_DISABLED]);
    click();
    assert(!timer_on);

    pick(0, 1, 1);
    assert(!props[action][P_DISABLED]);
    click();
    assert(awake && timer_on && props[picker][P_HIDDEN]);
    assert(!strcmp(texts[time_text], "01:01"));
    click();   /* A queued second click must not restart the countdown. */
    assert(timer_starts == 1);
    tick(999);
    assert(!strcmp(texts[time_text], "01:01"));
    tick(1000);
    assert(!strcmp(texts[time_text], "01:00"));
    pick(23, 59, 59);   /* A stale picker event cannot change the duration. */
    tick(60001);
    assert(!strcmp(texts[time_text], "00:01"));
    tick(61000);
    assert(!timer_on && awake && !strcmp(texts[heading], "Time's up"));
    assert(!strcmp(texts[time_text], "00:00"));
    tick(90000);
    assert(!timer_on && !strcmp(texts[time_text], "00:00"));
    on_event(EV_TOUCH_DOWN, 100, 100);
    on_event(EV_TOUCH_UP, 100, 100);
    assert(!awake && !props[picker][P_HIDDEN] && !props[action][P_DISABLED]);
    assert(!strcmp(texts[action_text], "Start"));
    assert(props[wheels[0]][P_SELECTED] == 0 && props[wheels[1]][P_SELECTED] == 1 &&
           props[wheels[2]][P_SELECTED] == 1);

    /* Timing crosses both the signed boundary and a full uint32_t wrap. */
    const uint32_t starts[] = { 0x7FFFFF00u, 0xFFFFFF00u };
    for (unsigned i = 0; i < 2; i++) {
        fresh(); pick(0, 0, 2); clock_ms = starts[i]; click();
        tick(starts[i] + 1999);
        assert(timer_on && !strcmp(texts[time_text], "00:01"));
        tick(starts[i] + 2000);
        assert(!timer_on && !strcmp(texts[time_text], "00:00"));
    }

    fresh(); pick(23, 59, 59); click();
    assert(!strcmp(texts[time_text], "23:59:59"));
    clock_ms = 86399000u;   /* A late wake catches up, independent of UI ticks. */
    on_event(EV_SCREEN, 1, 0);
    assert(!timer_on && !strcmp(texts[heading], "Time's up"));

    fresh(); pick(0, 0, 3); fail_timer = 1; click();
    assert(!props[picker][P_HIDDEN] && !timer_on && !awake);
    fail_timer = 0; click();
    assert(timer_on && awake);
    on_event(EV_UI, close_button, UI_CLICKED);
    assert(exited);
    on_stop(STOP_EXIT);
    assert(!timer_on && !awake);
    fresh(); pick(0, 0, 10); click();
    clock_ms = 1200; countdown_tap();
    assert(phase == PAUSED && remaining_ms == 8800 && !timer_on);
    clock_ms = 1300; countdown_tap();
    assert(phase == FINISHED && !strcmp(texts[heading], "Time's up"));
    clock_ms = 1701; countdown_tap();
    assert(phase == SETTING && props[wheels[2]][P_SELECTED] == 10);
    on_event(EV_BUTTON, BUTTON_PRIMARY, BTN_DOWN);
    assert(resetting && props[action][P_DISABLED] && props[wheels[2]][P_SELECTED] == 0);
    pick(0, 1, 0); /* Events cannot enable Start while the reset animation runs. */
    assert(props[action][P_DISABLED]);
    on_event(EV_TIMER, RESET_TIMER, 0);
    assert(!resetting && props[wheels[2]][P_SELECTED] == 0);
    fresh(); pick(0, 0, 5); click(); clock_ms = 1000; countdown_tap();
    clock_ms = 10000; countdown_tap(); /* Time paused does not consume duration. */
    assert(phase == COUNTING && remaining_ms == 4000);
    tick(13999); assert(phase == COUNTING);
    tick(14000); assert(phase == FINISHED);
    fresh(); pick(0, 0, 5);
    on_event(EV_UI, reset_hint, UI_CLICKED);
    assert(picker_seconds() == 5); /* Header icon is an indicator only. */
    click(); clock_ms = 500;
    on_event(EV_TOUCH_DOWN, 100, 100); on_event(EV_TOUCH_MOVE, 130, 100); on_event(EV_TOUCH_UP, 130, 100);
    assert(phase == COUNTING);
    on_event(EV_TOUCH_DOWN, 100, 100); on_event(EV_TOUCH_UP, 100, 100);
    assert(phase == PAUSED && props[ring][P_OPA] == 60);
    clock_ms = 600;
    on_event(EV_TOUCH_DOWN, 100, 100); on_event(EV_TOUCH_UP, 100, 100);
    assert(phase == FINISHED && props[ring][P_OPA] == 255 && props[ring][P_ARC_COLOR] == 0xFF3030);
    countdown_tap(); assert(phase == FINISHED); /* No instant dismissal from the finishing gesture. */
    fresh(); pick(0, 0, 5); click();
    assert(props[action][P_HIDDEN]);
    on_event(EV_BUTTON, BUTTON_SECONDARY, BTN_DOWN);
    assert(exited); /* No exit confirmation. */
    fresh(); pick(0, 0, 5); click();
    assert(props[action][P_HIDDEN]);
    clock_ms = 600; countdown_tap();
    assert(phase == PAUSED && props[action][P_HIDDEN]);
    click(); /* Events from the hidden Start button do not resume or finish. */
    assert(phase == PAUSED);
    on_event(EV_BUTTON, BUTTON_PRIMARY, BTN_DOWN);
    assert(phase == SETTING && picker_seconds() == 5 && !timer_on);
    assert(!props[action][P_HIDDEN] && !props[action][P_DISABLED]);
    click(); tick(clock_ms + 5000);
    assert(phase == FINISHED && props[action][P_HIDDEN]);
    clock_ms += 500;
    on_event(EV_TOUCH_DOWN, 100, 100); on_event(EV_TOUCH_UP, 100, 100);
    assert(phase == SETTING && !props[action][P_HIDDEN] && picker_seconds() == 5);
    puts("timer app: picker, countdown boundaries, delayed ticks, wrap and cleanup passed");
    return 0;
}
