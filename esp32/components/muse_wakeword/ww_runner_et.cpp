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

#include "ww_runner.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <et_model.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ww_detector.h"
#include "ww_frontend.h"

extern "C" {
extern const uint8_t _binary_hey_muse_pte_start[];
extern const uint8_t _binary_hey_muse_pte_end[];
}

namespace
{

const char *kTag = "ww_et";
/* et_model_create requires 16-byte-aligned program data. */
constexpr size_t kPteAlignment = 16;
constexpr size_t kMethodArenaBytes = 64 * 1024;
constexpr size_t kTempArenaBytes = 64 * 1024;
constexpr size_t kInputBytes = WW_FRONTEND_N_MELS * sizeof(float);
constexpr size_t kOutputBytes = 3 * sizeof(float);

} // namespace

struct ww_runner {
    ww_config_t cfg;
    ww_frontend_t *frontend = nullptr;
    ww_detector_t detector;
    et_model_t *model = nullptr;
    uint8_t *pte_copy = nullptr;   /* owned aligned .pte copy, if the embed was misaligned */
    /* Method memory, allocated once at create and handed to et_model as
     * caller buffers: a reload (reset) reuses it, so reset never frees
     * and reallocates ~164 KB, and cannot fail for lack of memory. */
    uint8_t *arena = nullptr;
    uint8_t *temp_arena = nullptr;
    uint8_t *planned = nullptr;
    std::mutex mutex;
    bool listening = true;
    bool suppressed = false;
    bool ready = false;
    bool fired = false;
    bool error_logged = false;
    /* Bring-up timing telemetry: per-window frontend + inference cost.
     * push_t0_us is the esp_timer time feed() started the push whose
     * first frame the callback is about to infer (-1: none pending);
     * with 320-sample pushes that is normally the frame's only
     * frontend cost (FFT + mel + normalization). */
    uint64_t push_us = 0;
    uint64_t infer_us = 0;
    uint64_t frontend_us = 0;
    uint32_t infer_us_max = 0;
    uint32_t frontend_us_max = 0;
    uint32_t infer_count = 0;
    int64_t push_t0_us = -1;
};

/* Allocator high-water marks, as ESP-IDF reports them: the minimum
 * free size ever seen for each heap. Logged once when the runner is
 * ready (arenas, planned buffers, and the wake rings are allocated
 * by then) and again with every timing window, so the worst case
 * after Wi-Fi/tunnel come up is captured too. */
static void ww_log_heap_hwm()
{
    const size_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        ESP_LOGI(kTag, "heap hwm: internal min-free %u B (free %u B), spiram none",
                 (unsigned)internal_min, (unsigned)internal_free);
        return;
    }
    ESP_LOGI(kTag, "heap hwm: internal min-free %u B (free %u B), spiram min-free %u B (free %u B)",
             (unsigned)internal_min, (unsigned)internal_free,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void ww_runner_et_reset_locked(ww_runner_t *runner, bool reload_model)
{
    ww_frontend_reset(runner->frontend);
    ww_detector_reset(&runner->detector);
    runner->fired = false;
    if (reload_model && runner->model != nullptr) {
        /* Reloading into the caller buffers zeroes the model's streaming
         * state (et_model clears planned memory on every load) and
         * recovers a runner an inference failure stopped. */
        runner->ready = et_model_load_method(runner->model, "forward");
        if (runner->ready) {
            runner->error_logged = false;   /* a later failure logs again */
        } else if (!runner->error_logged) {
            ESP_LOGE(kTag, "model reload failed: %s", et_model_last_error(runner->model));
            runner->error_logged = true;
        }
    }
}

/* Method memory is the runner's, not et_model's: freed after the model. */
static void ww_runner_et_free_buffers(ww_runner_t *runner)
{
    heap_caps_free(runner->planned);
    heap_caps_free(runner->temp_arena);
    heap_caps_free(runner->arena);
    runner->planned = runner->temp_arena = runner->arena = nullptr;
}

static uint8_t *ww_runner_et_alloc(size_t bytes)
{
    /* What ET_MEM_DEFAULT asks of et_model: PSRAM for large blocks
     * where present, internal RAM otherwise. 16-aligned, as required
     * for caller buffers. */
    return static_cast<uint8_t *>(
        heap_caps_aligned_alloc(kPteAlignment, bytes == 0 ? 1 : bytes, MALLOC_CAP_DEFAULT));
}

extern "C" void ww_runner_et_frame_cb(void *ctx, const float frame[WW_FRONTEND_N_MELS])
{
    auto *runner = static_cast<ww_runner_t *>(ctx);
    if (runner == nullptr || !runner->ready || runner->model == nullptr) {
        if (runner != nullptr) {
            runner->push_t0_us = -1;
        }
        return;
    }

    const int64_t infer_t0 = esp_timer_get_time();
    if (runner->push_t0_us >= 0) {
        const uint32_t frontend_dt = (uint32_t)(infer_t0 - runner->push_t0_us);
        runner->frontend_us += frontend_dt;
        if (frontend_dt > runner->frontend_us_max) {
            runner->frontend_us_max = frontend_dt;
        }
        runner->push_t0_us = -1;
    }
    if (!et_model_set_input(runner->model, 0, frame, kInputBytes) ||
        !et_model_execute(runner->model)) {
        runner->ready = false;
        if (!runner->error_logged) {
            ESP_LOGE(kTag, "inference failed: %s", et_model_last_error(runner->model));
            runner->error_logged = true;
        }
        return;
    }

    size_t out_bytes = 0;
    const void *out = et_model_output_data(runner->model, 0, &out_bytes);
    if (out == nullptr || out_bytes != kOutputBytes) {
        runner->ready = false;
        if (!runner->error_logged) {
            ESP_LOGE(kTag, "unexpected model output (%zu bytes)", out_bytes);
            runner->error_logged = true;
        }
        return;
    }

    const uint32_t infer_dt = (uint32_t)(esp_timer_get_time() - infer_t0);
    runner->infer_us += infer_dt;
    if (infer_dt > runner->infer_us_max) {
        runner->infer_us_max = infer_dt;
    }
    if (++runner->infer_count >= 250) {   /* 5 s of audio per window */
        ESP_LOGI(kTag,
                 "timing: %lu infer, avg %.2f ms, max %.2f ms; frontend avg %.2f ms, "
                 "max %.2f ms; push (frontend+infer) avg %.2f ms; stack free %u B",
                 (unsigned long)runner->infer_count,
                 (double)runner->infer_us / runner->infer_count / 1000.0,
                 (double)runner->infer_us_max / 1000.0,
                 (double)runner->frontend_us / runner->infer_count / 1000.0,
                 (double)runner->frontend_us_max / 1000.0,
                 (double)runner->push_us / runner->infer_count / 1000.0,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        ww_log_heap_hwm();
        runner->infer_us = 0;
        runner->infer_us_max = 0;
        runner->frontend_us = 0;
        runner->frontend_us_max = 0;
        runner->push_us = 0;
        runner->infer_count = 0;
    }

    float posterior = static_cast<const float *>(out)[0];
    if (!std::isfinite(posterior)) {
        posterior = 0.0f;
    }
    if (posterior < 0.0f) {
        posterior = 0.0f;
    } else if (posterior > 1.0f) {
        posterior = 1.0f;
    }

    if (ww_detector_push(&runner->detector, posterior)) {
        runner->fired = true;
        ESP_LOGI(kTag, "wake posterior %.3f", (double)posterior);
    }
}

extern "C" {

ww_runner_t *ww_runner_create(const ww_config_t *cfg)
{
    ww_config_t effective = {};
    if (cfg != nullptr) {
        effective = *cfg;
    } else {
        effective.sample_rate_hz = WW_FRONTEND_SAMPLE_RATE_HZ;
        effective.sensitivity = WW_SENSITIVITY_DEFAULT;
        effective.vad = WW_VAD_ENERGY;
    }

    if (effective.sample_rate_hz != WW_FRONTEND_SAMPLE_RATE_HZ) {
        ESP_LOGE(kTag, "unsupported sample rate %lu (need %d)",
                 (unsigned long)effective.sample_rate_hz, WW_FRONTEND_SAMPLE_RATE_HZ);
        return nullptr;
    }
    if (effective.vad != WW_VAD_ENERGY) {
        ESP_LOGE(kTag, "Silero VAD is not implemented in this runner");
        return nullptr;
    }

    const uint8_t *pte = effective.model;
    size_t pte_len = effective.model_len;
    if (pte == nullptr || pte_len == 0) {
        pte = _binary_hey_muse_pte_start;
        pte_len = (size_t)(_binary_hey_muse_pte_end - _binary_hey_muse_pte_start);
    }

    /* EMBED_FILES does not guarantee the alignment Program::load
     * requires, and rodata layout shifts between builds: copy a
     * misaligned .pte into owned aligned storage (et_model keeps
     * pointing into it, so the copy lives as long as the runner). */
    uint8_t *pte_copy = nullptr;
    if ((reinterpret_cast<uintptr_t>(pte) % kPteAlignment) != 0) {
        pte_copy = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(kPteAlignment, pte_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (pte_copy == nullptr) {
            pte_copy = static_cast<uint8_t *>(
                heap_caps_aligned_alloc(kPteAlignment, pte_len, MALLOC_CAP_DEFAULT));
        }
        if (pte_copy == nullptr) {
            ESP_LOGE(kTag, "no memory for aligned pte copy (%zu B)", pte_len);
            return nullptr;
        }
        memcpy(pte_copy, pte, pte_len);
        ESP_LOGW(kTag, "pte at %p is not %zu-aligned; copied %zu B", (const void *)pte,
                 kPteAlignment, pte_len);
        pte = pte_copy;
    }

    auto *runner = new (std::nothrow) ww_runner_t();
    if (runner == nullptr) {
        free(pte_copy);
        return nullptr;
    }
    runner->cfg = effective;
    runner->pte_copy = pte_copy;
    runner->frontend = ww_frontend_create();
    ww_detector_init(&runner->detector, effective.sensitivity);
    if (runner->frontend == nullptr) {
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    et_model_config_t model_cfg = ET_MODEL_CONFIG_DEFAULT(pte, pte_len);
    model_cfg.arena_size = kMethodArenaBytes;
    model_cfg.arena_location = ET_MEM_DEFAULT;
    model_cfg.temp_arena_size = kTempArenaBytes;
    model_cfg.temp_location = ET_MEM_DEFAULT;
    /* Size the planned buffers without loading the method: creating a
     * model only checks the program header (the .pte stays in flash). */
    size_t planned_total = 0;
    et_model_t *probe = et_model_create(&model_cfg);
    const bool sized = probe != nullptr &&
                       et_model_planned_info(probe, "forward", nullptr, &planned_total);
    et_model_destroy(probe);
    if (sized) {
        runner->arena = ww_runner_et_alloc(kMethodArenaBytes);
        runner->temp_arena = ww_runner_et_alloc(kTempArenaBytes);
        runner->planned = ww_runner_et_alloc(planned_total);
    }
    if (!sized || runner->arena == nullptr || runner->temp_arena == nullptr ||
        runner->planned == nullptr) {
        if (sized) {
            ESP_LOGE(kTag, "no memory for the model (%zu B planned)", planned_total);
        } else {
            ESP_LOGE(kTag, "cannot size the model");
        }
        ww_runner_et_free_buffers(runner);
        ww_frontend_destroy(runner->frontend);
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }
    model_cfg.arena_buffer = runner->arena;
    model_cfg.temp_buffer = runner->temp_arena;
    model_cfg.planned_buffer = runner->planned;
    model_cfg.planned_buffer_size = planned_total;
    /* Planned buffers (29.5 KB for Hey Muse) are not internal-only: in the
     * full firmware internal RAM is nearly exhausted once Wi-Fi/BLE are
     * up (~12 KiB free, 4 KiB largest block on BOX-3). Like the arenas
     * they come from ww_runner_et_alloc() (PSRAM where present) once, at
     * create, so a later reload never allocates. Revisit per board if
     * timing data says the activations belong in internal RAM. */
    runner->model = et_model_create(&model_cfg);
    if (runner->model == nullptr) {
        ESP_LOGE(kTag, "et_model_create failed");
        ww_runner_et_free_buffers(runner);
        ww_frontend_destroy(runner->frontend);
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    runner->ready = et_model_load_method(runner->model, "forward");
    et_tensor_info_t input_info = {};
    et_tensor_info_t output_info = {};
    if (!runner->ready || et_model_num_inputs(runner->model) != 1 ||
        et_model_num_outputs(runner->model) != 1 ||
        !et_model_input_info(runner->model, 0, &input_info) ||
        !et_model_output_info(runner->model, 0, &output_info) || input_info.nbytes != kInputBytes ||
        output_info.nbytes != kOutputBytes) {
        ESP_LOGE(kTag, "model contract mismatch: %s", et_model_last_error(runner->model));
        et_model_destroy(runner->model);
        ww_runner_et_free_buffers(runner);
        ww_frontend_destroy(runner->frontend);
        free(runner->pte_copy);
        delete runner;
        return nullptr;
    }

    size_t planned_count = 0;
    size_t planned_bytes = 0;
    et_model_planned_info(runner->model, NULL, &planned_count, &planned_bytes);
    ESP_LOGI(kTag,
             "Hey Muse runner ready: pte=%zu B planned=%zu B in %zu buffer(s) "
             "arena=%zu B",
             pte_len, planned_bytes, planned_count, kMethodArenaBytes);
    ww_log_heap_hwm();
    return runner;
}

void ww_runner_destroy(ww_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    et_model_destroy(runner->model);
    ww_runner_et_free_buffers(runner);
    ww_frontend_destroy(runner->frontend);
    free(runner->pte_copy);
    delete runner;
}

void ww_runner_reset(ww_runner_t *runner)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    ww_runner_et_reset_locked(runner, true);
}

ww_event_t ww_runner_feed(ww_runner_t *runner, const int16_t *pcm, size_t frames)
{
    if (runner == nullptr || pcm == nullptr) {
        return WW_EVENT_NONE;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (!runner->ready || !runner->listening || runner->suppressed) {
        return WW_EVENT_NONE;
    }

    runner->fired = false;
    const int64_t push_t0 = esp_timer_get_time();
    runner->push_t0_us = push_t0;
    ww_frontend_push(runner->frontend, pcm, frames, ww_runner_et_frame_cb, runner);
    /* Includes the per-frame inference the callback runs; the callback
     * also splits out the frontend cost of the push's first frame. */
    runner->push_us += (uint64_t)(esp_timer_get_time() - push_t0);
    return runner->fired ? WW_EVENT_WAKE : WW_EVENT_NONE;
}

bool ww_runner_ok(ww_runner_t *runner)
{
    if (runner == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    return runner->ready;
}

void ww_runner_set_listening(ww_runner_t *runner, bool on)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (runner->listening != on) {
        runner->listening = on;
        if (!on) {
            ww_runner_et_reset_locked(runner, false);
        }
    }
}

void ww_runner_set_suppressed(ww_runner_t *runner, bool on)
{
    if (runner == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(runner->mutex);
    if (runner->suppressed != on) {
        runner->suppressed = on;
        if (on) {
            ww_runner_et_reset_locked(runner, false);
        }
    }
}

} // extern "C"
