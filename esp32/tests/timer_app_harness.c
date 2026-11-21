/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../components/muse_apps/sdk/gadget.h"

static int32_t props[48][256], parents[48], kinds[48], next_obj;
static char texts[48][192];
static uint32_t clock_ms;
static int awake, timer_on, timer_starts, fail_timer, exited;

int32_t sys_millis(void) { return (int32_t)clock_ms; }
int32_t sys_info(int32_t key) { return key == INFO_SCREEN_WIDTH || key == INFO_SCREEN_HEIGHT ? 466 : 0; }
int32_t sys_timer(int32_t id, int32_t ms, int32_t repeat)
{
    assert(id == 1 && ms >= 10 && repeat == 1);
    if (fail_timer) return ERR_BUSY;
    timer_on = 1;
    timer_starts++;
    return 0;
}
int32_t sys_timer_stop(int32_t id) { assert(id == 1); timer_on = 0; return 0; }
int32_t sys_frame_rate(int32_t fps) { assert(fps == 0); return 0; }
void sys_keep_awake(int32_t on) { awake = on; }
void sys_exit(void) { exited = 1; }
int32_t ui_create(int32_t kind, int32_t parent)
{
    assert(next_obj < 47);
    int32_t obj = ++next_obj;
    parents[obj] = parent;
    kinds[obj] = kind;
    return obj;
}
int32_t ui_set(int32_t obj, int32_t property, int32_t value)
{
    assert(obj >= 0 && obj < 48 && property > 0 && property < 256);
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
int32_t ui_on(int32_t obj, int32_t events) { (void)obj; (void)events; return 0; }
int32_t ui_style(int32_t obj, int32_t selector, int32_t property, int32_t value)
{
    (void)obj; (void)selector; (void)property; (void)value;
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
    assert(!timer_on && awake && !strcmp(texts[heading], "TIME'S UP"));
    assert(!strcmp(texts[time_text], "00:00"));
    tick(90000);
    assert(!timer_on && !strcmp(texts[time_text], "00:00"));
    click();
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
    assert(!timer_on && !strcmp(texts[heading], "TIME'S UP"));

    fresh(); pick(0, 0, 3); fail_timer = 1; click();
    assert(!props[picker][P_HIDDEN] && !timer_on && !awake);
    fail_timer = 0; click();
    assert(timer_on && awake);
    on_event(EV_UI, close_button, UI_CLICKED);
    assert(exited);
    on_stop(STOP_EXIT);
    assert(!timer_on && !awake);
    puts("timer app: picker, countdown boundaries, delayed ticks, wrap and cleanup passed");
    return 0;
}
