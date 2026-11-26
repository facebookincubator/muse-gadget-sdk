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

/* LVGL profiler hooks for the bench: see muse_lv_profiler.h. */
#include "muse_lv_profiler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/core/lv_global.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_private.h"

#define SLOTS 96        /* distinct tags */
#define STACKS 6        /* threads that run LVGL code: its task and the draw thread, with room */
#define DEPTH 32

typedef struct {
    const char *tag;
    uint32_t calls;
    uint64_t us;
} slot_t;

typedef struct {
    TaskHandle_t task;
    int depth;
    struct {
        const char *tag;     /* as LVGL passed it, to match the end */
        const char *name;    /* as tallied */
        uint32_t start;
    } f[DEPTH];
} stack_t;

static volatile bool s_on;
static slot_t s_slots[SLOTS];
static stack_t s_stacks[STACKS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_draw_sw_unit_t *s_sw;

static const char *const TYPE_NAMES[] = {
    [LV_DRAW_TASK_TYPE_NONE] = "draw:none",
    [LV_DRAW_TASK_TYPE_FILL] = "draw:fill",
    [LV_DRAW_TASK_TYPE_BORDER] = "draw:border",
    [LV_DRAW_TASK_TYPE_BOX_SHADOW] = "draw:shadow",
    [LV_DRAW_TASK_TYPE_LETTER] = "draw:letter",
    [LV_DRAW_TASK_TYPE_LABEL] = "draw:label",
    [LV_DRAW_TASK_TYPE_IMAGE] = "draw:image",
    [LV_DRAW_TASK_TYPE_LAYER] = "draw:layer",
    [LV_DRAW_TASK_TYPE_LINE] = "draw:line",
    [LV_DRAW_TASK_TYPE_ARC] = "draw:arc",
    [LV_DRAW_TASK_TYPE_TRIANGLE] = "draw:triangle",
    [LV_DRAW_TASK_TYPE_MASK_RECTANGLE] = "draw:mask_rect",
    [LV_DRAW_TASK_TYPE_MASK_BITMAP] = "draw:mask_bitmap",
    [LV_DRAW_TASK_TYPE_BLUR] = "draw:blur",
};

static inline uint32_t now_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static stack_t *my_stack(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < STACKS; i++) {
        if (s_stacks[i].task == me) {
            return &s_stacks[i];
        }
    }
    stack_t *st = NULL;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < STACKS && !st; i++) {
        if (!s_stacks[i].task) {
            s_stacks[i].task = me;
            s_stacks[i].depth = 0;
            st = &s_stacks[i];
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

/* The software draw thread is about to run a task: which kind. */
static const char *draw_task_name(void)
{
    if (!s_sw) {
        for (lv_draw_unit_t *u = LV_GLOBAL_DEFAULT()->draw_info.unit_head; u; u = u->next) {
            if (u->name && !strcmp(u->name, "SW")) {
                s_sw = (lv_draw_sw_unit_t *)u;
            }
        }
    }
    if (s_sw) {
        for (int i = 0; i < LV_DRAW_SW_DRAW_UNIT_CNT; i++) {
            lv_draw_task_t *t = s_sw->thread_dscs[i].task_act;
            if (t && t->type < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]) && TYPE_NAMES[t->type]) {
                return TYPE_NAMES[t->type];
            }
        }
    }
    return "draw:?";
}

void muse_lv_prof_begin(const char *tag)
{
    if (!s_on) {
        return;
    }
    stack_t *st = my_stack();
    if (!st || st->depth >= DEPTH) {
        return;
    }
    const char *name = tag;
    if (tag[0] == 'e' && !strcmp(tag, "execute_drawing")) {
        name = draw_task_name();
    }
    st->f[st->depth].tag = tag;
    st->f[st->depth].name = name;
    st->f[st->depth].start = now_us();
    st->depth++;
}

void muse_lv_prof_end(const char *tag)
{
    if (!s_on) {
        return;
    }
    uint32_t t = now_us();
    stack_t *st = my_stack();
    if (!st || st->depth == 0 || st->f[st->depth - 1].tag != tag) {
        return;   /* began before counting started */
    }
    st->depth--;
    const char *name = st->f[st->depth].name;
    uint32_t us = t - st->f[st->depth].start;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        if (s_slots[i].tag == name || !s_slots[i].tag) {
            s_slots[i].tag = name;
            s_slots[i].calls++;
            s_slots[i].us += us;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
}

void muse_lv_prof_enable(bool on)
{
    if (on) {
        taskENTER_CRITICAL(&s_lock);
        memset(s_slots, 0, sizeof(s_slots));
        for (int i = 0; i < STACKS; i++) {
            s_stacks[i].depth = 0;
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    s_on = on;
}

bool muse_lv_prof_enabled(void)
{
    return s_on;
}

static int by_time(const void *a, const void *b)
{
    uint64_t x = ((const slot_t *)a)->us, y = ((const slot_t *)b)->us;
    return x < y ? 1 : x > y ? -1 : 0;
}

void muse_lv_prof_report(char *buf, size_t cap, int top, int frames)
{
    static slot_t sorted[SLOTS];
    taskENTER_CRITICAL(&s_lock);
    memcpy(sorted, s_slots, sizeof(sorted));
    taskEXIT_CRITICAL(&s_lock);
    qsort(sorted, SLOTS, sizeof(sorted[0]), by_time);
    if (frames < 1) {
        frames = 1;
    }
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < top && i < SLOTS && sorted[i].tag && len < cap; i++) {
        uint32_t per = (uint32_t)(sorted[i].us / frames);
        len += snprintf(buf + len, cap - len, " %s %u.%u ms/frame (%u)", sorted[i].tag, (unsigned)(per / 1000),
                        (unsigned)(per / 100 % 10), (unsigned)sorted[i].calls);
    }
}
