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
 * The Hey Muse runner: microfrontend features (ww_frontend.c), the int8
 * streaming model models/hey_muse.pte through ExecuTorch's ESP-NN
 * delegate, and the detector (ww_detector.c). The same file builds into
 * the ESP32 firmware (ESP-IDF's registry components pytorch/executorch and
 * esphome/esp-micro-speech-features) and, against the same components, off
 * the chip (tests/test_muse_wakeword.py), so both run identical code.
 *
 * The app drives ExecuTorch's Method API directly, with no copies:
 * - the .pte is exported with its graph inputs and outputs left out of
 *   ExecuTorch's memory plan, so each is bound once, at load, to a buffer
 *   here: the frontend quantizes straight into the input, the probability
 *   lands in a byte, and each streaming state's input and output share
 *   one buffer, updated in place. A step is one Method::execute().
 *   In-place state is safe because the ESP-NN delegate copies every input
 *   into its own arena before it writes any output.
 * - the method arena is sized exactly at create, by loading once into a
 *   counting allocator; the program needs no temp allocator.
 * The program is a single delegate call with no CPU operators, so no
 * kernel library is linked.
 */

#include "ww_runner.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>

#include <executorch/extension/data_loader/buffer_data_loader.h>
#include <executorch/runtime/core/hierarchical_allocator.h>
#include <executorch/runtime/core/memory_allocator.h>
#include <executorch/runtime/executor/method.h>
#include <executorch/runtime/executor/method_meta.h>
#include <executorch/runtime/executor/program.h>
#include <executorch/runtime/platform/runtime.h>

#include "ww_detector.h"
#include "ww_frontend.h"
#include "ww_model.h"

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#else
#include <cstdio>
#endif

/* models/hey_muse.pte, embedded 16-aligned by hey_muse_pte.S.in. */
extern "C" const uint8_t _binary_hey_muse_pte_start[];
extern "C" const uint8_t _binary_hey_muse_pte_end[];

using executorch::aten::ScalarType;
using executorch::aten::Tensor;
using executorch::aten::TensorImpl;
using executorch::extension::BufferDataLoader;
using executorch::runtime::Error;
using executorch::runtime::EValue;
using executorch::runtime::HierarchicalAllocator;
using executorch::runtime::MemoryAllocator;
using executorch::runtime::MemoryManager;
using executorch::runtime::Method;
using executorch::runtime::MethodMeta;
using executorch::runtime::Program;
using executorch::runtime::Span;
using executorch::runtime::TensorInfo;

namespace
{

constexpr size_t kAlign = 16;
constexpr size_t kMaxDims = 4;
/* Generous enough for the probe load on a 64-bit host; the real arena is
 * then exactly what the method used (1,392 B on the ESP32-S3). */
constexpr size_t kProbeArenaBytes = 16 * 1024;
constexpr size_t kMaxPlanned = 2;
constexpr uint64_t kMaxPlannedBytes = 256 * 1024;   /* per buffer; this model plans a few KB */

#ifdef ESP_PLATFORM
const char *kTag = "ww";
#define WW_LOGE(...) ESP_LOGE(kTag, __VA_ARGS__)
#define WW_LOGI(...) ESP_LOGI(kTag, __VA_ARGS__)
/* Internal RAM when it can spare it, as the ESP-NN delegate decides for its
 * own buffers (the same 48 KB kept free for Wi-Fi, BLE and TLS), else PSRAM;
 * boards without PSRAM take internal RAM. */
constexpr size_t kInternalReserve = 48 * 1024;
void *ww_alloc(size_t bytes)
{
    bytes = bytes ? bytes : 1;
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_largest_free_block(internal) > bytes + kInternalReserve) {
        if (void *p = heap_caps_aligned_alloc(kAlign, bytes, internal)) {
            return p;
        }
    }
    void *p = heap_caps_aligned_alloc(kAlign, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_aligned_alloc(kAlign, bytes, internal);
}
void ww_free(void *p) { heap_caps_free(p); }
#else
#define WW_LOGE(...) (std::fprintf(stderr, "hey-muse: " __VA_ARGS__), std::fputc('\n', stderr))
#define WW_LOGI(...) ((void)0)
void *ww_alloc(size_t bytes)
{
    return std::aligned_alloc(kAlign, bytes ? (bytes + kAlign - 1) & ~(kAlign - 1) : kAlign);
}
void ww_free(void *p) { std::free(p); }
#endif

size_t round16(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

/* A bump allocator that remembers how much it handed out. */
class CountingAllocator : public MemoryAllocator
{
public:
    CountingAllocator(uint32_t size, uint8_t *base) : MemoryAllocator(size, base), base_(base) {}
    void *allocate(size_t size, size_t alignment = kDefaultAlignment) override
    {
        void *p = MemoryAllocator::allocate(size, alignment);
        if (p != nullptr) {
            const size_t end = (size_t)(static_cast<uint8_t *>(p) - base_) + size;
            used_ = end > used_ ? end : used_;
        }
        return p;
    }
    size_t used() const { return used_; }

private:
    uint8_t *base_;
    size_t used_ = 0;
};

struct BoundTensor {
    TensorImpl::SizesType sizes[kMaxDims];
    TensorImpl::DimOrderType dim_order[kMaxDims];
    TensorImpl::StridesType strides[kMaxDims];
    alignas(TensorImpl) uint8_t impl[sizeof(TensorImpl)];
};

} // namespace

struct ww_runner {
    ww_config_t cfg = {};
    ww_frontend_t *frontend = nullptr;
    ww_detector_t detector = {};

    const uint8_t *pte = nullptr;
    size_t pte_len = 0;
    uint8_t *pte_copy = nullptr;   /* a misaligned caller-supplied model, copied */
    std::optional<BufferDataLoader> loader;
    std::optional<Program> program;
    uint8_t *arena = nullptr;
    size_t arena_bytes = 0;
    uint8_t *planned_mem = nullptr;
    Span<uint8_t> planned_spans[kMaxPlanned];
    size_t planned_count = 0;
    std::optional<MemoryAllocator> allocator;
    std::optional<HierarchicalAllocator> planned;
    std::optional<MemoryManager> memory;
    std::optional<Method> method;

    /* One block: the input (120 B), the probability (16 B slot), the states. */
    uint8_t *io = nullptr;
    int8_t *x = nullptr;
    int8_t *p = nullptr;
    int8_t *state = nullptr;
    BoundTensor tensors[1 + WW_MODEL_STATES];

    void (*score_hook)(void *, float) = nullptr;
    void *score_ctx = nullptr;
    int pending = 0;   /* feature frames already in x */
    bool listening = true;
    bool suppressed = false;
    bool ready = false;
    bool fired = false;
    bool error_logged = false;

#ifdef ESP_PLATFORM
    /* Telemetry: per-inference cost, logged after the first 15 s of
     * listening and then every 10 minutes. */
    uint64_t infer_us = 0;
    uint32_t infer_us_max = 0;
    uint64_t feed_us = 0;
    uint64_t fed_frames = 0;
    uint32_t window = 0;
    uint32_t windows = 0;
#endif
};

namespace
{

void fail_once(ww_runner_t *r, const char *what, int code)
{
    r->ready = false;
    if (!r->error_logged) {
        WW_LOGE("%s failed (0x%x)", what, code);
        r->error_logged = true;
    }
}

void unload(ww_runner_t *r)
{
    r->method.reset();
    r->memory.reset();
    r->planned.reset();
    r->allocator.reset();
}

/* Checks a tensor against the contract (int8, exactly the shape `want`,
 * a dim order that is a permutation) and makes an app-owned TensorImpl over
 * data with its shape and dim order. */
TensorImpl *bind_tensor(BoundTensor &t, const TensorInfo &info, void *data, const int32_t (&want)[kMaxDims])
{
    const size_t dims = info.sizes().size();
    if (info.scalar_type() != ScalarType::Char || dims != kMaxDims || info.dim_order().size() != dims) {
        return nullptr;
    }
    size_t elements = 1;
    bool seen[kMaxDims] = {};
    for (size_t d = 0; d < dims; d++) {
        const size_t order = info.dim_order()[d];
        if (info.sizes()[d] != want[d] || order >= dims || seen[order]) {
            return nullptr;
        }
        seen[order] = true;
        t.sizes[d] = info.sizes()[d];
        t.dim_order[d] = info.dim_order()[d];
        elements *= (size_t)want[d];   /* the contract's shapes are small: no overflow */
    }
    if (info.nbytes() != elements) {
        return nullptr;
    }
    /* Strides follow the dim order (outermost first). */
    TensorImpl::StridesType stride = 1;
    for (size_t k = dims; k-- > 0;) {
        const size_t d = t.dim_order[k];
        t.strides[d] = stride;
        stride *= t.sizes[d];
    }
    return new (t.impl) TensorImpl(ScalarType::Char, (ssize_t)dims, t.sizes, data, t.dim_order,
                                   t.strides);
}

/* Loads the method into arena (arena_bytes long); with probe set, into a
 * counting allocator whose high-water mark comes back in *used. */
bool load_method(ww_runner_t *r, uint8_t *arena, size_t arena_bytes, size_t *used)
{
    unload(r);
    std::optional<CountingAllocator> counting;
    MemoryAllocator *allocator;
    if (used != nullptr) {
        counting.emplace((uint32_t)arena_bytes, arena);
        allocator = &*counting;
    } else {
        r->allocator.emplace((uint32_t)arena_bytes, arena);
        allocator = &*r->allocator;
    }
    for (size_t i = 0; i < r->planned_count; i++) {
        if (r->planned_spans[i].size() > 0) {
            memset(r->planned_spans[i].data(), 0, r->planned_spans[i].size());
        }
    }
    r->planned.emplace(Span<Span<uint8_t>>(r->planned_spans, r->planned_count));
    r->memory.emplace(allocator, &*r->planned, nullptr);
    auto method = r->program->load_method("forward", &*r->memory);
    if (!method.ok()) {
        fail_once(r, "load_method", (int)method.error());
        unload(r);
        return false;
    }
    r->method.emplace(std::move(method.get()));
    if (used != nullptr) {
        *used = counting->used();
        r->method.reset();   /* it points into the counting allocator */
        r->memory.reset();
    }
    return true;
}

/* Binds x, p and the states once; see the top of this file. */
bool bind_io(ww_runner_t *r)
{
    Method &m = *r->method;
    const MethodMeta meta = m.method_meta();
    if (m.inputs_size() != 1 + WW_MODEL_STATES || m.outputs_size() != 1 + WW_MODEL_STATES) {
        return false;
    }
    auto x_info = meta.input_tensor_meta(0);
    auto p_info = meta.output_tensor_meta(0);
    if (!x_info.ok() || !p_info.ok() || p_info->scalar_type() != ScalarType::Char ||
        p_info->nbytes() != 1) {
        return false;
    }
    static constexpr int32_t kInputShape[kMaxDims] = { 1, WW_MODEL_FEATURES, WW_MODEL_STRIDE, 1 };
    const auto p_sizes = p_info->sizes();
    if (p_sizes.size() != 2 || p_sizes[0] != 1 || p_sizes[1] != 1) {
        return false;
    }
    TensorImpl *x = bind_tensor(r->tensors[0], x_info.get(), r->x, kInputShape);
    if (x == nullptr || m.set_input(EValue(Tensor(x)), 0) != Error::Ok ||
        m.set_output_data_ptr(r->p, 1, 0) != Error::Ok) {
        return false;
    }
    static constexpr int32_t kChannels[WW_MODEL_STATES] = WW_MODEL_STATE_CHANNELS;
    static constexpr int32_t kLengths[WW_MODEL_STATES] = WW_MODEL_STATE_LENGTHS;
    size_t off = 0;
    for (size_t k = 0; k < WW_MODEL_STATES; k++) {
        auto in = meta.input_tensor_meta(1 + k);
        auto out = meta.output_tensor_meta(1 + k);
        if (!in.ok() || !out.ok() || out->nbytes() != in->nbytes() ||
            out->scalar_type() != ScalarType::Char || in->nbytes() > WW_MODEL_STATE_BYTES - off) {
            return false;
        }
        const int32_t shape[kMaxDims] = { 1, kChannels[k], kLengths[k], 1 };
        int8_t *buf = r->state + off;
        TensorImpl *h = bind_tensor(r->tensors[1 + k], in.get(), buf, shape);
        if (h == nullptr || m.set_input(EValue(Tensor(h)), 1 + k) != Error::Ok ||
            m.set_output_data_ptr(buf, out->nbytes(), 1 + k) != Error::Ok) {
            return false;
        }
        off += in->nbytes();
    }
    return off == WW_MODEL_STATE_BYTES;
}

void clear_stream(ww_runner_t *r)
{
    ww_frontend_reset(r->frontend);
    ww_detector_reset(&r->detector);
    memset(r->state, WW_MODEL_STATE_ZERO_POINT, WW_MODEL_STATE_BYTES);
    memset(r->x, WW_MODEL_INPUT_ZERO_POINT, WW_MODEL_INPUT_BYTES);
    r->pending = 0;
    r->fired = false;
}

/* Reloads and rebinds after a failure; the model's state lives here, so a
 * healthy runner never needs this. */
bool recover(ww_runner_t *r)
{
    if (!load_method(r, r->arena, r->arena_bytes, nullptr) || !bind_io(r)) {
        unload(r);
        return false;
    }
    r->ready = true;
    r->error_logged = false;
    return true;
}

#ifdef ESP_PLATFORM
void log_heap()
{
    ESP_LOGI(kTag, "heap: internal min-free %u B (free %u B), spiram min-free %u B",
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}
#endif

void on_features(void *ctx, const int8_t features[WW_MODEL_FEATURES])
{
    auto *r = static_cast<ww_runner_t *>(ctx);
    if (!r->ready) {
        return;
    }
    for (int c = 0; c < WW_MODEL_FEATURES; c++) {
        r->x[c * WW_MODEL_STRIDE + r->pending] = features[c];
    }
    if (++r->pending < WW_MODEL_STRIDE) {
        return;
    }
    r->pending = 0;
#ifdef ESP_PLATFORM
    const int64_t t0 = esp_timer_get_time();
#endif
    const Error err = r->method->execute();
    if (err != Error::Ok) {
        fail_once(r, "inference", (int)err);
        return;
    }
#ifdef ESP_PLATFORM
    const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    r->infer_us += dt;
    r->infer_us_max = dt > r->infer_us_max ? dt : r->infer_us_max;
    if (++r->window >= (r->windows == 0 ? 500u : 20000u)) {   /* 15 s, then 10 min */
        ESP_LOGI(kTag, "%lu inferences: avg %lu us, max %lu us; feed (front end + inference) avg "
                       "%lu us per 30 ms",
                 (unsigned long)r->window, (unsigned long)(r->infer_us / r->window),
                 (unsigned long)r->infer_us_max,
                 (unsigned long)(r->fed_frames ? r->feed_us * 480 / r->fed_frames : 0));
        log_heap();
        r->windows++;
        r->window = 0;
        r->infer_us = r->feed_us = r->fed_frames = 0;
        r->infer_us_max = 0;
    }
#endif
    const float probability = (float)(*r->p - WW_MODEL_OUTPUT_ZERO_POINT) / 256.0f;
    if (r->score_hook != nullptr) {
        r->score_hook(r->score_ctx, probability);
    }
    if (ww_detector_push(&r->detector, probability)) {
        r->fired = true;
        WW_LOGI("wake (window mean over %.2f)", (double)r->detector.cutoff);
    }
}

void destroy(ww_runner_t *r)
{
    unload(r);
    r->program.reset();
    r->loader.reset();
    ww_free(r->arena);
    ww_free(r->planned_mem);
    ww_free(r->io);
    ww_free(r->pte_copy);
    ww_frontend_destroy(r->frontend);
    r->~ww_runner();
    ww_free(r);
}

} // namespace

extern "C" {

ww_runner_t *ww_runner_create(const ww_config_t *cfg)
{
    ww_config_t effective = {};
    if (cfg != nullptr) {
        effective = *cfg;
    } else {
        effective.sample_rate_hz = 16000;
        effective.sensitivity = WW_SENSITIVITY_DEFAULT;
        effective.vad = WW_VAD_ENERGY;
    }
    if (effective.sample_rate_hz != 16000) {
        WW_LOGE("unsupported sample rate %lu (need 16000)", (unsigned long)effective.sample_rate_hz);
        return nullptr;
    }
    if (effective.vad != WW_VAD_ENERGY) {
        WW_LOGE("no VAD model in this runner");
        return nullptr;
    }

    void *mem = ww_alloc(sizeof(ww_runner_t));
    if (mem == nullptr) {
        return nullptr;
    }
    auto *r = new (mem) ww_runner_t();
    r->cfg = effective;
    ww_detector_init(&r->detector, effective.sensitivity);

    r->pte = effective.model;
    r->pte_len = effective.model_len;
    if (r->pte == nullptr || r->pte_len == 0) {
        r->pte = _binary_hey_muse_pte_start;
        r->pte_len = (size_t)(_binary_hey_muse_pte_end - _binary_hey_muse_pte_start);
    }
    /* Program::load needs the program aligned to alignof(max_align_t): 8
     * on the ESP32, 16 on x86-64 hosts. 16 covers both. */
    if ((reinterpret_cast<uintptr_t>(r->pte) % kAlign) != 0) {
#ifdef ESP_PLATFORM
        r->pte_copy = static_cast<uint8_t *>(heap_caps_aligned_alloc(kAlign, r->pte_len, MALLOC_CAP_DEFAULT));
#else
        r->pte_copy = static_cast<uint8_t *>(ww_alloc(r->pte_len));
#endif
        if (r->pte_copy == nullptr) {
            destroy(r);
            return nullptr;
        }
        memcpy(r->pte_copy, r->pte, r->pte_len);
        r->pte = r->pte_copy;
    }

    executorch::runtime::runtime_init();
    r->frontend = ww_frontend_create();
    r->io = static_cast<uint8_t *>(ww_alloc(round16(WW_MODEL_INPUT_BYTES) + kAlign + WW_MODEL_STATE_BYTES));
    if (r->frontend == nullptr || r->io == nullptr) {
        WW_LOGE("no memory for the front end");
        destroy(r);
        return nullptr;
    }
    r->x = reinterpret_cast<int8_t *>(r->io);
    r->p = reinterpret_cast<int8_t *>(r->io + round16(WW_MODEL_INPUT_BYTES));
    r->state = reinterpret_cast<int8_t *>(r->io + round16(WW_MODEL_INPUT_BYTES) + kAlign);

    r->loader.emplace(r->pte, r->pte_len);
    /* The embedded model is part of the firmware; a model from a file is
     * checked in full where the build includes ExecuTorch's verifier. */
    auto program = Program::load(&*r->loader, effective.model != nullptr ? Program::Verification::InternalConsistency
                                                                         : Program::Verification::Minimal);
    if (!program.ok()) {
        WW_LOGE("not a usable model (0x%x)", (unsigned)program.error());
        destroy(r);
        return nullptr;
    }
    r->program.emplace(std::move(program.get()));
    auto meta = r->program->method_meta("forward");
    if (!meta.ok() || meta->num_memory_planned_buffers() > kMaxPlanned) {
        WW_LOGE("model has no usable forward method");
        destroy(r);
        return nullptr;
    }
    /* The planned sizes come from the model file: bound each one (and so
     * their sum) before allocating, so a model built for something else
     * fails here rather than wrapping a size on a 32-bit host. */
    size_t planned_bytes = 0;
    r->planned_count = meta->num_memory_planned_buffers();
    for (size_t i = 0; i < r->planned_count; i++) {
        auto n = meta->memory_planned_buffer_size(i);
        if (!n.ok() || n.get() < 0 || (uint64_t)n.get() > kMaxPlannedBytes) {
            WW_LOGE("model plans more memory than a wake word needs");
            destroy(r);
            return nullptr;
        }
        planned_bytes += round16((size_t)n.get());
    }
    if (planned_bytes > 0) {
        r->planned_mem = static_cast<uint8_t *>(ww_alloc(planned_bytes));
        if (r->planned_mem == nullptr) {
            destroy(r);
            return nullptr;
        }
    }
    for (size_t i = 0, off = 0; i < r->planned_count; i++) {
        const size_t n = (size_t)meta->memory_planned_buffer_size(i).get();
        r->planned_spans[i] = Span<uint8_t>(r->planned_mem + off, n);
        off += round16(n);
    }

    /* Size the method arena exactly, then load for real. */
    size_t used = 0;
    auto *probe = static_cast<uint8_t *>(ww_alloc(kProbeArenaBytes));
    const bool sized = probe != nullptr && load_method(r, probe, kProbeArenaBytes, &used);
    ww_free(probe);
    r->arena_bytes = round16(used);
    r->arena = sized ? static_cast<uint8_t *>(ww_alloc(r->arena_bytes)) : nullptr;
    if (r->arena == nullptr || !load_method(r, r->arena, r->arena_bytes, nullptr) || !bind_io(r)) {
        WW_LOGE("model doesn't match the runner's contract");
        destroy(r);
        return nullptr;
    }
    /* One inference on silence proves the model runs (and that it needs
     * no temp allocator); then start clean. */
    clear_stream(r);
    const Error err = r->method->execute();
    if (err != Error::Ok) {
        WW_LOGE("first inference failed (0x%x)", (unsigned)err);
        destroy(r);
        return nullptr;
    }
    clear_stream(r);
    r->ready = true;
    WW_LOGI("Hey Muse runner ready: model %u B, method arena %u B, cutoff %.2f", (unsigned)r->pte_len,
            (unsigned)r->arena_bytes, (double)r->detector.cutoff);
#ifdef ESP_PLATFORM
    log_heap();
#endif
    return r;
}

void ww_runner_destroy(ww_runner_t *r)
{
    if (r != nullptr) {
        destroy(r);
    }
}

void ww_runner_reset(ww_runner_t *r)
{
    if (r == nullptr) {
        return;
    }
    clear_stream(r);
    if (!r->ready) {
        recover(r);
    }
}

ww_event_t ww_runner_feed(ww_runner_t *r, const int16_t *pcm, size_t frames)
{
    if (r == nullptr || pcm == nullptr || !r->ready || !r->listening || r->suppressed) {
        return WW_EVENT_NONE;
    }
    r->fired = false;
#ifdef ESP_PLATFORM
    const int64_t t0 = esp_timer_get_time();
#endif
    ww_frontend_push(r->frontend, pcm, frames, on_features, r);
#ifdef ESP_PLATFORM
    r->feed_us += (uint64_t)(esp_timer_get_time() - t0);
    r->fed_frames += frames;
#endif
    return r->fired ? WW_EVENT_WAKE : WW_EVENT_NONE;
}

bool ww_runner_ok(ww_runner_t *r)
{
    return r != nullptr && r->ready;
}

void ww_runner_set_score_hook(ww_runner_t *r, void (*fn)(void *ctx, float probability), void *ctx)
{
    if (r != nullptr) {
        r->score_hook = fn;
        r->score_ctx = ctx;
    }
}

void ww_runner_set_listening(ww_runner_t *r, bool on)
{
    if (r != nullptr && r->listening != on) {
        r->listening = on;
        if (!on) {
            clear_stream(r);
        }
    }
}

void ww_runner_set_suppressed(ww_runner_t *r, bool on)
{
    if (r != nullptr && r->suppressed != on) {
        r->suppressed = on;
        if (on) {
            clear_stream(r);
        }
    }
}

} // extern "C"
