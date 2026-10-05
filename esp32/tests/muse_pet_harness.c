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

// Host harness for the pet's care model (components/muse/muse_pet.c): how its
// needs fall, what care does and refuses, poops after meals, neglect into
// sickness and fainting, sleep, the hours off it catches up, and the Muse's name as its page shows it. The runner
// extracts the code between the model markers into muse_pet_model.inc.
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "muse_pet.h"

#include "muse_pet_model.inc"

static bool near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void in_bounds(const pet_t *p) {
    const float v[] = { p->food, p->clean, p->fun, p->energy, p->health };
    for (int i = 0; i < 5; i++) assert(v[i] >= 0 && v[i] <= 100);
    assert(p->poops <= POOPS_MAX && p->germs >= 0);
}

/* A pet well cared for: what the decay and neglect timelines start from. */
static void pet_cared(pet_t *p) {
    pet_new(p);
    p->food = 80;
    p->clean = 90;
    p->fun = 80;
    p->energy = 90;
    p->poops = 0;
}

static void test_new(void) {
    pet_t p;
    pet_new(&p);
    /* It starts wanting care (something to do at once), but well. */
    assert(p.food == 45 && p.clean == 55 && p.fun == 40 && p.energy == 60 && p.health == 100);
    assert(!strcmp(p.name, "MUSE") && !p.sick && !p.asleep && !p.fainted && p.poops == 1);
}

static void test_an_hour(void) {
    pet_t p;
    pet_cared(&p);
    assert(pet_advance(&p, 1.0f) == 0);
    assert(near(p.food, 76) && near(p.fun, 76.5f) && near(p.energy, 85) && near(p.clean, 87));
    assert(near(p.age_h, 1) && p.health == 100);
}

static void test_feeding(void) {
    pet_t p;
    pet_cared(&p);
    assert(pet_do(&p, MUSE_PET_FEED) == NULL && p.food == 100 && p.cared == 1);
    assert(!strcmp(pet_do(&p, MUSE_PET_FEED), "FULL") && p.cared == 1);
    assert(near(p.poop_in_h, 3));
    float fun = p.fun, health = p.health;
    assert(pet_do(&p, MUSE_PET_SNACK) == NULL);   // a treat's fine when full
    assert(near(p.fun, fun + 12 > 100 ? 100 : fun + 12) && near(p.health, health - 1));
    // The poop comes three hours after the meal, not three after the snack.
    assert(pet_advance(&p, 2.95f) == 0 && p.poops == 0);
    assert(pet_advance(&p, 0.1f) & EV_POOP);
    assert(p.poops == 1 && p.poop_in_h == 0);
    float clean = p.clean;
    pet_advance(&p, 1);
    assert(near(clean - p.clean, 3 + 1.5f));   // a poop on the floor dirties it faster
    assert(pet_do(&p, MUSE_PET_CLEAN) == NULL && p.poops == 0);
    assert(!strcmp(pet_do(&p, MUSE_PET_CLEAN), "ALL CLEAN"));
}

static void test_wash(void) {
    pet_t p;
    pet_cared(&p);
    p.clean = 10;
    p.poops = 3;
    p.germs = 2;
    assert(!strcmp(pet_need(&p), "CLEAN UP!"));
    assert(pet_do(&p, MUSE_PET_WASH) == NULL);
    assert(p.clean == 100 && p.poops == 0 && near(p.germs, 0.5f));
}

// Hours of neglect: hungry, then sick, then fainted; nothing dies.
static void test_neglect(void) {
    pet_t p;
    pet_cared(&p);
    pet_do(&p, MUSE_PET_FEED);
    float hungry = -1, sick = -1, fainted = -1;
    unsigned seen = 0;
    for (float h = 0; h < 72; h += 0.25f) {
        unsigned ev = pet_advance(&p, 0.25f);
        seen |= ev;
        in_bounds(&p);
        if (hungry < 0 && p.food < 25) hungry = h;
        if (sick < 0 && (ev & EV_SICK)) sick = h;
        if (fainted < 0 && (ev & EV_FAINTED)) fainted = h;
    }
    printf("neglected: hungry after %.1f h, sick after %.1f h, fainted after %.1f h\n", hungry, sick, fainted);
    assert(hungry > 12 && hungry < 24);   // fed at night, hungry the next day
    assert(sick > 20 && sick < 36);       // a day of neglect
    assert(fainted > 40 && fainted < 72); // two

    assert(seen & EV_POOP);
    assert(p.fainted && p.sick && p.health == 0);
    assert(!strcmp(pet_need(&p), "FAINTED!"));
    // Medicine brings it back, and care brings back its health.
    assert(pet_do(&p, MUSE_PET_HEAL) == NULL);
    assert(!p.sick && !p.fainted && p.germs == 0 && near(p.health, 55));
    p.asleep = 0;
    pet_do(&p, MUSE_PET_FEED);
    pet_do(&p, MUSE_PET_FEED);
    pet_do(&p, MUSE_PET_FEED);
    pet_do(&p, MUSE_PET_WASH);
    p.fun = 90;
    p.energy = 90;
    pet_advance(&p, 4);
    assert(p.health > 60 && !p.sick);
}

static void test_sleep(void) {
    pet_t p;
    pet_cared(&p);
    p.energy = 15;
    assert(!strcmp(pet_need(&p), "SLEEPY"));
    assert(pet_do(&p, MUSE_PET_SLEEP) == NULL && p.asleep);
    assert(!strcmp(pet_do(&p, MUSE_PET_FEED), "ASLEEP"));
    assert(!strcmp(pet_do(&p, MUSE_PET_PLAY), "ASLEEP"));
    assert(!strcmp(pet_do(&p, MUSE_PET_WASH), "ASLEEP"));
    assert(pet_do(&p, MUSE_PET_PET) == NULL);   // a stroke's always welcome
    assert(!strcmp(pet_need(&p), "ZZZ..."));
    float food = p.food;
    unsigned ev = 0;
    for (int i = 0; i < 20 && p.asleep; i++) ev |= pet_advance(&p, 0.5f);
    assert(ev & EV_WOKE);
    assert(!p.asleep && p.energy > 97 && p.food < food);   // woke full, a little of the hour left
    assert(food - p.food < 4 * 5);   // hunger slows in its sleep
    // Worn out, it drops off by itself.
    p.energy = 5;
    assert(pet_advance(&p, 1) & EV_FELL_ASLEEP);
    assert(p.asleep);
    assert(pet_do(&p, MUSE_PET_WAKE) == NULL && !p.asleep);
    assert(!strcmp(pet_do(&p, MUSE_PET_WAKE), "AWAKE"));
}

static void test_play(void) {
    pet_t p;
    pet_cared(&p);
    p.fun = 20;
    assert(!strcmp(pet_need(&p), "BORED"));
    assert(pet_do(&p, MUSE_PET_PLAY) == NULL && near(p.fun, 42) && near(p.energy, 80));
    p.energy = 10;
    assert(!strcmp(pet_do(&p, MUSE_PET_PLAY), "TOO TIRED"));
    p.energy = 50;
    p.sick = 1;
    assert(!strcmp(pet_do(&p, MUSE_PET_PLAY), "TOO POORLY"));
    assert(!strcmp(pet_need(&p), "FEELING SICK"));
    assert(pet_do(&p, MUSE_PET_HEAL) == NULL && !p.sick);
    assert(!strcmp(pet_do(&p, MUSE_PET_HEAL), "NOT SICK"));
}

// Three days off (the most tick() catches up), from any state: in bounds, and
// the same whether in one step or many.
static void test_catch_up(void) {
    pet_t a, b;
    pet_cared(&a);
    pet_cared(&b);
    pet_advance(&a, 72);
    for (int i = 0; i < 72 * 6; i++) pet_advance(&b, 1.0f / 6);
    in_bounds(&a);
    assert(near(a.food, b.food) && near(a.health, b.health) && a.poops == b.poops && a.sick == b.sick);
    assert(near(a.age_h, 72));
}

// The hours it was off, from its last step to this boot; a new pet has none,
// even made before the clock is set, when its old save's would still count.
static void test_hours_off(void) {
    const int64_t t = TIME_VALID + 86400;
    pet_t p;
    pet_new(&p);
    p.updated = t;
    assert(near(pet_off_hours(&p, t + 5 * 3600 + 60, 60), 5));   // off five hours, up a minute
    assert(near(pet_off_hours(&p, t + 30 * 86400, 60), CATCH_UP_MAX_H));
    assert(pet_off_hours(&p, t - 3600, 60) == 0);                 // the clock went back
    p.updated = 0;
    assert(pet_off_hours(&p, t, 60) == 0);                        // never set: nothing to count from
    // Reset with the clock unset, then set ten hours on: the old pet's save doesn't count.
    p.updated = t;
    p.food = 90;
    pet_restart(&p, 100);
    assert(p.updated == 0 && p.food == 45 && pet_off_hours(&p, t + 10 * 3600, 60) == 0);
    pet_restart(&p, t);
    assert(p.updated == t && pet_off_hours(&p, t + 5, 5) == 0);
}

// Unnamed, it shows the Muse's name, kept to what its page's font draws.
static void test_muse_name(void) {
    char out[16];
    assert(pet_muse_name("Jolly", out, sizeof(out)) && !strcmp(out, "JOLLY"));
    assert(pet_muse_name("  Mr.  O'Neil-2 ", out, sizeof(out)) && !strcmp(out, "MR. O'NEIL-2"));
    assert(pet_muse_name("Zo\xc3\xab", out, sizeof(out)) && !strcmp(out, "ZO"));   // UTF-8 dropped whole
    // Cut at 15, and never inside a multi-byte character.
    assert(pet_muse_name("abcdefghijklmn\xc3\xa9xyz", out, sizeof(out)) && !strcmp(out, "ABCDEFGHIJKLMNX"));
    assert(pet_muse_name("A very long name", out, sizeof(out)) && !strcmp(out, "A VERY LONG NAM"));
    assert(!pet_muse_name("\xe5\xa4\xaa\xe9\x83\x8e", out, sizeof(out)));   // nothing left: it stays MUSE
    assert(!pet_muse_name(" -.. ", out, sizeof(out)));
    assert(!pet_muse_name("", out, sizeof(out)));
}

// A pet looked after a few times a day stays well.
static void test_cared_for(void) {
    pet_t p;
    pet_cared(&p);
    for (int day = 0; day < 7; day++) {
        for (int visit = 0; visit < 4; visit++) {
            pet_do(&p, MUSE_PET_FEED);
            if (p.poops) pet_do(&p, MUSE_PET_WASH);
            pet_do(&p, MUSE_PET_PLAY);
            pet_advance(&p, 4);
            if (p.asleep) pet_do(&p, MUSE_PET_WAKE);
        }
        pet_do(&p, MUSE_PET_SLEEP);
        pet_advance(&p, 8);
        if (p.asleep) pet_do(&p, MUSE_PET_WAKE);
    }
    printf("cared for a week: food %.0f clean %.0f fun %.0f energy %.0f health %.0f, sick %d\n", p.food, p.clean,
           p.fun, p.energy, p.health, p.sick);
    assert(!p.sick && !p.fainted && p.health > 80);
}

int main(void) {
    test_new();
    test_an_hour();
    test_feeding();
    test_wash();
    test_neglect();
    test_sleep();
    test_play();
    test_catch_up();
    test_hours_off();
    test_muse_name();
    test_cared_for();
    printf("pet model ok\n");
    return 0;
}
