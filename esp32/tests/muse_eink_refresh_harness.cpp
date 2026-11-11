/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * SPDX-License-Identifier: Apache-2.0 */
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "esp_heap_caps.h"
static int64_t esp_timer_get_time() { return 0; }
template <typename... T> static void log(const char *, const char *, T...) {}
#define ESP_LOGD log
#define ESP_LOGW log
struct EInkDisplay {
    enum { FULL_REFRESH, FAST_REFRESH };
    std::vector<uint8_t> fb = std::vector<uint8_t>(48000, 255), red = fb;
    int calls = 0, mode = -1, x = 0, y = 0, w = 0, h = 0, drive = 0;
    bool custom = false;
    uint8_t *getFrameBuffer() { return fb.data(); }
    int getDisplayWidthBytes() { return 100; }
    int getDisplayHeight() { return 480; }
    void setCustomLUT(bool on, const unsigned char * = nullptr) { custom = on; }
    void cleanupGrayscaleBuffers(const uint8_t *previous) {
        memcpy(red.data(), previous, red.size()); memcpy(fb.data(), previous, fb.size());
    }
    void displayBuffer(int refresh, bool = false) {
        ++calls; mode = refresh; red = fb; drive = 0;
    }
    void displayWindow(int left, int top, int width, int height, bool = false) {
        ++calls; x = left; y = top; w = width; h = height;
        if ((red[3] ^ fb[3]) & 1) drive += (fb[3] & 1) ? 1 : -1;
        for (int row = y; row < y + h; ++row)
            memcpy(red.data() + row * 100 + x / 8, fb.data() + row * 100 + x / 8, w / 8);
    }
};
#include "boards/board_reterminal_sticky_eink.cpp"
int main() {
    void *probe = heap_caps_malloc(1, 0);
    bool psram = probe != nullptr; heap_caps_free(probe);
    EInkDisplay panel; const auto *r = muse_eink::board_rendering();
    r->begin(&panel); r->present(true);
    if (!psram) {
        for (int i = 1; i <= 30; ++i) {
            panel.fb[3] ^= 1; r->present(false);
            assert(panel.mode == (i == 30 ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH));
            assert(panel.calls == i + 1 && !panel.custom && !r->pending());
        }
        return 0;
    }
    panel.fb[3] ^= 1; r->present(false);
    assert(panel.x == 24 && panel.y == 0 && panel.w == 8 && panel.h == 1);
    for (int i = 1; i < 5; ++i) { assert(r->pending()); r->present(false); }
    assert(!r->pending() && panel.calls == 6 && panel.drive == -5);
    panel.fb[3] ^= 1; r->present(false); r->present(false);
    assert(panel.drive == -3 && r->pending());
    panel.fb[3] ^= 1; r->present(false);
    assert(panel.drive == -4 && r->pending()); r->present(false);
    assert(panel.drive == -5 && !r->pending() && panel.calls == 10);
}
