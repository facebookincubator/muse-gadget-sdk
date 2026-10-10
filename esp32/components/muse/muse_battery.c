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

#include "muse_battery.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "muse_battery";

#if CONFIG_PM_PROFILING
#define MAX_LOCKS 24
#define DUMP_MAX 3072

/* Power-management modes, by the strongest lock held: none (light sleep allowed), none
 * (light sleep off), APB_FREQ_MAX (a peripheral), CPU_FREQ_MAX (a core running a task). */
enum { MODE_SLEEP, MODE_APB_MIN, MODE_APB_MAX, MODE_CPU_MAX, MODE_COUNT };
static const char *const MODE_NAMES[MODE_COUNT] = { "SLEEP", "APB_MIN", "APB_MAX", "CPU_MAX" };

typedef struct {
    char name[16], type[16];
    int64_t held_us;
    int taken;
} lock_stat_t;

typedef struct {
    int64_t at_us;   /* 0 if they couldn't be read */
    int64_t mode_us[MODE_COUNT];
    long rejects;    /* light sleeps refused, a wake-up already pending */
    int n;
    lock_stat_t locks[MAX_LOCKS];
} pm_stats_t;

typedef struct {
    const lock_stat_t *lock;
    int64_t held_us;
    int taken;
} lock_delta_t;

static char *s_dump;
static lock_delta_t s_deltas[MAX_LOCKS];
#endif

/* The counters at one moment; a measurement is the difference of two. */
typedef struct {
    int64_t at_us;
    int64_t slept_us;
    uint32_t sleeps;
#if CONFIG_PM_PROFILING
    pm_stats_t pm;
#endif
} snap_t;

/*
 * The measurement as JSON, kept in RTC memory across resets (not power loss):
 * saved when it starts, when USB comes back, and on battery at most every
 * SAVE_MS, from a battery reading the input task takes anyway, so it never
 * wakes the chip. Plugging back in resets some boards once their port opens,
 * and a board can crash on battery; either way the next boot reports the last
 * one saved. The USB-back and periodic saves also go to flash, for a battery
 * that runs flat. The one at the start doesn't, or each boot on battery would
 * replace the run with an empty one.
 */
#define SAVE_MS (30 * 60 * 1000)
#define NVS_NS "muse_battery"
#define SAVED_MAGIC 0x4d424154   /* "MBAT" */
#if CONFIG_PM_PROFILING
#define SAVED_MAX 2048           /* two dozen power locks */
#else
#define SAVED_MAX 384
#endif

typedef struct {
    uint32_t magic, len, sum;
    char json[SAVED_MAX];
} saved_t;

static RTC_NOINIT_ATTR saved_t s_saved;
static char *s_prev;             /* the one saved before this boot, until power.reset */
static int64_t s_saved_us;
static nvs_handle_t s_nvs;
static uint16_t s_boots;         /* boots on battery since USB power was last seen, in flash */
static bool s_seen_usb;          /* on USB power at some point since boot (">nap" included) */
static bool s_counted;           /* this boot is in s_boots */

static SemaphoreHandle_t s_lock;
static snap_t *s_start, *s_end, *s_now;   /* s_end once stopped; s_now for reads while running */
static bool s_started, s_running;
static int s_pct_start, s_pct_now, s_mv_start, s_mv_now;
static int64_t s_off_us, s_rest_us, s_last_us;   /* screen off, resting, and when last added to */
static bool s_screen_off, s_resting;
static muse_power_t s_power = { .battery_pct = -1 };

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static portMUX_TYPE s_sleep_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_slept_us;
static uint32_t s_sleeps;

/* The idle task, on every wake from automatic light sleep. */
static esp_err_t IRAM_ATTR on_wake(int64_t slept_us, void *arg)
{
    (void)arg;
    if (slept_us > 0) {
        portENTER_CRITICAL_ISR(&s_sleep_lock);
        s_slept_us += slept_us;
        s_sleeps++;
        portEXIT_CRITICAL_ISR(&s_sleep_lock);
    }
    return ESP_OK;
}
#endif

#if CONFIG_PM_PROFILING
static int split(char *line, char **w, int max)
{
    int n = 0;
    char *save;
    for (char *t = strtok_r(line, " \r", &save); t && n < max; t = strtok_r(NULL, " \r", &save)) {
        w[n++] = t;
    }
    return n;
}

/* IDF keeps these counters to itself; esp_pm_dump_locks() is the only way out. */
static void pm_read(pm_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    memset(s_dump, 0, DUMP_MAX);
    FILE *f = fmemopen(s_dump, DUMP_MAX - 1, "w");
    if (!f) {
        return;
    }
    esp_pm_dump_locks(f);
    fclose(f);

    enum { OTHER, LOCKS, MODES } section = OTHER;
    char *save;
    for (char *line = strtok_r(s_dump, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *w[8];
        int n;
        if (!strncmp(line, "Time since boot up:", 19)) {
            out->at_us = strtoll(line + 19, NULL, 10);
        } else if (!strcmp(line, "Lock stats:")) {
            section = LOCKS;
        } else if (!strcmp(line, "Mode stats:")) {
            section = MODES;
        } else if (!strncmp(line, "light_sleep_counts:", 19)) {
            char *r = strstr(line, "reject_counts:");
            out->rejects = r ? strtol(r + 14, NULL, 10) : 0;
        } else if ((n = split(line, w, 8)) >= 7 && section == LOCKS && strcmp(w[0], "Name") && out->n < MAX_LOCKS) {
            /* Name Type Arg Active Total_count Time(us) Time(%), and "5  %" is two words */
            lock_stat_t *l = &out->locks[out->n++];
            strlcpy(l->name, w[0], sizeof(l->name));
            strlcpy(l->type, w[1], sizeof(l->type));
            l->taken = atoi(w[4]);
            l->held_us = strtoll(w[5], NULL, 10);
        } else if (section == MODES && n >= 3) {
            /* Mode CPU_freq Time(us) Time(%), the frequency "240M" or "40 M" */
            int t = w[1][strlen(w[1]) - 1] == 'M' ? 2 : 3;
            for (int i = 0; i < MODE_COUNT && t < n; i++) {
                if (!strcmp(w[0], MODE_NAMES[i])) {
                    out->mode_us[i] = strtoll(w[t], NULL, 10);
                }
            }
        }
    }
}

/* Each lock's use since the start, matched to its start counters by name and type. */
static int lock_deltas(const pm_stats_t *end)
{
    const pm_stats_t *start = &s_start->pm;
    bool used[MAX_LOCKS] = { 0 };
    for (int i = 0; i < end->n; i++) {
        const lock_stat_t *l = &end->locks[i];
        s_deltas[i] = (lock_delta_t){ .lock = l, .held_us = l->held_us, .taken = l->taken };
        for (int j = 0; j < start->n; j++) {
            const lock_stat_t *was = &start->locks[j];
            if (!used[j] && !strcmp(was->name, l->name) && !strcmp(was->type, l->type)) {
                used[j] = true;
                if (l->held_us >= was->held_us) {   /* else a new lock under an old name */
                    s_deltas[i].held_us -= was->held_us;
                    s_deltas[i].taken -= was->taken;
                }
                break;
            }
        }
    }
    return end->n;
}
#endif

static void snap(snap_t *s)
{
    s->at_us = esp_timer_get_time();
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    portENTER_CRITICAL(&s_sleep_lock);
    s->slept_us = s_slept_us;
    s->sleeps = s_sleeps;
    portEXIT_CRITICAL(&s_sleep_lock);
#endif
#if CONFIG_PM_PROFILING
    pm_read(&s->pm);
#endif
}

static void accrue(int64_t now)
{
    if (s_running) {
        s_off_us += s_screen_off ? now - s_last_us : 0;
        s_rest_us += s_resting ? now - s_last_us : 0;
    }
    s_last_us = now;
}

static void persist(bool flash);

static void save_boots(uint16_t n)
{
    s_boots = n;
    if (s_nvs) {
        nvs_set_u16(s_nvs, "boots", n);
        nvs_commit(s_nvs);
    }
}

static void start(void)
{
    snap(s_start);
    s_started = s_running = true;
    s_pct_start = s_pct_now = s_power.battery_pct;
    s_mv_start = s_mv_now = s_power.battery_mv;
    s_off_us = s_rest_us = 0;
    s_last_us = s_start->at_us;
    ESP_LOGI(TAG, "on battery at %d%% (%d mV): measuring", s_pct_start, s_mv_start);
    persist(false);
}

static void stop(void)
{
    snap(s_end);
    accrue(s_end->at_us);
    s_running = false;
    ESP_LOGI(TAG, "USB back after %lld s on battery: %d%% -> %d%% (%d -> %d mV)",
             (long long)((s_end->at_us - s_start->at_us) / 1000000), s_pct_start, s_pct_now,
             s_mv_start, s_mv_now);
    persist(true);
}

/* Brings a running measurement up to now; returns its end. */
static const snap_t *measure_end(void)
{
    if (!s_running) {
        return s_end;
    }
    snap(s_now);
    accrue(s_now->at_us);
    return s_now;
}

static int per_mille(int64_t part, int64_t whole)
{
    return whole > 0 && part > 0 ? (int)(part * 1000 / whole) : 0;
}

static void summarize(const snap_t *end, muse_battery_t *out)
{
    memset(out, 0, sizeof(*out));
    out->slept_pm = out->busy_pm = -1;
    out->pct_start = out->pct_now = -1;
    if (!s_started) {
        return;
    }
    int64_t span = end->at_us - s_start->at_us;
    out->started = true;
    out->running = s_running;
    out->secs = span / 1000000;
    out->pct_start = s_pct_start;
    out->pct_now = s_pct_now;
    out->mv_start = s_mv_start;
    out->mv_now = s_mv_now;
    out->screen_off_pm = per_mille(s_off_us, span);
    out->resting_pm = per_mille(s_rest_us, span);
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    out->slept_pm = per_mille(end->slept_us - s_start->slept_us, span);
    out->sleeps = end->sleeps - s_start->sleeps;
#endif
#if CONFIG_PM_PROFILING
    int64_t pm_span = end->pm.at_us - s_start->pm.at_us;
    if (!s_start->pm.at_us || !end->pm.at_us || pm_span <= 0) {
        return;
    }
    out->busy_pm = per_mille(end->pm.mode_us[MODE_CPU_MAX] - s_start->pm.mode_us[MODE_CPU_MAX], pm_span);
    /* The two held longest, past the CPUs' own (rtos0, rtos1: a core running a task). */
    int n = lock_deltas(&end->pm);
    const lock_delta_t *top[2] = { 0 };
    for (int i = 0; i < n; i++) {
        const lock_delta_t *d = &s_deltas[i];
        if (!strncmp(d->lock->name, "rtos", 4) || per_mille(d->held_us, pm_span) < 1) {
            continue;
        }
        if (!top[0] || d->held_us > top[0]->held_us) {
            top[1] = top[0];
            top[0] = d;
        } else if (!top[1] || d->held_us > top[1]->held_us) {
            top[1] = d;
        }
    }
    size_t len = 0;
    for (int i = 0; i < 2 && top[i]; i++) {
        int pm = per_mille(top[i]->held_us, pm_span);
        len += snprintf(out->awake + len, sizeof(out->awake) - len, "%s%s %d.%d%%", i ? ", " : "",
                        top[i]->lock->name, pm / 10, pm % 10);
        len = len < sizeof(out->awake) ? len : sizeof(out->awake) - 1;
    }
    if (!len) {
        strlcpy(out->awake, "nothing", sizeof(out->awake));
    }
#endif
}

static void *alloc(size_t size)
{
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    p = p ? p : calloc(1, size);
    configASSERT(p);
    return p;
}

static uint32_t checksum(const char *p, size_t n)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    while (n--) {
        h = (h ^ (uint8_t)*p++) * 16777619u;
    }
    return h;
}

void muse_battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_nvs = 0;
    } else {
        nvs_get_u16(s_nvs, "boots", &s_boots);
    }
    size_t n = SAVED_MAX;
    if (s_saved.magic == SAVED_MAGIC && s_saved.len < SAVED_MAX && s_saved.sum == checksum(s_saved.json, s_saved.len)) {
        s_prev = alloc(s_saved.len + 1);
        memcpy(s_prev, s_saved.json, s_saved.len);
    } else if (s_nvs && nvs_get_blob(s_nvs, "run", NULL, &n) == ESP_OK && n < SAVED_MAX) {
        s_prev = alloc(n + 1);
        nvs_get_blob(s_nvs, "run", s_prev, &n);
        s_prev[n] = '\0';
    }
    if (s_prev) {
        /* Also unasked: a board whose USB is a UART bridge (the Watcher) takes no commands. */
        printf("@power.saved %s\n", s_prev);
    }
    s_start = alloc(sizeof(snap_t));
    s_end = alloc(sizeof(snap_t));
    s_now = alloc(sizeof(snap_t));
#if CONFIG_PM_PROFILING
    s_dump = alloc(DUMP_MAX);
#endif
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = on_wake };
    esp_err_t err = esp_pm_light_sleep_register_cbs(&cbs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "light-sleep callback: %s", esp_err_to_name(err));
    }
#endif
}

void muse_battery_note_power(const muse_power_t *p, bool on_battery)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_power = *p;
    if (p->usb && !s_seen_usb) {
        s_seen_usb = true;
        if (s_boots) {
            save_boots(0);   /* charged: a later power-on from battery counts from 1 */
        }
    }
    if (on_battery && !s_running) {
        /* Once a boot, and only one that began on battery: a run started
         * after unplugging, or by power.reset, isn't a boot. */
        if (!s_seen_usb && !s_counted) {
            s_counted = true;
            save_boots(s_boots + 1);
        }
        start();
    } else if (!on_battery && s_running) {
        stop();
    }
    if (s_running) {
        s_pct_now = p->battery_pct;
        s_mv_now = p->battery_mv;
        if (esp_timer_get_time() - s_saved_us >= SAVE_MS * 1000LL) {
            persist(true);
        }
    }
    xSemaphoreGive(s_lock);
}

void muse_battery_note_state(bool screen_off, bool resting)
{
    if (screen_off == s_screen_off && resting == s_resting) {
        return;   /* only the input task changes them */
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    accrue(esp_timer_get_time());
    s_screen_off = screen_off;
    s_resting = resting;
    xSemaphoreGive(s_lock);
}

void muse_battery_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_prev);
    s_prev = NULL;
    s_saved.magic = 0;
    if (s_nvs) {
        nvs_erase_key(s_nvs, "run");
        nvs_commit(s_nvs);
    }
    if (s_running) {
        start();
    } else {
        s_started = false;
    }
    xSemaphoreGive(s_lock);
}

void muse_battery_read(muse_battery_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    summarize(s_started ? measure_end() : NULL, out);
    xSemaphoreGive(s_lock);
}

bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h)
{
    int used = b->pct_start - b->pct_now;
    if (!b->started || used <= 0 || b->secs < 600) {
        return false;
    }
    *rate10 = (int)(used * 36000LL / b->secs);
    *full_h = (int)(b->secs / (used * 36LL));
    return true;
}

static void add(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    *len = n < 0 ? *len : (*len + n < cap ? *len + n : cap - 1);
}

/* A per-mille figure as a JSON percentage. */
static const char *pct(char out[16], int pm)
{
    if (pm < 0) {
        return "null";
    }
    snprintf(out, 16, "%d.%d", pm / 10, pm % 10);
    return out;
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "other";
    }
}

/* With s_lock held. */
static int json(char *buf, size_t cap)
{
    char a[16], b[16], c[16], d[16];
    size_t len = 0;
    const snap_t *end = s_started ? measure_end() : NULL;
    muse_battery_t m;
    summarize(end, &m);
    /* How this boot began and how long ago: a panic or brownout on battery shows here. */
    add(buf, cap, &len,
        "{\"boot\":\"%s\",\"uptime\":%lld,\"battery_boots\":%u,"
        "\"started\":%s,\"running\":%s,\"secs\":%lld,\"battery_pct\":[%d,%d],\"battery_mv\":[%d,%d],"
        "\"screen_off\":%s,\"resting\":%s,\"slept\":%s,\"sleeps\":%lu,\"busy\":%s",
        reset_name(esp_reset_reason()), (long long)(esp_timer_get_time() / 1000000), s_boots,
        m.started ? "true" : "false", m.running ? "true" : "false", (long long)m.secs, m.pct_start, m.pct_now,
        m.mv_start, m.mv_now, pct(a, m.screen_off_pm), pct(b, m.resting_pm), pct(c, m.slept_pm),
        (unsigned long)m.sleeps, pct(d, m.busy_pm));
#if CONFIG_PM_PROFILING
    int64_t pm_span = end ? end->pm.at_us - s_start->pm.at_us : 0;
    if (end && s_start->pm.at_us && end->pm.at_us && pm_span > 0) {
        add(buf, cap, &len, ",\"modes\":{");
        for (int i = 0; i < MODE_COUNT; i++) {
            add(buf, cap, &len, "%s\"%s\":%s", i ? "," : "", MODE_NAMES[i],
                pct(a, per_mille(end->pm.mode_us[i] - s_start->pm.mode_us[i], pm_span)));
        }
        add(buf, cap, &len, "},\"sleep_rejects\":%ld,\"locks\":[", end->pm.rejects - s_start->pm.rejects);
        int n = lock_deltas(&end->pm);
        for (int i = 0; i < n; i++) {
            const lock_delta_t *l = &s_deltas[i];
            add(buf, cap, &len, "%s{\"name\":\"%s\",\"type\":\"%s\",\"held\":%s,\"taken\":%d}", i ? "," : "",
                l->lock->name, l->lock->type, pct(a, per_mille(l->held_us, pm_span)), l->taken);
        }
        add(buf, cap, &len, "]");
    }
#endif
    add(buf, cap, &len, "}");
    return (int)len;
}

/* With s_lock held. Written in place: a reset partway leaves a bad checksum. */
static void persist(bool flash)
{
    s_saved.len = json(s_saved.json, sizeof(s_saved.json));
    s_saved.sum = checksum(s_saved.json, s_saved.len);
    s_saved.magic = SAVED_MAGIC;
    s_saved_us = esp_timer_get_time();
    if (flash && s_nvs && s_saved.len < SAVED_MAX) {
        nvs_set_blob(s_nvs, "run", s_saved.json, s_saved.len);
        nvs_commit(s_nvs);
    }
}

int muse_battery_json(char *buf, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int len = json(buf, cap);
    xSemaphoreGive(s_lock);
    return len;
}

const char *muse_battery_saved_json(void)
{
    return s_prev;
}
