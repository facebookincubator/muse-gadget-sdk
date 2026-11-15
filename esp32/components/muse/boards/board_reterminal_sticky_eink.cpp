/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#include <EInkDisplay.h>
#include <cstring>
#include <array>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "eink/muse_eink_board.h"

namespace {
struct StickySettling {
    uint8_t *cycles{}, *baseline{};
    size_t bytes{}, pending{};

    bool ready() const { return cycles && baseline && bytes; }
    void reset() {
        if (cycles) memset(cycles, 0, bytes * 8);
        pending = 0;
    }

    bool prepare(const uint8_t *previous, const uint8_t *target, unsigned drives) {
        bool more = false;
        pending = 0;
        for (size_t i = 0; i < bytes; ++i) {
            uint8_t changed = previous[i] ^ target[i], mask = 0;
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto &left = cycles[i * 8 + bit];
                if (changed & (1U << bit)) left = left ? drives - left : drives;
                if (left) { mask |= 1U << bit; ++pending; }
                if (left > 1) more = true;
            }
            baseline[i] = target[i] ^ mask;
        }
        return more;
    }

    void advance() {
        pending = 0;
        for (size_t i = 0; i < bytes * 8; ++i) {
            if (cycles[i]) --cycles[i];
            if (cycles[i]) ++pending;
        }
    }
};
// SSD1677 Rev1.0 Fig6-6: VS, TP/RP, FR, then 03h/04h/2Ch rails.
// https://files.waveshare.com/upload/2/2a/SSD1677_1.0.pdf
static constexpr auto s_fast_lut = [] {
    std::array<unsigned char, 110> data{};
    data[10] = 0x80; data[20] = 0x40;  // VS[0A-L1/L2]; other VS cells hold.
    data[50] = 16; data[51] = 1;       // TP[0A/B]; TP[0C/D] and RP[0] = 0.
    for (int i = 100; i < 105; ++i) data[i] = 0x88;  // FR group pairs.
    const unsigned char rails[] = {0x17, 0x41, 0xa8, 0x32, 0x30};  // VGH, VSH1/2, VSL, VCOM.
    for (int i = 0; i < 5; ++i) data[105 + i] = rails[i];
    return data;
}();
static const char *TAG = "sticky.eink";
static EInkDisplay *s_panel;
static uint8_t *s_presented_frame;
static bool s_presented_valid;
static int s_fast_count;
static StickySettling s_settling;
static constexpr int CLEAN_EVERY = 120;

static void sticky_begin(void *display)
{
    s_panel = static_cast<EInkDisplay *>(display);
    /* All retained state is optional; allocation failure uses factory refresh. */
    s_presented_frame = (uint8_t *)heap_caps_malloc(
        (size_t)s_panel->getDisplayWidthBytes() * s_panel->getDisplayHeight(), MALLOC_CAP_SPIRAM);
    if (!s_presented_frame) ESP_LOGW(TAG, "no PSRAM for presented frame; windowed refresh disabled");
    if (s_presented_frame) {
        s_settling.bytes = (size_t)s_panel->getDisplayWidthBytes() * s_panel->getDisplayHeight();
        s_settling.cycles = (uint8_t *)heap_caps_calloc(s_settling.bytes * 8, 1, MALLOC_CAP_SPIRAM);
        s_settling.baseline = (uint8_t *)heap_caps_malloc(s_settling.bytes, MALLOC_CAP_SPIRAM);
        if (!s_settling.ready()) {
            heap_caps_free(s_settling.cycles);
            heap_caps_free(s_settling.baseline);
            s_settling = {};
            ESP_LOGW(TAG, "no PSRAM for short-pulse settling; using factory refresh");
        }
    }
}

struct RefreshWindow {
    int x, y, w, h;
    bool changed, full;
};

/* Native panel coordinates: the drawing target already rotated the pixels.
 * Keep small changes inside their changed-byte window. */
static RefreshWindow refresh_window(const uint8_t *shown, const uint8_t *next, int row_bytes, int rows)
{
    const RefreshWindow full = {0, 0, row_bytes * 8, rows, true, true};
    if (!shown) return full;
    int left = row_bytes, right = -1, top = rows, bottom = -1;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < row_bytes; x++) {
            if (shown[y * row_bytes + x] == next[y * row_bytes + x]) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    if (right < 0) return {};
    RefreshWindow window = {left * 8, top, (right - left + 1) * 8, bottom - top + 1, true, false};
    if (window.w * window.h * 4 > full.w * full.h * 3) return full;
    return window;
}

static void sticky_present(bool force_clean)
{
    if (!s_settling.ready()) {
        // Match the original full-buffer FAST path and cleanup cadence when
        // optional retained state is unavailable, including duplicate calls.
        bool clean = force_clean || ++s_fast_count >= 30;
        s_panel->setCustomLUT(false);
        s_panel->displayBuffer(clean ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
        if (clean) s_fast_count = 0;
        return;
    }
    const int row_bytes = s_panel->getDisplayWidthBytes(), rows = s_panel->getDisplayHeight();
    const uint8_t *frame = s_panel->getFrameBuffer();
    bool snapshot_copied = false;
    bool clean = force_clean;
    RefreshWindow window = refresh_window(s_presented_valid ? s_presented_frame : nullptr,
                                          frame, row_bytes, rows);
    bool changed = window.changed;
    if (!clean && !changed && !s_settling.pending) return;
    clean = clean || (changed && ++s_fast_count >= CLEAN_EVERY);
    int64_t t = esp_timer_get_time();
    clean = clean || !s_presented_valid;
    bool turn_off = false;
    if (clean) {
        s_settling.reset();
    } else {
        turn_off = !s_settling.prepare(s_presented_frame, frame, 5);
        window = refresh_window(s_settling.baseline, frame, row_bytes, rows);
        // The public single-buffer cleanup API seeds RED RAM, but also copies
        // its argument into the write buffer. Preserve and restore the target.
        memcpy(s_presented_frame, frame, (size_t)row_bytes * rows);
        snapshot_copied = true;
        s_panel->cleanupGrayscaleBuffers(s_settling.baseline);
        memcpy(s_panel->getFrameBuffer(), s_presented_frame, (size_t)row_bytes * rows);
    }
    s_panel->setCustomLUT(!clean, clean ? nullptr : s_fast_lut.data());
    if (!clean && !window.full) {
        s_panel->displayWindow(window.x, window.y, window.w, window.h, turn_off);
    } else {
        s_panel->displayBuffer(clean ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH, turn_off);
    }
    if (!clean) s_settling.advance();
    s_panel->setCustomLUT(false);  // later full/gray calls must load their OTP/LUT
    if (s_presented_frame && !snapshot_copied) {
        memcpy(s_presented_frame, frame, (size_t)row_bytes * rows);
        s_presented_valid = true;
    }
    ESP_LOGD(TAG, "%s refresh %d ms", clean ? "full" : "fast", (int)((esp_timer_get_time() - t) / 1000));
    if (clean) s_fast_count = 0;
}

static void sticky_invalidate()
{
    s_presented_valid = false;
    s_settling.reset();
}

static bool sticky_pending() { return s_settling.pending != 0; }
}  // namespace

namespace muse_eink {
const BoardRendering *board_rendering()
{
    static const BoardRendering rendering = {sticky_begin, sticky_present, sticky_invalidate, sticky_pending};
    return &rendering;
}
}  // namespace muse_eink
