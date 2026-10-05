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

#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Muse as a pet, on its own page left of the face. Its needs run down in
 * real time (the clock, so hours off count too): food, cleanliness, fun,
 * energy and health, with poops to clean up and sickness from neglect. Care
 * restores them. Its state is kept in NVS. Every call is safe from any task.
 */

typedef enum {
    MUSE_PET_FEED,      /* a meal */
    MUSE_PET_SNACK,     /* a treat: fun, a little food */
    MUSE_PET_WASH,      /* a bath: clean, and the poops gone */
    MUSE_PET_CLEAN,     /* one poop picked up */
    MUSE_PET_PLAY,
    MUSE_PET_SLEEP,     /* lights out */
    MUSE_PET_WAKE,
    MUSE_PET_HEAL,      /* medicine */
    MUSE_PET_PET,       /* a stroke */
    MUSE_PET_ACTIONS,
} muse_pet_action_t;

typedef struct {
    float food, clean, fun, energy, health;   /* 0..100 */
    int poops;                                /* 0..4 on the floor */
    bool sick, asleep, fainted;
    float age_days;
    int cared;                                /* things done for it, ever */
    char name[16];                            /* its own, or else the Muse's */
    bool named;                               /* its own: not MUSE, which goes by the Muse's */
} muse_pet_state_t;

void muse_pet_init(void);

/* Whether it's running: only boards with touch have a pet. */
bool muse_pet_ready(void);

void muse_pet_state(muse_pet_state_t *out);

/* Does it; NULL, or why not ("full", "asleep"...). */
const char *muse_pet_care(muse_pet_action_t action);

const char *muse_pet_action_name(muse_pet_action_t action);
bool muse_pet_action_by_name(const char *name, muse_pet_action_t *out);

void muse_pet_set_name(const char *name);
/* A new pet, from the start: its care begins again (the name too). */
void muse_pet_reset(void);

/* What it needs most, for a label: "HUNGRY", "NEEDS A BATH"...; "" if nothing. */
const char *muse_pet_need(void);

/* The page (the LVGL task): built in its tile around an image of avatar_src,
 * which it returns for the UI to redraw; ticked each frame it shows. */
struct _lv_obj_t;
struct _lv_obj_t *muse_pet_ui_build(struct _lv_obj_t *tile, const void *avatar_src);
void muse_pet_ui_tick(float now);
