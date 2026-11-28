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

#include "mg_chain.h"

#include <string.h>

void mg_chain_init(mg_chain_t *c, void (*run)(int16_t *, size_t), void (*reset)(void))
{
    c->run = run;
    c->reset = reset;
    c->quiet = true;
}

void mg_chain_audio(mg_chain_t *c, int16_t *pcm, size_t n)
{
    if (!c->run || !n) {
        return;
    }
    c->quiet = false;
    c->run(pcm, n);
}

const int16_t *mg_chain_silence(mg_chain_t *c, const int16_t *silence, int16_t *scratch, size_t n)
{
    if (!c->run || c->quiet) {
        return silence;
    }
    memset(scratch, 0, n * sizeof(int16_t));
    c->run(scratch, n);
    bool zero = true;
    for (size_t i = 0; i < n && zero; i++) {
        zero = scratch[i] == 0;
    }
    if (zero) {
        mg_chain_discard(c);   /* its look-ahead is zeros, its filters below a sample: start the next afresh */
    }
    return scratch;
}

void mg_chain_discard(mg_chain_t *c)
{
    if (c->reset) {
        c->reset();
    }
    c->quiet = true;
}
