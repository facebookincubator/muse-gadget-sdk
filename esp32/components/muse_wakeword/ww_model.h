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

/*
 * The contract of models/hey_muse.pte, the one model the runner runs,
 * on the chip and off it. models/hey_muse.json records the same values;
 * tests/test_muse_wakeword.py checks the two agree.
 *
 * Every 30 ms the model takes the newest three 10 ms feature frames and its
 * own streaming state, and returns one wake score and the next state:
 *
 *   (p, h0'..h5') = forward(x, h0..h5)
 *
 * x is int8 [1, 40, 3, 1]: channel c of frame f at x[c * 3 + f]. Each value
 * is a microfrontend feature v (uint16) quantized as
 * q = clamp(lrintf(v * 0.0390625 / WW_MODEL_INPUT_SCALE) + zero point).
 * p is int8 [1, 1]: probability (p - WW_MODEL_OUTPUT_ZERO_POINT) / 256.
 * The six states are int8; each comes back with the qparams it went in
 * with, and starts at its zero point (a float zero).
 */

#define WW_MODEL_FEATURES 40              /* microfrontend channels */
#define WW_MODEL_STRIDE 3                 /* feature frames per inference */
#define WW_MODEL_INPUT_BYTES (WW_MODEL_FEATURES * WW_MODEL_STRIDE)
#define WW_MODEL_INPUT_SCALE 0.10196078568696976f
#define WW_MODEL_INPUT_ZERO_POINT (-128)
#define WW_MODEL_OUTPUT_ZERO_POINT (-128) /* output scale is 1/256 */
#define WW_MODEL_STATES 6
#define WW_MODEL_STATE_BYTES 2160         /* all six states together */
#define WW_MODEL_STATE_ZERO_POINT (-128)  /* the same for every state */
