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
#include <assert.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "muse_eink_text.h"
using namespace muse_eink;

#define pdTRUE 1
#define portMAX_DELAY -1
#define MIC_PX 26
#define MIC_HIT_PAD 12
#define LONG_PRESS_MS 600
#define SWIPE_PX 80
#define METER_SEGS 14
#define METER_SEG_PX 9
#define METER_GAP_PX 4
#define CAPTION_W 256
#define CAPTION_LINE_SPACE 2
#define MALLOC_CAP_SPIRAM 0
using QueueHandle_t = int;
static int g_w = 8, g_h = 8, s_task = 1;
static std::atomic<bool> s_ready{true}, s_paused, s_dark, s_draw_now;
static std::atomic<bool> s_face_active{true}, s_image_shown, s_need_clean{true};
static std::atomic<uint32_t> s_navigation_epoch{}, s_presented_epoch{};
static int s_fast_count;
static int64_t now_us;
static std::vector<bool> ptt;
static int wakes, pokes, notifications;
static bool asleep, pairing;
struct Board { bool touch_talk; };
static Board board{true}, *muse_board = &board;
#define MUSE_LINK_CONFIRM 1
static int muse_link_state() { return pairing ? MUSE_LINK_CONFIRM : 0; }
static void muse_input_touch_talk(bool down) { ptt.push_back(down); }
static void muse_state_poke() { pokes++; }
static bool muse_state_asleep() { return asleep; }
static void muse_state_set_asleep(bool v) { asleep = v; wakes++; }
static void xTaskNotifyGive(int) { notifications++; }
static int64_t esp_timer_get_time() { return now_us; }
static void request_draw() { s_draw_now = true; }
static void settings_home() {}
static void settings_scroll(int) {}
static bool settings_typing() { return true; }
static bool settings_keyboard_changed() { return false; }
static bool settings_back() { return false; }
struct InputSnapshot {
    int16_t touchX{}, touchY{};
    bool touchReleased{}, longPress{};
};
struct App {
    std::vector<InputSnapshot> inputs;
    bool route(const InputSnapshot &in) { inputs.push_back(in); return true; }
};
static App app, *g_app = &app;
#include "eink_epoch.inc"
struct TouchEvent;
static int xQueueSend(QueueHandle_t, const TouchEvent *, int);
#include "eink_touch.inc"
static std::deque<TouchEvent> events;
static int xQueueSend(QueueHandle_t, const TouchEvent *e, int) { events.push_back(*e); return pdTRUE; }
static int xQueueReceive(QueueHandle_t, TouchEvent *e, int) {
    if (events.empty()) return 0;
    *e = events.front(); events.pop_front(); return pdTRUE;
}

static std::atomic<bool> paused_in_bitmap{}, continue_bitmap{};
static bool pause_bitmap;
static std::atomic<int> waiting_writers{};
template <typename Predicate>
static void wait_for_condition(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
struct Mutex { std::mutex m; };
using SemaphoreHandle_t = Mutex *;
static void xSemaphoreTake(Mutex *m, int) {
    if (!m->m.try_lock()) {
        waiting_writers++;
        m->m.lock();
    }
}
static void xSemaphoreGive(Mutex *m) { m->m.unlock(); }
static std::atomic<int> freed;
static bool fail_planes;
static void *heap_caps_malloc(size_t size, int) { return fail_planes ? nullptr : malloc(size); }
static void *heap_caps_calloc(size_t count, size_t size, int) { return calloc(count, size); }
static void heap_caps_free(void *p) { if (p) freed++; free(p); }
struct Rect { int16_t x, y, width, height; };
enum class Color { Black, White, DarkGray };
struct Paint { static Paint solid(Color) { return {}; } };
enum class BitmapFormat { BW1 };
enum class BitmapMode { Center };
struct BitmapRef { const uint8_t *data; uint16_t width, height; BitmapFormat format; bool progmem; };
namespace freeink { enum class GrayscaleMode { Absolute }; }
static std::vector<std::vector<uint8_t>> copied;
struct Target {
    void bitmap(Rect, BitmapRef ref, BitmapMode) {
        if (pause_bitmap && copied.empty()) {
            paused_in_bitmap = true;
            wait_for_condition([] { return continue_bitmap.load(); });
        }
        const size_t size = ((ref.width + 7) / 8) * ref.height;
        copied.emplace_back(ref.data, ref.data + size);
    }
    void fill(Rect, Paint) {}
    void stroke(Rect, Paint, int) {}
};
static Target target, *s_target = &target;
struct EInkDisplay {
    enum { FULL_REFRESH };
    uint8_t fb[8]{};
    bool grayscale = true;
    int refreshes = 0;
    uint8_t *getFrameBuffer() { return fb; }
    int getDisplayWidthBytes() { return 1; }
    int getDisplayHeight() { return 8; }
    bool displayGrayscaleBase(freeink::GrayscaleMode) { return grayscale; }
    void copyGrayscaleBuffers(uint8_t *, uint8_t *) {}
    void displayGrayBuffer() { refreshes++; }
    void displayBuffer(int) { refreshes++; }
};
static EInkDisplay s_display;
struct Read { int px = 4, y{}, w = 256, h = 32, top{}, lines = 2; };
static Read s_read;
static int s_canvas_px = 4, s_big_y, s_dy;
#define MUSE_MODE_LISTENING 1
#define MUSE_MODE_THINKING 2
struct Face {
    bool answering{}, mic = true;
    int ring{}, bar_x{}, mode{}, lit{};
    const char *state = "READY";
    char name[4]{}, caption[4]{};
};
static Face s_face;
using FontId = int;
#define FONT_UNSCII16 0
static int line_height(FontId) { return 16; }
static void draw_status_line(const Face &, int) {}
static void draw_art(int, int) {}
static void draw_mic(int, int, int) {}
static void draw_text_centered(int, int, FontId, const char *, Color, int = 0) {}
static void draw_wrapped(int, int, int, FontId, const char *, int, int, bool) {}
static void draw_pairing(const Face &) {}
struct Frame { void hit(Rect, int) {} };
struct Screen {
    Frame f;
    Frame &frame() { return f; }
};
static Screen screen;
static void settings_screen(Screen &) {}
static bool settings_in_subpage() { return true; }
static void draw_dots(bool) {}
#define ACTION_MUSE 1
#define ACTION_IMAGE 2
#include "eink_face.inc"
#include "eink_image.inc"
#include "eink_pages.inc"
#include "eink_handle_touch.inc"
#include "eink_done.inc"

static int text_width(FontId, const char *s) {
    int count = 0;
    for (size_t i = 0; s[i];) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        assert(c < 0x80 || c >= 0xC2);
        for (int j = 1; j < n; j++) assert(s[i+j] && ((unsigned char)s[i+j] & 0xC0) == 0x80);
        i += n; count++;
    }
    return count;
}
#define strlcat muse_test_strlcat
static size_t muse_test_strlcat(char *out, const char *src, size_t cap) {
    const size_t n = strlen(out), len = strlen(src);
    if (n < cap) {
        size_t copy = len < cap - n - 1 ? len : cap - n - 1;
        memcpy(out+n, src, copy); out[n+copy] = 0;
    }
    return n + len;
}
#include "eink_fit.inc"

static void init_image() {
    static Mutex mutex;
    s_image_mutex = &mutex;
    s_image_bits = static_cast<uint8_t *>(malloc(16));
    assert(s_image_bits);
    memset(s_image_bits, 0xAA, 8); memset(s_image_bits+8, 0xBB, 8);
    s_image_shown = true;
}
static void image_race(bool replacement) {
    init_image(); pause_bitmap = true;
    if (replacement) {
        s_image_buf = static_cast<uint16_t *>(calloc(64, sizeof(uint16_t)));
        assert(s_image_buf);
    }
    std::thread reader([] { assert(present_image()); });
    wait_for_condition([] { return paused_in_bitmap.load(); });
    std::thread writer([replacement] { if (replacement) muse_ui_image_done(); else image_hide(); });
    wait_for_condition([] { return waiting_writers.load() > 0; });
    assert(freed == 0);   // writer is blocked inside the real lifetime mutex
    continue_bitmap = true;
    reader.join(); writer.join();
    assert(copied.size() == 2);
    assert(copied[0] == std::vector<uint8_t>(8, 0xAA));
    assert(copied[1] == std::vector<uint8_t>(8, 0xBB));
    assert(s_display.refreshes == 1);
    image_hide();
}
static void paint_view() {
    root_screen(screen,nullptr);
    s_presented_epoch = s_navigation_epoch.load();
}
static void init_touch() {
    g_w = 480; g_h = 800;
    s_touch = {}; s_paused = false; s_dark = false; s_image_shown = false;
    s_face_active = true; s_mic_visible = false;
    paint_view(); assert(s_mic_visible);
}
static void tap(int x = 240, int y = 737) {
    touch_sample(true, x, y); now_us += 100000; touch_sample(false, x, y);
}
static void test_touch(const std::string &name) {
    init_touch();
    if (name == "touch_face") {
        tap(); assert(ptt == std::vector<bool>({true,false})); assert(events.empty());
    } else if (name == "touch_settings") {
        set_page(true); assert(!mic_available()); paint_view();
        tap(); handle_touch(); assert(ptt.empty()); assert(app.inputs.size() == 1);
        set_page(false); assert(!mic_available()); paint_view(); assert(mic_available());
    } else if (name == "touch_image") {
        s_image_shown = true; assert(!mic_available()); paint_view();
        tap(); handle_touch(); assert(ptt.empty()); assert(app.inputs.size() == 1);
    } else if (name == "touch_hold_modal") {
        touch_sample(true,240,737); set_page(true); touch_sample(true,240,737);
        touch_sample(false,240,737);
        assert(ptt == std::vector<bool>({true,false})); assert(events.empty());
    } else if (name == "touch_wake_pairing") {
        asleep = true; tap(); assert(wakes == 1); assert(ptt.empty()); assert(events.empty());
        pairing = true; tap(20,20); assert(ptt == std::vector<bool>({true,false}));
    } else if (name == "touch_longpress") {
        set_page(true); paint_view(); touch_sample(true,240,737); now_us += 650000;
        touch_sample(false,240,737); now_us += 3000000; handle_touch();
        assert(app.inputs.size() == 1 && app.inputs[0].longPress);
        touch_sample(true,240,737); now_us += 100000; touch_sample(false,240,737);
        now_us += 3000000; handle_touch();
        assert(app.inputs.size() == 2 && !app.inputs[1].longPress);
    } else assert(false);
}
static void test_utf8() {
    const char *words[] = {"ASCII", "éééé", "ЖЖЖЖ", "中文中文", "😀😀😀", "aéЖ文😀z"};
    for (const char *word : words) {
        for (size_t cap = 1; cap <= strlen(word)+5; cap++) {
            for (int w = 0; w < 12; w++) {
                char out[64]; memset(out,0x5A,sizeof(out));
                fit(out,cap,word,0,w); assert(strlen(out) < cap);
                text_width(0,out); assert(out[cap] == 0x5A);
            }
        }
    }
    char out[16]; fit(out,sizeof(out),"ééééé",0,5); assert(strcmp(out,"ééééé") == 0);
    fit(out,sizeof(out),"éééééé",0,5); assert(strcmp(out,"éé...") == 0);
    out[0] = 'X'; fit(out,0,"é",0,0); assert(out[0] == 'X');
}
int main(int argc, char **argv) {
    assert(argc == 2); const std::string name = argv[1];
    if (name == "image_hide") image_race(false);
    else if (name == "image_replace") image_race(true);
    else if (name == "image_empty") {
        init_image(); image_hide(); assert(!present_image()); assert(copied.empty());
    } else if (name == "image_allocation_failure") {
        init_image(); fail_planes = true; assert(present_image()); assert(s_display.refreshes == 1); image_hide();
    } else if (name.compare(0,6,"touch_") == 0) test_touch(name);
    else if (name == "utf8") test_utf8();
    else assert(false);
}
