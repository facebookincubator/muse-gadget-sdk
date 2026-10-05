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

// The pet (components/muse/muse_pet.h) for the agent and scripts: pet.status,
// pet.care and pet.name. What happens to it arrives as "pet" events.

#include <math.h>
#include <string.h>

#include "muse_board.h"
#include "muse_hw_commands_priv.h"
#include "muse_pet.h"

static cJSON *state_json(void) {
    muse_pet_state_t st;
    muse_pet_state(&st);
    cJSON *pl = cJSON_CreateObject();
    cJSON_AddStringToObject(pl, "name", st.name);
    cJSON_AddBoolToObject(pl, "named", st.named);   // false: it goes by the Muse's name
    cJSON_AddNumberToObject(pl, "food", (int)st.food);
    cJSON_AddNumberToObject(pl, "clean", (int)st.clean);
    cJSON_AddNumberToObject(pl, "fun", (int)st.fun);
    cJSON_AddNumberToObject(pl, "energy", (int)st.energy);
    cJSON_AddNumberToObject(pl, "health", (int)st.health);
    cJSON_AddNumberToObject(pl, "poops", st.poops);
    cJSON_AddBoolToObject(pl, "sick", st.sick);
    cJSON_AddBoolToObject(pl, "asleep", st.asleep);
    cJSON_AddBoolToObject(pl, "fainted", st.fainted);
    cJSON_AddNumberToObject(pl, "age_days", floorf(st.age_days * 10) / 10);
    cJSON_AddNumberToObject(pl, "cared", st.cared);
    const char *need = muse_pet_need();
    cJSON_AddStringToObject(pl, "needs", need[0] ? need : "nothing");
    return pl;
}

void muse_hw_pet_register(cJSON *commands) {
    // muse_board is set by now (muse_hw_commands_register), and it has a pet only with touch.
    if (!muse_board->touch) return;
    hw_add(commands, "pet.status",
           "Muse's pet: the page left of the face, a virtual pet of Muse itself. Its needs run from "
           "0 to 100 and fall by the hour: food, clean, fun, energy, and health, which neglect and "
           "sickness wear down. Also poops on the floor, sick, asleep, fainted (health ran out: "
           "heal it), age_days, and what it needs most right now. "
           "name is its own when named is true, else the Muse's.",
           NULL, NULL, 0);
    hw_add(commands, "pet.care",
           "Look after the pet, as its page does: feed (a meal), snack (a treat, more fun), "
           "wash (a bath, poops gone), clean (pick up one poop), play (costs energy), sleep, wake, "
           "heal (medicine, for sick or fainted) or pet (a stroke). done is false with a reason when "
           "it won't (FULL, ASLEEP, TOO TIRED...). Returns its state.",
           hw_params("action", hw_param("string", "feed, snack, wash, clean, play, sleep, wake, heal or pet.")),
           NULL, 0);
    hw_add(commands, "pet.reset", "Start a new pet: its needs start low, with a poop, so there's care to give.",
           NULL, NULL, 0);
    hw_add(commands, "pet.name",
           "Name the pet; it's shown on its page. MUSE unnames it: it goes by the Muse's name again.",
           hw_params("name", hw_param("string", "1 to 15 letters, digits or spaces.")), NULL, 0);
}

cJSON *muse_hw_pet_command(const char *command, cJSON *params) {
    if (!muse_pet_ready()) {
        // With touch, it's only not started yet.
        return muse_board->touch ? hw_error("unavailable", "the pet is still starting")
                                 : hw_error("unsupported", "this board has no pet");
    }
    if (!strcmp(command, "pet.status")) return hw_ok(state_json());
    if (!strcmp(command, "pet.care")) {
        const char *action = hw_str(params, "action");
        muse_pet_action_t a;
        if (!action || !muse_pet_action_by_name(action, &a)) {
            return hw_error("invalid_params", "action is feed, snack, wash, clean, play, sleep, wake, heal or pet");
        }
        const char *why = muse_pet_care(a);
        cJSON *pl = state_json();
        cJSON_AddBoolToObject(pl, "done", !why);
        if (why) cJSON_AddStringToObject(pl, "reason", why);
        return hw_ok(pl);
    }
    if (!strcmp(command, "pet.reset")) {
        muse_pet_reset();
        return hw_ok(state_json());
    }
    if (!strcmp(command, "pet.name")) {
        const char *name = hw_str(params, "name");
        size_t n = name ? strlen(name) : 0;
        bool ok = n >= 1 && n <= 15;
        for (size_t i = 0; ok && i < n; i++) {
            char c = name[i];
            ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
        }
        if (!ok) return hw_error("invalid_params", "name is 1 to 15 letters, digits or spaces");
        char upper[16];
        for (size_t i = 0; i <= n; i++) upper[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        muse_pet_set_name(upper);   // the page's font is capitals
        return hw_ok(state_json());
    }
    return NULL;
}
