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
#include <stdint.h>

typedef enum { AEC_MODE_FD_HIGH_PERF = 6 } aec_mode_t;
typedef enum { AEC_NLP_LEVEL_NORMAL = 0 } aec_nlp_level_t;
typedef struct {
    int mic_num, ref_num, out_num, filter_length, sample_rate;
    uint32_t caps;
    aec_mode_t mode;
    aec_nlp_level_t nlp_level;
} aec_config_t;
typedef struct { int frame_size; } aec_handle_t;

aec_handle_t *aec_create_from_config(aec_config_t *config);
int aec_get_chunksize(const aec_handle_t *handle);
void aec_process(const aec_handle_t *handle, int16_t *mic, int16_t *ref, int16_t *out);
void aec_destroy(aec_handle_t *handle);
