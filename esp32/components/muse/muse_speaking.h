/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

/* Normalized, interruptible speaking choreography. Pure C, fixed storage, no
 * clock/random APIs or allocations; the renderer supplies time and audio level.
 * A rest is a real state, not an invitation to fill every frame with motion. */
enum { MUSE_SPEAK_REST, MUSE_SPEAK_RAISE_RIGHT, MUSE_SPEAK_STEPS,
       MUSE_SPEAK_RAISE_LEFT, MUSE_SPEAK_OPEN };
typedef struct {
    bool active, heard_voice;
    unsigned action, next, side, leader, opener;
    uint32_t seed;
    float time, duration, mode_t, quiet, energy;
    float lift[2], extend[2], inward[2], wrist[2];
    float mouth, brow, eye_wide, gaze, nod;
} muse_speaking_t;

static inline float muse_speak_clamp(float x, float lo, float hi)
{
    return x < lo ? lo : x > hi ? hi : x;
}

static inline float muse_speak_ease(float x)
{
    x = muse_speak_clamp(x, 0, 1);
    return x * x * (3 - 2 * x);
}

static inline float muse_speak_random(muse_speaking_t *s)
{
    s->seed = s->seed * 1664525u + 1013904223u;
    return (s->seed >> 16) / 65535.0f;
}

static inline float muse_speak_pulse(float t, float length, float rise, float fall)
{
    return muse_speak_ease(t / rise) * (1 - muse_speak_ease((t - length + fall) / fall));
}

static inline void muse_speaking_update(muse_speaking_t *s, bool speaking, float dt, float mode_t, float level)
{
    dt = muse_speak_clamp(dt, 0, .2f);
    level = muse_speak_clamp(level, 0, 1);
    if (speaking && (!s->active || mode_t < s->mode_t)) {
        s->active = true;
        s->heard_voice = false;
        s->quiet = 0;
        s->action = MUSE_SPEAK_REST;
        /* Rotate openers too: several short replies must not always show the
         * same first gesture while never reaching the rest of the sequence. */
        s->next = MUSE_SPEAK_RAISE_RIGHT + s->opener++ % 4;
        s->time = 0;
        s->duration = .12f;
        s->leader = muse_speak_random(s) > .5f;
        s->mode_t = mode_t;
    }
    /* mode_t does not advance during preview warm-up. Real state changes
     * start from the current pose even if the first visible frame arrives late. */
    float advance = speaking ? muse_speak_clamp(mode_t - s->mode_t, 0, .2f) : dt;
    s->mode_t = mode_t;
    s->energy += ((speaking ? level : 0) - s->energy) * muse_speak_clamp(dt * 18, 0, 1);
    if (level > .06f && speaking) { s->heard_voice = true; s->quiet = 0; }
    else s->quiet += advance;
    bool pause = speaking && s->heard_voice && s->quiet > .28f;
    if (!speaking || pause) {
        s->action = MUSE_SPEAK_REST;
        s->time = 0;
        s->duration = .18f;
        if (!speaking) s->active = false;
    } else {
        s->time += advance;
        if (s->time >= s->duration) {
            s->time = 0;
            if (s->action == MUSE_SPEAK_REST) {
                s->action = s->next;
                s->side = s->action == MUSE_SPEAK_RAISE_RIGHT ? 1
                        : s->action == MUSE_SPEAK_RAISE_LEFT ? 0 : s->leader;
                s->duration = (s->action == MUSE_SPEAK_RAISE_RIGHT || s->action == MUSE_SPEAK_RAISE_LEFT) ? 1.15f
                            : s->action == MUSE_SPEAK_STEPS ? 1.90f : 1.65f;
                /* Advance on START, not completion. A mid-gesture pause must
                 * settle and continue with a different family on resumption. */
                s->next = s->action == MUSE_SPEAK_OPEN ? MUSE_SPEAK_RAISE_RIGHT : s->action + 1;
                if (s->action == MUSE_SPEAK_OPEN) s->leader ^= 1;
            } else {
                s->action = MUSE_SPEAK_REST;
                s->duration = .42f + .30f * muse_speak_random(s);
            }
        }
    }
    float lift[2] = {0}, extend[2] = {0}, inward[2] = {0}, wrist[2] = {0};
    float brow = 0, eye_wide = 0, gaze = 0, nod = 0;
    if (speaking && !pause && s->action != MUSE_SPEAK_REST) {
        float amplitude = .88f + .12f * s->energy;
        if (s->action == MUSE_SPEAK_RAISE_RIGHT || s->action == MUSE_SPEAK_RAISE_LEFT) {
            /* Each raise is independent. The opposite hand is never queued as
             * the second half of a raise; another movement comes between them. */
            int hand = s->side;
            float p = muse_speak_pulse(s->time, 1.10f, .28f, .34f) * amplitude;
            lift[hand] = p;
            extend[hand] = p * .14f;
            wrist[hand] = (hand ? -1 : 1) * p * .90f;
            brow = .75f * p;
            gaze = (hand ? 1 : -1) * .65f * p;
            nod = -.12f * p;
        } else if (s->action == MUSE_SPEAK_STEPS) {
            /* A hand in front of the torso marks three compact beats, moving
             * slightly higher with each point. The support paw stays low. */
            float envelope = muse_speak_pulse(s->time, s->duration, .25f, .30f);
            float beat = 0, progress = 0;
            for (int i = 0; i < 3; i++) {
                float t = s->time - .24f - i * .43f;
                beat += muse_speak_pulse(t, .34f, .10f, .17f);
                progress += muse_speak_ease(t / .12f) / 3;
            }
            int hand = s->side, support = hand ^ 1;
            lift[hand] = envelope * (.20f + .28f * progress + .18f * beat) * amplitude;
            inward[hand] = envelope * (.96f - .27f * beat);
            wrist[hand] = (hand ? 1 : -1) * envelope * .75f;
            lift[support] = .12f * envelope;
            inward[support] = .38f * envelope;
            brow = .30f * envelope + .40f * beat;
            eye_wide = .25f * beat;
            gaze = (hand ? 1 : -1) * .25f * envelope;
            nod = .50f * beat;
        } else {
            /* Open both paws outward together: a broad, balanced silhouette. */
            float p = muse_speak_pulse(s->time, s->duration, .36f, .42f) * amplitude;
            for (int i = 0; i < 2; i++) {
                lift[i] = p * .48f;
                extend[i] = p;
                wrist[i] = (i ? 1 : -1) * p * .50f;
            }
            brow = .85f * p;
            eye_wide = .80f * p;
            nod = -.40f * p;
        }
    }
    /* Critically bounded travel, including interrupted preparations and holds.
     * No final gesture is queued when speech ends, and a second hand is optional. */
    float blend = muse_speak_clamp(dt * 12, 0, 1);
    for (int i = 0; i < 2; i++) {
        s->lift[i] += (lift[i] - s->lift[i]) * blend;
        s->extend[i] += (extend[i] - s->extend[i]) * blend;
        s->inward[i] += (inward[i] - s->inward[i]) * blend;
        s->wrist[i] += (wrist[i] - s->wrist[i]) * blend;
        if (s->lift[i] < .001f) s->lift[i] = 0;
        if (s->extend[i] < .001f) s->extend[i] = 0;
        if (s->inward[i] < .001f) s->inward[i] = 0;
        if (s->wrist[i] > -.001f && s->wrist[i] < .001f) s->wrist[i] = 0;
    }
    s->brow += (brow - s->brow) * blend;
    s->eye_wide += (eye_wide - s->eye_wide) * blend;
    s->gaze += (gaze - s->gaze) * blend;
    s->nod += (nod - s->nod) * blend;

    float mouth = speaking ? muse_speak_clamp((s->energy - .035f) * 1.5f, 0, 1) : 0;
    if (speaking && !s->heard_voice) {
        /* Caption-paced replies may have silent PCM. Give them visible
         * articulation, with varied syllables and a short phrase breath. Once
         * actual voice is heard, its pauses remain closed instead of being
         * filled by this fallback. This is a visual rhythm, not phoneme sync. */
        static const float syllables[] = {.50f, .90f, .65f, .80f, .40f, .75f, .60f};
        float t = mode_t > .12f ? mode_t - .12f : 0;
        float syllable = t * 5.2f;
        unsigned index = (unsigned)syllable;
        float phase = syllable - index;
        float phrase = t - floorf(t / 2.8f) * 2.8f;
        float pulse = muse_speak_pulse(phase, .90f, .18f, .40f);
        mouth = phrase < 2.38f ? pulse * syllables[index % 7] : 0;
    }
    s->mouth += (mouth - s->mouth) * muse_speak_clamp(dt * 22, 0, 1);
    if (s->mouth < .001f) s->mouth = 0;
}
