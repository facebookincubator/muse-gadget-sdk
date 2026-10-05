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

/*
 * Muse as a pet (muse_pet.h). The model runs on an esp_timer every 10 s: its
 * needs fall per hour of real time (the clock once it's set, so hours off
 * count, up to three days), poops follow meals, neglect builds germs into
 * sickness, and sickness and unmet needs wear its health down to fainting.
 * Nothing is random, and nothing dies: medicine and care bring it back.
 * The page is the avatar with its needs around it, care along the bottom
 * (foods and a shower to drag, buttons for the rest), and little pixel-art
 * animations for what's done.
 */
#include "muse_pet.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "nvs.h"

#include "muse_audio.h"
#include "muse_board.h"
#if CONFIG_MUSE_HATCH
#include "muse_chat.h"
#endif
#include "muse_hw.h"
#include "muse_mem.h"
#include "muse_settings.h"
#include "muse_state.h"
#include "muse_voice.h"

static const char *TAG = "muse_pet";

#define TICK_US (10 * 1000000LL)
#define SAVE_US (5 * 60 * 1000000LL)

/* ---- The model (host-tested: tests/test_muse_apps.py) ---- */

#define CATCH_UP_MAX_H 72.0f
#define TIME_VALID 1735689600           /* 2025-01-01: the clock is set */

#define MAGIC 0x50455402u                /* "PET", version 2 */
#define POOPS_MAX 4

typedef struct {
    uint32_t magic;
    float food, clean, fun, energy, health;
    float poop_in_h;      /* until the next poop, after a meal; 0 none due */
    float germs;          /* neglect, building to sickness at 4 */
    float age_h;
    uint8_t poops, sick, asleep, fainted;
    uint32_t cared;
    int64_t updated;      /* the clock at the last step; 0 while it wasn't set */
    char name[16];
} pet_t;

/* What changed in a step, for events. */
enum {
    EV_POOP = 1, EV_SICK = 2, EV_FAINTED = 4, EV_WOKE = 8, EV_FELL_ASLEEP = 16,
};

static float clamp100(float v)
{
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void pet_new(pet_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic = MAGIC;
    /* A little hungry, grubby, bored and tired, with a poop: there's care to
     * give from the start. */
    p->food = 45;
    p->clean = 55;
    p->fun = 40;
    p->energy = 60;
    p->health = 100;
    p->poops = 1;
    strcpy(p->name, "MUSE");
}

/* Hours pass: needs fall, a meal's poop comes, neglect makes it sick. */
static unsigned pet_advance(pet_t *p, float hours)
{
    unsigned ev = 0;
    while (hours > 0) {
        float h = hours > 0.1f ? 0.1f : hours;
        hours -= h;
        if (p->asleep) {
            p->energy += 18 * h;
            if (p->energy >= 100) {
                p->asleep = 0;
                ev |= EV_WOKE;
            }
        } else {
            p->energy -= 5 * h;
        }
        p->food -= (p->asleep ? 1.5f : 4) * h;
        p->fun -= (p->asleep ? 0.5f : 3.5f) * h;
        p->clean -= (3 + 1.5f * p->poops) * h;
        if (p->poop_in_h > 0 && (p->poop_in_h -= h) <= 0) {
            p->poop_in_h = 0;
            if (p->poops < POOPS_MAX) {
                p->poops++;
                ev |= EV_POOP;
            }
        }
        int neglect = (p->food < 15) + (p->clean < 15) + (p->poops >= 2);
        p->germs += neglect ? h * neglect : -h * 0.5f;
        if (p->germs < 0) p->germs = 0;
        if (!p->sick && p->germs >= 4) {
            p->sick = 1;
            ev |= EV_SICK;
        }
        int low = (p->food < 15) + (p->fun < 15) + (p->clean < 15) + (p->energy < 10);
        p->health -= (p->sick ? 2 : 0) * h + 1.0f * low * h;
        if (!p->sick && !low && p->food > 40 && p->clean > 40) p->health += 3 * h;
        p->food = clamp100(p->food);
        p->clean = clamp100(p->clean);
        p->fun = clamp100(p->fun);
        p->energy = clamp100(p->energy);
        p->health = clamp100(p->health);
        if (p->health <= 0 && !p->fainted) {
            p->fainted = 1;
            ev |= EV_FAINTED;
        }
        if (!p->asleep && p->energy <= 3) {
            p->asleep = 1;
            ev |= EV_FELL_ASLEEP;
        }
        p->age_h += h;
    }
    return ev;
}

/* Care; NULL, or why not. */
static const char *pet_do(pet_t *p, muse_pet_action_t a)
{
    switch (a) {
    case MUSE_PET_FEED:
    case MUSE_PET_SNACK:
        if (p->asleep) return "ASLEEP";
        if (a == MUSE_PET_FEED && p->food > 92) {
            p->fun = clamp100(p->fun - 3);
            return "FULL";
        }
        p->food = clamp100(p->food + (a == MUSE_PET_FEED ? 30 : 8));
        p->fun = clamp100(p->fun + (a == MUSE_PET_FEED ? 3 : 12));
        if (a == MUSE_PET_SNACK) p->health = clamp100(p->health - 1);
        if (p->poop_in_h <= 0) p->poop_in_h = 3.0f;
        break;
    case MUSE_PET_WASH:
        if (p->asleep) return "ASLEEP";
        p->clean = 100;
        p->poops = 0;
        p->germs = p->germs > 1.5f ? p->germs - 1.5f : 0;
        p->fun = clamp100(p->fun - 4);
        break;
    case MUSE_PET_CLEAN:
        if (!p->poops) return "ALL CLEAN";
        p->poops--;
        p->clean = clamp100(p->clean + 5);
        break;
    case MUSE_PET_PLAY:
        if (p->asleep) return "ASLEEP";
        if (p->sick || p->fainted) return "TOO POORLY";
        if (p->energy < 12) return "TOO TIRED";
        p->fun = clamp100(p->fun + 22);
        p->energy = clamp100(p->energy - 10);
        p->food = clamp100(p->food - 4);
        break;
    case MUSE_PET_SLEEP:
        if (p->asleep) return "ASLEEP";
        p->asleep = 1;
        break;
    case MUSE_PET_WAKE:
        if (!p->asleep) return "AWAKE";
        p->asleep = 0;
        if (p->energy < 60) p->fun = clamp100(p->fun - 5);   /* grumpy */
        break;
    case MUSE_PET_HEAL:
        if (!p->sick && !p->fainted && p->health > 90) return "NOT SICK";
        p->sick = p->fainted = 0;
        p->germs = 0;
        p->health = clamp100((p->health < 40 ? 40 : p->health) + 15);
        p->fun = clamp100(p->fun - 6);   /* yuck */
        break;
    case MUSE_PET_PET:
        p->fun = clamp100(p->fun + (p->asleep ? 2 : 5));
        break;
    default:
        return "?";
    }
    p->cared++;
    return NULL;
}

static const char *pet_need(const pet_t *p)
{
    if (p->fainted) return "FAINTED!";
    if (p->sick) return "FEELING SICK";
    if (p->asleep) return "ZZZ...";
    if (p->food < 25) return "HUNGRY";
    if (p->poops >= 2) return "CLEAN UP!";
    if (p->clean < 25) return "NEEDS A BATH";
    if (p->energy < 20) return "SLEEPY";
    if (p->fun < 25) return "BORED";
    return "";
}

/* The hours it was off, caught up once the clock is set: from its last step
 * to this boot, up to three days; none if the clock wasn't set then. */
static float pet_off_hours(const pet_t *p, int64_t now, int64_t uptime_s)
{
    if (p->updated < TIME_VALID) return 0;
    float off = (float)(now - uptime_s - p->updated) / 3600.0f;
    return off <= 0 ? 0 : off > CATCH_UP_MAX_H ? CATCH_UP_MAX_H : off;
}

/* The Muse's name for a pet with none of its own, kept to what its page's
 * font draws: capitals, digits, single spaces and simple punctuation, other
 * bytes (UTF-8) dropped, cut at size - 1. False if no letter or digit is left. */
static bool pet_muse_name(const char *muse, char *out, size_t size)
{
    size_t n = 0;
    bool any = false;
    for (; *muse && n + 1 < size; muse++) {
        char c = *muse;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum && !strchr(" .,'!?&-", c)) continue;
        if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
        out[n++] = c;
        any |= alnum;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return any;
}

/* A new pet, its clock starting now: none of the old one's hours off. */
static void pet_restart(pet_t *p, int64_t now)
{
    pet_new(p);
    p->updated = now >= TIME_VALID ? now : 0;
}

/* ---- Model end ---- */

static SemaphoreHandle_t s_lock;
static pet_t s_pet;
static bool s_dirty;              /* changed since saved */
static int64_t s_saved_us, s_mono_us;
/* Saves run on the esp_timer task: a script's task, its stack in PSRAM,
 * mustn't write flash. */
static esp_timer_handle_t s_save_timer;

static const char *const ACTIONS[MUSE_PET_ACTIONS] = {
    "feed", "snack", "wash", "clean", "play", "sleep", "wake", "heal", "pet",
};

const char *muse_pet_action_name(muse_pet_action_t a)
{
    return a < MUSE_PET_ACTIONS ? ACTIONS[a] : "?";
}

bool muse_pet_action_by_name(const char *name, muse_pet_action_t *out)
{
    for (int i = 0; name && i < MUSE_PET_ACTIONS; i++) {
        if (!strcmp(name, ACTIONS[i])) {
            *out = (muse_pet_action_t)i;
            return true;
        }
    }
    return false;
}

/* On the esp_timer task, with s_lock held. */
static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open("muse_pet", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, "state", &s_pet, sizeof(s_pet)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
    s_saved_us = esp_timer_get_time();
}

static void save_now(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) save_locked();
    xSemaphoreGive(s_lock);
}

/* Changed: saved in a moment, whichever task changed it. */
static void save_soon(void)
{
    s_dirty = true;
    if (s_save_timer && !esp_timer_is_active(s_save_timer)) esp_timer_start_once(s_save_timer, 500 * 1000);
}

static void post_events(unsigned ev)
{
    if (ev & EV_POOP) muse_hw_pet("poop", s_pet.poops);
    if (ev & EV_SICK) muse_hw_pet("sick", 1);
    if (ev & EV_FAINTED) muse_hw_pet("fainted", 1);
    if (ev & EV_WOKE) muse_hw_pet("woke", 1);
    if (ev & EV_FELL_ASLEEP) muse_hw_pet("fell_asleep", 1);
}

/* Needs as they cross into want, once each until met again. */
static void post_needs(const pet_t *before, const pet_t *after)
{
    static const struct {
        const char *name;
        size_t off;
        float below;
    } NEEDS[] = {
        { "hungry", offsetof(pet_t, food), 25 },
        { "dirty", offsetof(pet_t, clean), 25 },
        { "bored", offsetof(pet_t, fun), 25 },
        { "tired", offsetof(pet_t, energy), 20 },
    };
    for (size_t i = 0; i < sizeof(NEEDS) / sizeof(NEEDS[0]); i++) {
        float b = *(const float *)((const char *)before + NEEDS[i].off);
        float a = *(const float *)((const char *)after + NEEDS[i].off);
        if (b >= NEEDS[i].below && a < NEEDS[i].below) muse_hw_pet(NEEDS[i].name, (int)a);
    }
}

static bool s_caught_up;   /* the hours it was off, counted this boot */

static void tick(void *arg)
{
    (void)arg;
    int64_t mono = esp_timer_get_time();
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* On, the monotonic timer counts: the wall clock can step (NTP, a script
     * setting it). Off, only the clock can tell: once it's set, the time from
     * the last save to this boot is caught up, once. */
    float hours = (float)(mono - s_mono_us) / 3.6e9f;
    s_mono_us = mono;
    if (!s_caught_up && now >= TIME_VALID) {
        s_caught_up = true;
        hours += pet_off_hours(&s_pet, now, mono / 1000000);
    }
    if (now >= TIME_VALID) s_pet.updated = now;
    pet_t before = s_pet;
    unsigned ev = 0;
    if (hours > 0) {
        ev = pet_advance(&s_pet, hours);
        s_dirty = true;
    }
    if (s_dirty && (ev || mono - s_saved_us >= SAVE_US)) save_locked();
    pet_t after = s_pet;
    xSemaphoreGive(s_lock);
    post_events(ev);
    post_needs(&before, &after);
}

void muse_pet_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t len = sizeof(s_pet);
    bool ok = nvs_open("muse_pet", NVS_READONLY, &h) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(h, "state", &s_pet, &len) == ESP_OK && len == sizeof(s_pet) && s_pet.magic == MAGIC;
        nvs_close(h);
    }
    if (!ok) {
        pet_new(&s_pet);
        ESP_LOGI(TAG, "a new pet");
    }
    s_pet.name[sizeof(s_pet.name) - 1] = '\0';
    s_mono_us = esp_timer_get_time();
    s_saved_us = s_mono_us;
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t a = { .callback = tick, .name = "muse_pet" };
    if (esp_timer_create(&a, &timer) == ESP_OK) esp_timer_start_periodic(timer, TICK_US);
    const esp_timer_create_args_t save = { .callback = save_now, .name = "muse_pet_save" };
    esp_timer_create(&save, &s_save_timer);
}

bool muse_pet_ready(void)
{
    return s_lock != NULL;
}

void muse_pet_state(muse_pet_state_t *out)
{
    if (!s_lock) {
        *out = (muse_pet_state_t){ 0 };
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = (muse_pet_state_t){
        .food = s_pet.food, .clean = s_pet.clean, .fun = s_pet.fun, .energy = s_pet.energy,
        .health = s_pet.health, .poops = s_pet.poops, .sick = s_pet.sick, .asleep = s_pet.asleep,
        .fainted = s_pet.fainted, .age_days = s_pet.age_h / 24.0f, .cared = (int)s_pet.cared,
    };
    strlcpy(out->name, s_pet.name, sizeof(out->name));
    xSemaphoreGive(s_lock);
    out->named = strcmp(out->name, "MUSE") != 0;
#if CONFIG_MUSE_HATCH
    /* Never named: it goes by the Muse's own name, as its page can draw it. */
    char muse[32], shown[sizeof(out->name)];
    if (!out->named && muse_chat_muse_name(muse, sizeof(muse)) && pet_muse_name(muse, shown, sizeof(shown))) {
        strcpy(out->name, shown);
    }
#else
    (void)pet_muse_name;   /* no Muse identity to go by */
#endif
}

const char *muse_pet_need(void)
{
    if (!s_lock) return "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *n = pet_need(&s_pet);
    xSemaphoreGive(s_lock);
    return n;
}

void muse_pet_reset(void)
{
    if (!s_lock) return;
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Not the old pet's last save, which would give it that pet's hours off
     * once the clock is set. */
    pet_restart(&s_pet, now);
    s_mono_us = esp_timer_get_time();
    save_soon();
    xSemaphoreGive(s_lock);
    muse_hw_pet("reset", 0);
}

void muse_pet_set_name(const char *name)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_pet.name, name, sizeof(s_pet.name));
    save_soon();
    xSemaphoreGive(s_lock);
}

/* ---- Sounds: little tunes for what's done ---- */

static void tune(const int16_t *hz_ms, int n)
{
    if (!muse_settings_speaker_on()) return;
    size_t frames = 0;
    for (int i = 0; i < n; i++) frames += (size_t)hz_ms[2 * i + 1] * MUSE_AUDIO_RATE / 1000;
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!pcm) return;
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        int len = hz_ms[2 * i + 1] * MUSE_AUDIO_RATE / 1000, fade = MUSE_AUDIO_RATE / 250;
        for (int j = 0; j < len; j++, k++) {
            float env = j < fade ? (float)j / fade : len - j < fade ? (float)(len - j) / fade : 1;
            /* About speech's loudness, so the volume (the codec's, the wheel's) suits both. */
            pcm[k] = hz_ms[2 * i] ? (int16_t)(sinf(6.2831853f * hz_ms[2 * i] * j / MUSE_AUDIO_RATE) * env * 4500) : 0;
        }
    }
    if (!muse_voice_request_play(pcm, frames, NULL, NULL)) free(pcm);
}

static void sound_for(muse_pet_action_t a, bool refused)
{
    static const int16_t NOPE[] = { 330, 90, 0, 30, 262, 140 };
    static const int16_t NOM[] = { 523, 70, 0, 40, 659, 70, 0, 40, 784, 110 };
    static const int16_t BUBBLES[] = { 988, 50, 1175, 50, 1319, 50, 1568, 90 };
    static const int16_t PLAY[] = { 659, 80, 784, 80, 988, 80, 1319, 160 };
    static const int16_t NIGHT[] = { 784, 120, 659, 120, 523, 200 };
    static const int16_t MORNING[] = { 523, 100, 659, 100, 784, 100, 1047, 180 };
    static const int16_t HEAL[] = { 440, 70, 554, 70, 659, 70, 880, 180 };
    static const int16_t PURR[] = { 1047, 60, 1319, 90 };
    static const int16_t POP[] = { 784, 40, 1175, 60 };
    if (refused) {
        tune(NOPE, 3);
        return;
    }
    switch (a) {
    case MUSE_PET_FEED:
    case MUSE_PET_SNACK: tune(NOM, 5); break;
    case MUSE_PET_WASH: tune(BUBBLES, 4); break;
    case MUSE_PET_CLEAN: tune(POP, 2); break;
    case MUSE_PET_PLAY: tune(PLAY, 4); break;
    case MUSE_PET_SLEEP: tune(NIGHT, 3); break;
    case MUSE_PET_WAKE: tune(MORNING, 4); break;
    case MUSE_PET_HEAL: tune(HEAL, 4); break;
    case MUSE_PET_PET: tune(PURR, 2); break;
    default: break;
    }
}

static void animate(muse_pet_action_t a);

/* What its page says a moment after care, from its buttons, Muse or a script:
 * what was done, or why not. */
static const char *const CAPTIONS[MUSE_PET_ACTIONS] = {
    "YUM!", "A TREAT!", "SPLASH!", "ALL TIDY", "WHEE!", "NIGHT NIGHT", "MORNING!", "MEDICINE", "PURR...",
};
static char s_say[24];
static int64_t s_say_until_us;
static bool s_say_no;

const char *muse_pet_care(muse_pet_action_t a)
{
    if (!s_lock) return "NOT READY";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pet_t before = s_pet;
    const char *why = pet_do(&s_pet, a);
    pet_t after = s_pet;
    if (!why) save_soon();
    strlcpy(s_say, why ? why : CAPTIONS[a], sizeof(s_say));
    s_say_until_us = esp_timer_get_time() + 2000000;
    s_say_no = why != NULL;
    xSemaphoreGive(s_lock);
    sound_for(a, why != NULL);
    post_needs(&before, &after);   /* play can tire it, a bath bore it */
    if (!why) {
        if (a != MUSE_PET_SLEEP && a != MUSE_PET_WASH && a != MUSE_PET_HEAL) muse_state_make_happy();
        muse_state_poke();
        muse_hw_pet(muse_pet_action_name(a), 1);
        animate(a);
    }
    return why;
}

/* ---- Pixel art ---- */

typedef struct {
    const char *rows[12];
} icon_t;

enum {
    I_FOOD, I_BUBBLE, I_STAR, I_BOLT, I_HEART, I_HEART_OFF, I_PILL, I_MOON, I_SUN, I_BALL, I_POOP, I_PAD,
    I_KIT, I_RICE, I_APPLE, I_CAKE, I_SHOWER, I_DROP, I_COUNT,
};

static const icon_t ICONS[I_COUNT] = {
    [I_FOOD] = { { "........kk..", ".......kwwk.", "......kwwk..", ".....kwwk...", "..kkkkkk....", ".kmmmmmmk...",
                   "kmmmmwmmmk..", "kmmmmmmmmk..", "kmmmmmmmk...", ".kmmmmmk....", "..kkkkk.....", "............" } },
    [I_BUBBLE] = { { "...kkkkk....", "..kcccccck..", ".kcwwccccck.", "kcwwcccccck.", "kcwcccccccck", "kcccccccccck",
                     "kcccccccccck", "kcccccccccck", ".kccccccccck", "..kcccccck..", "...kkkkkk...", "............" } },
    [I_STAR] = { { ".....kk.....", ".....kyk....", "....kyyk....", "kkkkyyyykkkk", "kyyyyyyyyyyk", ".kyyyyyyyyk.",
                   "..kyyyyyyk..", "..kyyyyyyk..", ".kyyykkyyyk.", ".kyyk..kyyk.", ".kkk....kkk.", "............" } },
    [I_BOLT] = { { "......kkkk..", ".....kyyyk..", "....kyyyk...", "...kyyyk....", "..kyyyykkkk.", ".kyyyyyyyyk.",
                   ".kkkkyyyyk..", "....kyyyk...", "...kyyyk....", "..kyyk......", "..kyk.......", "..kk........" } },
    [I_HEART] = { { "............", ".kkk...kkk..", "krrrk.krrrk.", "krwrrkrrrrk.", "krrrrrrrrrk.", "krrrrrrrrrk.",
                    ".krrrrrrrk..", "..krrrrrk...", "...krrrk....", "....krk.....", ".....k......", "............" } },
    [I_HEART_OFF] = { { "............", ".kkk...kkk..", "kxxxk.kxxxk.", "kxxxxkxxxxk.", "kxxxxxxxxxk.", "kxxxxxxxxxk.",
                        ".kxxxxxxxk..", "..kxxxxxk...", "...kxxxk....", "....kxk.....", ".....k......", "............" } },
    [I_PILL] = { { "............", "....kkkk....", "...krrrrk...", "..krwrrrrk..", "..krrrrrrk..", "..kkkkkkkk..",
                   "..kwwwwwwk..", "..kwwwwwwk..", "...kwwwwk...", "....kkkk....", "............", "............" } },
    [I_MOON] = { { "....kkkk....", "..kkyyyk....", ".kyyyk......", ".kyyk.......", "kyyyk.......", "kyyyk.......",
                   "kyyyyk......", ".kyyyyk..k..", ".kyyyyykkyk.", "..kkyyyyyk..", "....kkkkk...", "............" } },
    [I_SUN] = { { ".....yy.....", "..y..yy..y..", "...y.kk.y...", "....kyyk....", "yy.kyyyyk.yy", "yy.kyyyyk.yy",
                  "....kyyk....", "...y.kk.y...", "..y..yy..y..", ".....yy.....", "............", "............" } },
    [I_BALL] = { { "...kkkkk....", "..krrrrrk...", ".krrwwrrrk..", "krrwwrrrrrk.", "kwwwwwwwwwk.", "kbbbbbbbbbk.",
                   "kbbbbbbbbbk.", ".kbbbbbbbk..", "..kbbbbbk...", "...kkkkk....", "............", "............" } },
    [I_POOP] = { { ".....kk.....", "....knnk....", "...knnnnk...", "...kkkkkk...", "..knnnnnnk..", "..kwknnkwk..",
                   ".knnnnnnnnk.", ".kkkkkkkkkk.", "knnnnnnnnnnk", "knnnnnnnnnnk", ".kkkkkkkkkk.", "............" } },
    [I_PAD] = { { "............", "............", "..kkkkkkkk..", ".kppppppppk.", "kpwppppppypk", "kwwwppppcprk",
                  "kpwppppppgpk", "kppppkkppppk", "kpppk..kpppk", ".kkk....kkk.", "............", "............" } },
    [I_KIT] = { { "............", "....kkkk....", "...k....k...", ".kkkkkkkkkk.", "kwwwwrrwwwwk", "kwwwwrrwwwwk",
                  "kwwrrrrrrwwk", "kwwrrrrrrwwk", "kwwwwrrwwwwk", "kwwwwrrwwwwk", ".kkkkkkkkkk.", "............" } },
    [I_RICE] = { { ".....kk.....", "....kwwk....", "...kwwwwk...", "...kwwwwk...", "..kwwwwwwk..", "..kwwwwwwk..",
                   ".kwwwxxwwwk.", ".kwwxxxxwwk.", "kwwwxxxxwwwk", "kwwwxxxxwwwk", ".kkkkkkkkkk.", "............" } },
    [I_APPLE] = { { ".....kk.....", "....kgk.....", "..kkkgkkk...", ".krrrrrrrk..", "krrwrrrrrrk.", "krwrrrrrrrk.",
                    "krrrrrrrrrk.", "krrrrrrrrrk.", ".krrrrrrrk..", "..krrkrrk...", "...kk.kk....", "............" } },
    [I_CAKE] = { { ".....kk.....", "....krrk....", "....krrk....", "..kkkkkkkk..", ".kiiiwiiiik.", "kiiwiiiiiiik",
                   "kiiiiiiiwiik", ".kkkkkkkkkk.", ".kmnmnmnmnk.", "..kmnmnmnk..", "..kkkkkkkk..", "............" } },
    [I_SHOWER] = { { "........kkk.", ".......kewek", "......keeek.", ".....keeek..", "..kkkeeek...", ".keeeeeeek..",
                     "keweeeeeeek.", "keeeeeeeeek.", "kkkkkkkkkkk.", ".kbbbbbbbk..", "..kkkkkkk...", "............" } },
    [I_DROP] = { { ".....kk.....", "....kcck....", "....kcck....", "...kcccck...", "..kcwcccck..", "..kcwcccck..",
                   ".kcwcccccck.", ".kccccccbck.", ".kccccccbck.", "..kccccbck..", "...kkkkkk...", "............" } },
};

static uint32_t ink(char c)
{
    switch (c) {
    case 'k': return 0x2a1f33;
    case 'w': return 0xffffff;
    case 'r': return 0xff5a6e;
    case 'y': return 0xffd24a;
    case 'c': return 0x9fe3ff;
    case 'b': return 0x4a90ff;
    case 'm': return 0xd98a4e;
    case 'n': return 0x8a5a3c;
    case 'g': return 0x5ec46a;
    case 'x': return 0x3a3358;
    case 'p': return 0xa77dff;
    case 'e': return 0xc9c4dc;
    case 'i': return 0xff9ad5;
    default: return 0;
    }
}

/* An icon drawn at `scale` into a new ARGB8888 image, in PSRAM. */
static lv_image_dsc_t *icon_image(int i, int scale)
{
    int n = 12 * scale;
    lv_image_dsc_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    uint32_t *px = heap_caps_calloc((size_t)n * n, 4, MALLOC_CAP_SPIRAM);
    if (!d || !px) {
        free(d);
        free(px);
        return NULL;
    }
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12 && ICONS[i].rows[y][x]; x++) {
            char c = ICONS[i].rows[y][x];
            if (c == '.') continue;
            uint32_t argb = 0xff000000u | ink(c);
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) px[(y * scale + dy) * n + x * scale + dx] = argb;
            }
        }
    }
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = n;
    d->header.h = n;
    d->header.stride = n * 4;
    d->data_size = (size_t)n * n * 4;
    d->data = (const uint8_t *)px;
    return d;
}

/* ---- The page ---- */

#define GAUGES 4
#define SLOTS 5
#define PARTICLES 16
#define HEARTS 5

static lv_obj_t *s_tile, *s_avatar, *s_name, *s_need, *s_night, *s_sleep_icon;
static lv_obj_t *s_gauge[GAUGES], *s_slot[SLOTS];
static lv_obj_t *s_heart[HEARTS], *s_poop[POOPS_MAX], *s_particle[PARTICLES];
static lv_image_dsc_t *s_img[I_COUNT], *s_small[I_COUNT], *s_big[I_COUNT];
static int s_shown_gauge[GAUGES] = { -1, -1, -1, -1 };
static int s_avatar_y;
static int s_next_particle;

/* Up the sides, leaving the bottom to care. */
static const struct {
    int icon;
    uint32_t color;
    int x, y;
    const char *name;
} GAUGE_AT[GAUGES] = {
    { I_FOOD, 0xff9f3a, -128, -112, "FOOD" },
    { I_BUBBLE, 0x64d2ff, -165, -45, "CLEAN" },
    { I_STAR, 0xffd24a, 128, -112, "FUN" },
    { I_BOLT, 0x5ed36a, 165, -45, "ENERGY" },
};

/* Round the bottom, left to right: food to drag to it, buttons, and the
 * shower to drag. */
static const struct {
    muse_pet_action_t action;
    int icon;
    bool drag;
    float angle;    /* degrees, 90 is straight down */
} SLOT_AT[SLOTS] = {
    { MUSE_PET_FEED, I_FOOD, true, 150 },   /* the food slot: its food changes (FOODS) */
    { MUSE_PET_PLAY, I_PAD, false, 120 },
    { MUSE_PET_SLEEP, I_MOON, false, 90 },
    { MUSE_PET_HEAL, I_KIT, false, 60 },
    { MUSE_PET_WASH, I_SHOWER, true, 30 },
};
#define SLOT_PX 76        /* a fingertip: five fit round the edge */
#define SLOT_RING 162     /* px from the centre, inside the round edge */
#define SLOT_LOW 145      /* and no lower: clear of the page dots */

/* What the food slot holds: a different one after each is eaten. A meal
 * feeds it, the cupcake is a treat (a snack). */
static const struct {
    int icon;
    muse_pet_action_t action;
} FOODS[] = {
    { I_FOOD, MUSE_PET_FEED }, { I_RICE, MUSE_PET_FEED }, { I_APPLE, MUSE_PET_FEED }, { I_CAKE, MUSE_PET_SNACK },
};
static int s_food;

/* A slot's action: the food slot's is its food's. */
static muse_pet_action_t slot_action(int i)
{
    return SLOT_AT[i].action == MUSE_PET_FEED ? FOODS[s_food].action : SLOT_AT[i].action;
}
#define TOOL_PX 48        /* a scale-4 icon */

/* A tool dragged, or on its way back. */
static struct {
    int x, y;             /* its centre, from the page's */
    int grab_x, grab_y;   /* the finger, from its centre */
    bool held, moved;
} s_drag[SLOTS];
static lv_obj_t *s_under;         /* what the shower's water has been on, */
static uint32_t s_under_ms;       /* since */
static uint32_t s_spray_ms;
static uint32_t s_bath_ms;        /* no drops just after a bath: they'd take its bubbles' sprites */
static unsigned s_drops;
static bool s_bathed;             /* once a drag */

static void set_y(void *obj, int32_t v) { lv_obj_set_y(obj, v); }
static void set_x(void *obj, int32_t v) { lv_obj_set_x(obj, v); }
static void set_opa(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }
static void set_scale(void *obj, int32_t v) { lv_image_set_scale(obj, (uint32_t)v); }
static void hide_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static void anim_then(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, uint32_t delay,
                      lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_linear);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

static void anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms, uint32_t delay,
                 lv_anim_path_cb_t path, bool last)
{
    anim_then(o, cb, from, to, ms, delay, path, last ? hide_done : NULL);
}

/* A sprite from the pool, at (x, y) from the page's centre. */
static lv_obj_t *sprite(int icon, bool small, int x, int y)
{
    lv_obj_t *o = s_particle[s_next_particle];
    s_next_particle = (s_next_particle + 1) % PARTICLES;
    lv_anim_delete(o, NULL);
    lv_image_set_src(o, small ? s_small[icon] : s_img[icon]);
    lv_image_set_scale(o, 256);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    lv_obj_align(o, LV_ALIGN_CENTER, x, y);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(o);
    return o;
}

/* Only the slots use icons this big. */
static const void *big(int icon)
{
    if (!s_big[icon]) s_big[icon] = icon_image(icon, 4);
    return s_big[icon];
}

static void animate_locked(muse_pet_action_t a)
{
    int mouth = s_avatar_y + 14;
    switch (a) {
    case MUSE_PET_FEED:
    case MUSE_PET_SNACK: {
        lv_obj_t *o = sprite(a == MUSE_PET_FEED ? I_FOOD : I_CAKE, false, 0, mouth - 60);
        anim(o, set_y, mouth - 60, mouth, 500, 0, lv_anim_path_ease_in, false);
        anim(o, set_scale, 256, 0, 700, 500, NULL, true);
        break;
    }
    case MUSE_PET_WASH:
        for (int i = 0; i < 8; i++) {
            int x = -70 + (i * 37) % 140;
            lv_obj_t *o = sprite(I_BUBBLE, i % 2, x, 70);
            anim(o, set_y, 70, -110, 1400, i * 120, lv_anim_path_ease_out, false);
            anim(o, set_opa, 255, 0, 1400, i * 120, NULL, true);
        }
        break;
    case MUSE_PET_PLAY: {
        lv_obj_t *o = sprite(I_BALL, false, -120, 40);
        anim(o, set_x, -120, 120, 1200, 0, NULL, false);
        anim(o, set_y, -40, 40, 400, 0, lv_anim_path_bounce, false);
        anim(o, set_opa, 255, 0, 300, 1100, NULL, true);
        break;
    }
    case MUSE_PET_HEAL: {
        lv_obj_t *o = sprite(I_PILL, false, 0, mouth - 90);
        anim(o, set_y, mouth - 90, mouth, 700, 0, lv_anim_path_ease_in, false);
        anim(o, set_opa, 255, 0, 300, 700, NULL, true);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *s = sprite(I_STAR, true, -50 + i * 50, s_avatar_y - 40);
            anim(s, set_opa, 0, 255, 300, 700 + i * 120, NULL, false);
            anim(s, set_y, s_avatar_y - 40, s_avatar_y - 90, 900, 700 + i * 120, NULL, true);
        }
        break;
    }
    case MUSE_PET_PET:
        for (int i = 0; i < 3; i++) {
            lv_obj_t *o = sprite(I_HEART, true, -40 + i * 40, s_avatar_y - 30);
            anim(o, set_y, s_avatar_y - 30, s_avatar_y - 110, 1000, i * 150, lv_anim_path_ease_out, false);
            anim(o, set_opa, 255, 0, 1000, i * 150, NULL, true);
        }
        break;
    default:
        break;
    }
}

static void animate(muse_pet_action_t a)
{
    if (!s_tile || !muse_board || !muse_board->display_lock(100)) return;
    animate_locked(a);
    muse_board->display_unlock();
}

/* Care from the page; NULL, or why not. `show`: its usual animation, unless
 * the page shows it its own way. */
static const char *care(muse_pet_action_t a, bool show)
{
    /* The UI's own task: animate() would take the lock it already holds. */
    lv_obj_t *tile = s_tile;
    s_tile = NULL;
    const char *why = muse_pet_care(a);
    s_tile = tile;
    if (!why && show) animate_locked(a);
    return why;
}

/* A caption for a moment that isn't care: how to use a tool. */
static void say(const char *what)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_say, what, sizeof(s_say));
    s_say_until_us = esp_timer_get_time() + 2000000;
    s_say_no = false;
    xSemaphoreGive(s_lock);
}

static void on_button(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    muse_pet_action_t a = SLOT_AT[i].action;
    if (a == MUSE_PET_SLEEP) {
        muse_pet_state_t st;
        muse_pet_state(&st);
        if (st.asleep) a = MUSE_PET_WAKE;
    }
    care(a, true);
}

static void on_avatar(lv_event_t *e)
{
    (void)e;
    care(MUSE_PET_PET, true);
}

/* One poop gone, tapped or showered. */
static void clean_poop(lv_obj_t *o)
{
    if (lv_anim_get(o, NULL)) return;   /* already going */
    if (care(MUSE_PET_CLEAN, false)) return;
    /* The tick shows poops 0..n-1: this one takes slot n, the one to go. */
    muse_pet_state_t st;
    muse_pet_state(&st);
    for (int k = 0; k < POOPS_MAX && st.poops < POOPS_MAX; k++) {
        if (s_poop[k] == o) {
            s_poop[k] = s_poop[st.poops];
            s_poop[st.poops] = o;
            break;
        }
    }
    anim(o, set_opa, 255, 0, 400, 0, NULL, true);
}

static void on_poop(lv_event_t *e)
{
    clean_poop(lv_event_get_target(e));
}

/* ---- Dragging the shower and the foods ---- */

/* Page coordinates are from its centre, as the objects are aligned. */
static void page_centre_of(lv_obj_t *o, int *x, int *y)
{
    lv_area_t a, t;
    lv_obj_get_coords(o, &a);
    lv_obj_get_coords(s_tile, &t);
    *x = (a.x1 + a.x2) / 2 - (t.x1 + t.x2) / 2;
    *y = (a.y1 + a.y2) / 2 - (t.y1 + t.y2) / 2;
}

static void page_point(lv_indev_t *indev, int *x, int *y)
{
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);
    lv_area_t t;
    lv_obj_get_coords(s_tile, &t);
    *x = p.x - (t.x1 + t.x2) / 2;
    *y = p.y - (t.y1 + t.y2) / 2;
}

static void slot_home(int i, int *x, int *y)
{
    float rad = SLOT_AT[i].angle * 3.14159265f / 180.0f;
    *x = (int)lroundf(cosf(rad) * SLOT_RING);
    *y = (int)lroundf(sinf(rad) * SLOT_RING);
    if (*y > SLOT_LOW) *y = SLOT_LOW;
}

/* Its body, not the whole of its image. */
static bool on_pet(int x, int y)
{
    return abs(x) < 84 && y > s_avatar_y - 90 && y < s_avatar_y + 116;
}

static lv_obj_t *poop_at(int x, int y)
{
    for (int k = 0; k < POOPS_MAX; k++) {
        lv_obj_t *o = s_poop[k];
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) || lv_anim_get(o, NULL)) continue;
        int px, py;
        page_centre_of(o, &px, &py);
        if (abs(x - px) < 24 && abs(y - py) < 24) return o;
    }
    return NULL;
}

static void go_home(int i)
{
    int hx, hy;
    slot_home(i, &hx, &hy);
    lv_obj_t *o = s_slot[i];
    anim(o, set_x, s_drag[i].x, hx, 250, 0, lv_anim_path_ease_out, false);
    anim(o, set_y, s_drag[i].y, hy, 250, 0, lv_anim_path_ease_out, false);
    anim(o, set_scale, (int32_t)lv_image_get_scale(o), 256, 250, 0, NULL, false);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    s_drag[i].x = hx;
    s_drag[i].y = hy;
}

/* Eaten: another food appears in its slot, not the same one. */
static void refill(lv_anim_t *a)
{
    for (int i = 0; i < SLOTS; i++) {
        if (s_slot[i] != a->var) continue;
        if (SLOT_AT[i].action == MUSE_PET_FEED) {
            int n = (int)(sizeof(FOODS) / sizeof(FOODS[0]));
            s_food = (s_food + 1 + (int)(esp_random() % (uint32_t)(n - 1))) % n;
            lv_image_set_src(s_slot[i], big(FOODS[s_food].icon));
        }
        slot_home(i, &s_drag[i].x, &s_drag[i].y);
        lv_obj_set_pos(s_slot[i], s_drag[i].x, s_drag[i].y);
        lv_image_set_scale(s_slot[i], 256);
        lv_obj_add_flag(s_slot[i], LV_OBJ_FLAG_CLICKABLE);
        anim(s_slot[i], set_opa, 0, 255, 300, 0, NULL, false);
    }
}

static void eat(int i)
{
    lv_obj_t *o = s_slot[i];
    int mouth = s_avatar_y + 14;
    /* Not to be grabbed while it goes: deleting its animation would lose the
     * refill. A tap there strokes the pet instead. */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    anim(o, set_x, s_drag[i].x, 0, 300, 0, lv_anim_path_ease_in_out, false);
    anim(o, set_y, s_drag[i].y, mouth, 300, 0, lv_anim_path_ease_in_out, false);
    anim_then(o, set_scale, (int32_t)lv_image_get_scale(o), 0, 500, 300, NULL, refill);
    s_drag[i].x = 0;
    s_drag[i].y = mouth;
}

static void bath(void)
{
    if (care(MUSE_PET_WASH, true)) return;
    for (int k = 0; k < POOPS_MAX; k++) {   /* the poops go with the bath */
        if (!lv_obj_has_flag(s_poop[k], LV_OBJ_FLAG_HIDDEN) && !lv_anim_get(s_poop[k], NULL)) {
            anim(s_poop[k], set_opa, 255, 0, 400, 0, NULL, true);
        }
    }
}

/* The shower held at (x, y): water falls from its head, and what the water
 * stays on a moment is washed: a poop, or the pet itself, once a drag. */
static void shower(int x, int y)
{
    uint32_t now = lv_tick_get();
    int wx = x - 2, wy = y + 26;   /* the head's holes, left of centre */
    if (lv_tick_diff(now, s_spray_ms) >= 80 && (!s_bathed || lv_tick_diff(now, s_bath_ms) >= 1500)) {
        static const int8_t JITTER[] = { -9, 3, -3, 8, -6, 0 };
        int dx = wx + JITTER[s_drops++ % sizeof(JITTER)];
        lv_obj_t *d = sprite(I_DROP, true, dx, wy);
        lv_image_set_scale(d, 128);
        anim(d, set_y, wy, wy + 36, 300, 0, lv_anim_path_ease_in, false);
        anim(d, set_opa, 255, 0, 300, 0, NULL, true);
        s_spray_ms = now;
    }
    wy += 18;   /* where it lands */
    lv_obj_t *under = poop_at(wx, wy);
    if (!under && !s_bathed && on_pet(wx, wy)) under = s_avatar;
    if (under != s_under) {
        s_under = under;
        s_under_ms = now;
    } else if (under && lv_tick_diff(now, s_under_ms) >= (under == s_avatar ? 600u : 250u)) {
        s_under = NULL;
        if (under == s_avatar) {
            s_bathed = true;   /* refused too: once is enough to say why */
            s_bath_ms = now;
            bath();
        } else {
            clean_poop(under);
        }
    }
}

/* Its scroll chain is cut, so dragging it never swipes the page. */
static void on_tool(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *o = s_slot[i];
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_INDEV_RESET) code = LV_EVENT_PRESS_LOST;   /* its release won't come */
    bool wash = SLOT_AT[i].action == MUSE_PET_WASH;
    int px, py;
    page_point(lv_event_get_indev(e), &px, &py);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(o, NULL);   /* caught on its way back */
        page_centre_of(o, &s_drag[i].x, &s_drag[i].y);
        s_drag[i].grab_x = px - s_drag[i].x;
        s_drag[i].grab_y = py - s_drag[i].y;
        s_drag[i].held = true;
        s_drag[i].moved = false;
        s_under = NULL;
        s_bathed = false;
        lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
        lv_image_set_scale(o, 288);   /* picked up */
        lv_obj_move_foreground(o);
        return;
    }
    if (!s_drag[i].held) return;
    int x = px - s_drag[i].grab_x, y = py - s_drag[i].grab_y;
    if (code == LV_EVENT_PRESSING) {
        if (!s_drag[i].moved && abs(x - s_drag[i].x) + abs(y - s_drag[i].y) < 10) return;   /* a tap's wobble */
        s_drag[i].moved = true;
        s_drag[i].x = x;
        s_drag[i].y = y;
        lv_obj_set_pos(o, x, y);
        if (wash) shower(x, y);
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    s_drag[i].held = false;
    s_under = NULL;
    if (!s_drag[i].moved) {
        if (code == LV_EVENT_RELEASED) say(wash ? "SHOWER ME" : "DRAG TO ME");
    } else if (!wash && code == LV_EVENT_RELEASED && on_pet(s_drag[i].x, s_drag[i].y)) {
        if (!care(slot_action(i), false)) {
            eat(i);
            return;
        }
    }
    go_home(i);   /* also refused: FULL, ASLEEP */
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

static lv_obj_t *disc(lv_obj_t *o)
{
    lv_obj_set_size(o, SLOT_PX, SLOT_PX);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x2a2140), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

lv_obj_t *muse_pet_ui_build(lv_obj_t *tile, const void *avatar_src)
{
    for (int i = 0; i < I_COUNT; i++) {
        s_img[i] = icon_image(i, 3);
        s_small[i] = icon_image(i, 2);
    }
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* The avatar: the face's own image, a little up to leave the bottom to care. */
    s_avatar_y = -26;
    s_avatar = lv_image_create(tile);
    lv_image_set_src(s_avatar, avatar_src);
    lv_obj_align(s_avatar, LV_ALIGN_CENTER, 0, s_avatar_y);
    lv_obj_add_flag(s_avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_avatar, on_avatar, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < POOPS_MAX; i++) {
        /* By its feet, then its sides, above the dishes and below the gauges'
         * names: clear of the slots' discs and the tools' click areas. */
        static const int X[POOPS_MAX] = { -38, 38, -108, 108 }, Y[POOPS_MAX] = { 98, 98, 22, 22 };
        s_poop[i] = lv_image_create(tile);
        lv_image_set_src(s_poop[i], s_img[I_POOP]);
        lv_obj_align(s_poop[i], LV_ALIGN_CENTER, X[i], Y[i]);
        lv_obj_add_flag(s_poop[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(s_poop[i], on_poop, LV_EVENT_CLICKED, NULL);
    }

    /* Night: the page dims; the slots stay on top. */
    s_night = lv_obj_create(tile);
    lv_obj_remove_style_all(s_night);
    lv_obj_set_size(s_night, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_night, lv_color_hex(0x05030f), 0);
    lv_obj_set_style_bg_opa(s_night, LV_OPA_50, 0);
    lv_obj_add_flag(s_night, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_night, LV_OBJ_FLAG_CLICKABLE);

    s_name = label(tile, &lv_font_unscii_16, 0xf2efff, -168);
    for (int i = 0; i < HEARTS; i++) {
        s_heart[i] = lv_image_create(tile);
        lv_image_set_src(s_heart[i], s_small[I_HEART]);
        lv_obj_align(s_heart[i], LV_ALIGN_CENTER, (i - HEARTS / 2) * 26, -144);
    }
    s_need = label(tile, &lv_font_unscii_16, 0xffd24a, -120);
    lv_obj_set_style_text_letter_space(s_need, 0, 0);   /* a 12-letter caption fits between the top rings */

    for (int i = 0; i < GAUGES; i++) {
        lv_obj_t *g = lv_arc_create(tile);
        lv_obj_set_size(g, 54, 54);
        lv_obj_align(g, LV_ALIGN_CENTER, GAUGE_AT[i].x, GAUGE_AT[i].y);
        lv_arc_set_range(g, 0, 100);
        lv_obj_remove_style(g, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(g, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(g, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(g, lv_color_hex(0x221c33), LV_PART_MAIN);
        lv_obj_set_style_arc_color(g, lv_color_hex(GAUGE_AT[i].color), LV_PART_INDICATOR);
        lv_obj_t *ic = lv_image_create(g);
        lv_image_set_src(ic, s_small[GAUGE_AT[i].icon]);
        lv_obj_center(ic);
        s_gauge[i] = g;
        lv_obj_t *name = label(tile, &lv_font_montserrat_14, GAUGE_AT[i].color, GAUGE_AT[i].y + 38);
        lv_label_set_text(name, GAUGE_AT[i].name);
        lv_obj_align(name, LV_ALIGN_CENTER, GAUGE_AT[i].x, GAUGE_AT[i].y + 38);
    }

    for (int i = 0; i < SLOTS; i++) {
        int x, y;
        slot_home(i, &x, &y);
        if (!SLOT_AT[i].drag) {
            lv_obj_t *b = disc(lv_button_create(tile));
            lv_obj_set_style_bg_color(b, lv_color_hex(0x4a3b70), LV_STATE_PRESSED);
            lv_obj_align(b, LV_ALIGN_CENTER, x, y);
            lv_obj_add_event_cb(b, on_button, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, on_button, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
            lv_obj_t *ic = lv_image_create(b);
            lv_image_set_src(ic, big(SLOT_AT[i].icon));
            lv_obj_center(ic);
            if (SLOT_AT[i].action == MUSE_PET_SLEEP) s_sleep_icon = ic;
            s_slot[i] = b;
            continue;
        }
        /* A dish it sits in and goes back to; the tool itself moves. */
        lv_obj_t *d = lv_obj_create(tile);
        lv_obj_remove_style_all(d);
        disc(d);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(d, LV_ALIGN_CENTER, x, y);
        lv_obj_t *o = lv_image_create(tile);
        lv_image_set_src(o, big(SLOT_AT[i].icon));
        lv_obj_align(o, LV_ALIGN_CENTER, x, y);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER |
                                  LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_ext_click_area(o, (SLOT_PX - TOOL_PX) / 2);   /* the whole dish */
        static const lv_event_code_t EVENTS[] = { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                                                  LV_EVENT_PRESS_LOST, LV_EVENT_INDEV_RESET };
        for (size_t k = 0; k < sizeof(EVENTS) / sizeof(EVENTS[0]); k++) {
            lv_obj_add_event_cb(o, on_tool, EVENTS[k], (void *)(intptr_t)i);
        }
        s_drag[i].x = x;
        s_drag[i].y = y;
        s_slot[i] = o;
    }

    for (int i = 0; i < PARTICLES; i++) {
        s_particle[i] = lv_image_create(tile);
        lv_obj_add_flag(s_particle[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_particle[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_tile = tile;
    return s_avatar;
}

void muse_pet_ui_tick(float now)
{
    if (!s_tile) return;
    static int volume = -1;
    static float last_tick = -1;
    /* Ticked only while the page shows: a change made while it didn't (the
     * settings, the agent) is taken as it is, not shown as the wheel's. */
    if (now - last_tick > 0.5f) volume = -1;
    last_tick = now;
    if (volume != muse_settings_volume()) {
        if (volume >= 0) {
            char b[16];
            snprintf(b, sizeof(b), "VOLUME %d%%", muse_settings_volume());
            say(b);   /* the wheel turned: its tunes play at this */
        }
        volume = muse_settings_volume();
    }
    muse_pet_state_t st;
    muse_pet_state(&st);
    float v[GAUGES] = { st.food, st.clean, st.fun, st.energy };
    for (int i = 0; i < GAUGES; i++) {
        int x = (int)v[i];
        if (x != s_shown_gauge[i]) {
            lv_arc_set_value(s_gauge[i], x);
            s_shown_gauge[i] = x;
        }
    }
    for (int i = 0; i < HEARTS; i++) {
        const void *src = st.health > i * 20 + 10 ? s_small[I_HEART] : s_small[I_HEART_OFF];
        if (lv_image_get_src(s_heart[i]) != src) lv_image_set_src(s_heart[i], src);
    }
    for (int i = 0; i < POOPS_MAX; i++) {
        bool show = i < st.poops;
        if (!show && lv_anim_get(s_poop[i], NULL)) {
            continue;   /* fading out: it hides itself */
        }
        if (show == lv_obj_has_flag(s_poop[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_anim_delete(s_poop[i], NULL);
            lv_obj_set_style_opa(s_poop[i], LV_OPA_COVER, 0);
            lv_obj_set_flag(s_poop[i], LV_OBJ_FLAG_HIDDEN, !show);
        }
    }
    for (int i = 0; i < SLOTS; i++) {
        /* A tool left out (its release lost to a page change) goes back. */
        if (!SLOT_AT[i].drag || s_drag[i].held || lv_anim_get(s_slot[i], NULL)) continue;
        int hx, hy;
        slot_home(i, &hx, &hy);
        if (s_drag[i].x != hx || s_drag[i].y != hy) go_home(i);
    }
    if (st.asleep == lv_obj_has_flag(s_night, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_flag(s_night, LV_OBJ_FLAG_HIDDEN, !st.asleep);
        lv_image_set_src(s_sleep_icon, big(st.asleep ? I_SUN : I_MOON));
    }
    char name[40];
    snprintf(name, sizeof(name), "%s  DAY %d", st.name, (int)st.age_days + 1);
    if (strcmp(name, lv_label_get_text(s_name))) lv_label_set_text(s_name, name);
    char say[24];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool saying = esp_timer_get_time() < s_say_until_us, no = s_say_no;
    strlcpy(say, s_say, sizeof(say));
    xSemaphoreGive(s_lock);
    const char *need = saying ? say : muse_pet_need();
    if (!need[0]) {
        float avg = (st.food + st.clean + st.fun + st.energy + st.health) / 5;
        need = avg > 75 ? "HAPPY!" : "CONTENT";
    }
    if (strcmp(need, lv_label_get_text(s_need))) {
        lv_label_set_text(s_need, need);
        bool bad = st.sick || st.fainted || st.food < 25 || st.clean < 25 || st.poops >= 2;
        uint32_t color = saying ? (no ? 0xff8f8f : 0xffb3e6) : bad ? 0xffd24a : 0x9ff5cf;
        lv_obj_set_style_text_color(s_need, lv_color_hex(color), 0);
    }
}
