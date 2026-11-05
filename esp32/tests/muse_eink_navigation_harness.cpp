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
#include <FreeInkApp.h>
#include <assert.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
using namespace freeink::ui;
using App = FreeInkApp<160, 96>;

struct Target final : DrawTarget {
    Size measureText(FontId, const char *text, TextStyle) const override {
        return {static_cast<int16_t>(strlen(text) * 8), 16};
    }
    int16_t lineHeight(FontId) const override { return 16; }
    void fill(Rect, Paint, uint8_t, uint8_t) override {}
    void stroke(Rect, Paint, uint8_t, uint8_t, uint8_t) override {}
    void line(Point, Point, uint8_t, Paint) override {}
    void triangle(Point, Point, Point, Paint) override {}
    void text(Rect, const char *, TextStyle) override {}
    void bitmap(Rect, BitmapRef, BitmapMode, Paint, Rotation) override {}
};
static App *g_app;
#define pdTRUE 1
#define SWIPE_PX 80
#define LONG_PRESS_MS 600
#define TEXT_MAX 256
#define SHOWN_MAX 96
#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
static constexpr ActionId ACTION_SETTINGS_BASE = 100;
using QueueHandle_t = int;
static QueueHandle_t s_touch_q;
static bool s_settings = true, draw_requested;
static std::atomic<bool> s_image_shown;
static std::atomic<uint32_t> s_navigation_epoch{}, s_presented_epoch{};
static void request_draw() { draw_requested = true; }
#include "navigation_types.inc"
#include "navigation_epoch.inc"
static std::deque<TouchEvent> events;
static int xQueueReceive(QueueHandle_t, TouchEvent *e, int) {
    if (events.empty()) return 0;
    *e = events.front();
    events.pop_front();
    return pdTRUE;
}
#define strlcpy muse_test_strlcpy
static size_t muse_test_strlcpy(char *dst, const char *src, size_t cap) {
    const size_t len = strlen(src), n = cap && len >= cap ? cap-1 : len;
    if (cap) { memcpy(dst,src,n); dst[n] = 0; }
    return len;
}
static const char *muse_text_showable(const char *text, char *, size_t) { return text; }
static Page s_page = WIFI, s_text_back = WIFI;
static int s_scroll, s_saved_n, s_forget_armed;
static uint32_t s_shown_scan_gen;
static bool s_changed, s_text_password, s_text_reveal;
static int64_t s_link_reset_armed_us, s_batt_shown_us;
static char s_link_reset_lbl[24], s_text_title[SHOWN_MAX], s_text_hint[64], s_join_ssid[MUSE_SSID_MAX+1];
static char *s_text;
static size_t s_text_max;
using text_done_cb_t = void (*)(const char *);
static text_done_cb_t s_text_done;
static KeyboardEntry s_entry;
#include "keyboard_snapshot.inc"
static bool settings_tick() { return false; }
static void settings_scroll(int) {}
static void set_page(bool settings) { s_settings = settings; }
static std::string saved_ssid, saved_pass;
static void muse_settings_set_wifi(const char *ssid, const char *pass) {
    saved_ssid = ssid;
    saved_pass = pass;
}
#include "navigation_settings.inc"
#include "navigation_handlers.inc"
#include "navigation_input.inc"
static int ap_actions, key_actions;
#include "keyboard_dispatch.inc"

static void screen(App::ScreenType &s, void *) {
    if (s_page == WIFI) s.frame().hit(Rect{80,540,300,58}, ACT_WIFI_AP);
    if (s_page == TEXT) {
        s.frame().hit(Rect{0,0,56,48}, ACT_BACK);
        KeyboardProps kb;
        kb.layout = &keyboard_for_frame();
        kb.keyAction = ACT_KB_KEY;
        kb.shiftAction = ACT_KB_SHIFT;
        kb.modeAction = ACT_KB_MODE;
        kb.deleteAction = ACT_KB_DEL;
        kb.okAction = ACT_KB_OK;
        kb.langAction = ACT_KB_LANG;
        kb.inputMask = InputTouch | InputLongPress;
        kb.padding = {2,2,2,2};
        kb.gap = 4;
        kb.rowGap = 4;
        keyboard(s.frame(), Rect{8,540,464,248}, kb);
    }
}
static void select_ap(const ActionEvent &, void *) {
    ap_actions++;
    strlcpy(s_join_ssid,"secured-ap",sizeof(s_join_ssid));
    open_text("secured-ap", "", true, MUSE_PASS_MAX, "Password", on_wifi_pass, WIFI);
}
static void back(const ActionEvent &, void *) { settings_back(); }
static void render_page() {
    g_app->render();
    s_presented_epoch = s_navigation_epoch.load();
}
static Point hit_point(ActionId action, int16_t value = 0, bool match_value = true) {
    for (int16_t y = 540; y < 800; y += 2) {
        for (int16_t x = 0; x < 480; x += 2) {
            Interaction hit;
            if (g_app->hitPublished(x,y,action,hit) && (!match_value || hit.value == value)) return {x,y};
        }
    }
    assert(false && "actual keyboard did not publish the expected key");
    return {};
}
static int64_t edge_us;
static void queue_tap(int16_t x, int16_t y) {
    uint32_t epoch = s_presented_epoch.load();
    events.push_back({x,y,true,edge_us,epoch}); edge_us += 100000;
    events.push_back({x,y,false,edge_us,epoch}); edge_us += 100000;
}
static void queue_key(int16_t value) {
    Point p = hit_point(ACT_KB_KEY,value);
    queue_tap(p.x,p.y);
}
static void queue_control(ActionId action) {
    Point p = hit_point(action,0,false);
    queue_tap(p.x,p.y);
}
static void drain_and_present() {
    for (int i = 0; !events.empty(); i++) {
        assert(i < 32);
        handle_touch();
        render_page();
    }
}
static void open_editor() {
    select_ap({},nullptr);
    render_page();
}
static void navigation_test(const std::string &name) {
    Target target;
    DeviceContext device;
    device.width = 480; device.height = 800; device.hasTouch = true;
    App app(target,device);
    g_app = &app;
    app.setScreen(screen);
    app.on(ACT_WIFI_AP,select_ap);
    app.on(ACT_BACK,back);
    for (ActionId id : {ACT_KB_KEY,ACT_KB_SHIFT,ACT_KB_MODE,ACT_KB_DEL,ACT_KB_LANG,ACT_KB_OK}) app.on(id,keyboard_action);
    render_page();
    if (name == "double_tap") {
        queue_tap(120,560); queue_tap(120,560);
        handle_touch();
        assert(ap_actions == 1 && s_page == TEXT && s_text_back == WIFI);
        assert(events.size() == 2 && draw_requested && app.invalidated());
        render_page(); handle_touch();
        assert(events.empty() && ap_actions == 1 && s_page == TEXT && key_actions == 0 && !s_text[0]);
        queue_tap(20,20); handle_touch();
        assert(s_page == WIFI && !s_text[0]);
    } else if (name == "explicit_back") {
        select_ap({},nullptr); select_ap({},nullptr);
        assert(s_text_back == WIFI);
        assert(settings_back() && s_page == WIFI);
    } else if (name == "ssid_password_chain") {
        open_text("Other network", "test-ap", false, MUSE_SSID_MAX, "Network name", on_other_ssid, WIFI);
        text_ready();
        assert(s_page == TEXT && s_text_password && s_text_back == WIFI);
        strlcpy(s_text,"synthetic-password",s_text_max+1);
        text_ready();
        assert(s_page == WIFI && saved_ssid == "test-ap" && saved_pass == "synthetic-password");
        assert(!s_text[0]);
    } else if (name == "rapid_keys") {
        open_editor(); queue_key('a'); queue_key('a'); handle_touch();
        assert(key_actions == 2 && strcmp(s_text,"aa") == 0 && events.empty());
    } else if (name == "shift_key") {
        open_editor(); queue_control(ACT_KB_SHIFT); queue_key('q');
        handle_touch(); assert(events.size() == 2 && s_entry.shifted && !s_text[0]);
        render_page(); handle_touch();
        assert(events.empty() && strcmp(s_text,"Q") == 0 && !s_entry.shifted);
    } else if (name == "repeat_shift") {
        open_editor(); queue_control(ACT_KB_SHIFT); queue_control(ACT_KB_SHIFT); queue_key('q');
        drain_and_present();
        assert(strcmp(s_text,"q") == 0 && !s_entry.shifted);
    } else if (name == "auto_shift") {
        open_editor(); s_entry.shifted = true; render_page();
        queue_key('Q'); queue_key('W'); handle_touch();
        assert(strcmp(s_text,"Q") == 0 && events.size() == 2 && !s_entry.shifted);
        render_page(); handle_touch();
        assert(strcmp(s_text,"Qw") == 0 && events.empty());
    } else if (name == "cyrillic_shift" || name == "accented_shift") {
        open_editor();
        s_entry.layout = name == "cyrillic_shift" ? KeyboardLayoutId::CyrillicRu : KeyboardLayoutId::AzertyFr;
        render_page(); queue_control(ACT_KB_SHIFT); queue_key(name == "cyrillic_shift" ? 0x439 : 1001);
        drain_and_present();
        assert(strcmp(s_text,name == "cyrillic_shift" ? "\xD0\x99" : "\xC3\x89") == 0);
    } else if (name == "symbols") {
        open_editor(); queue_control(ACT_KB_MODE); queue_key('q');
        drain_and_present(); assert(s_entry.symbols && !s_text[0]);
        queue_key('1'); handle_touch(); assert(strcmp(s_text,"1") == 0);
        queue_control(ACT_KB_MODE); queue_key('1'); drain_and_present();
        assert(!s_entry.symbols && strcmp(s_text,"1") == 0);
        queue_key('a'); handle_touch(); assert(strcmp(s_text,"1a") == 0);
    } else if (name == "symbol_shift") {
        open_editor(); s_entry.symbols = true; render_page();
        queue_control(ACT_KB_SHIFT); queue_key('1'); drain_and_present();
        assert(s_entry.shifted && !s_text[0]);
        queue_key('['); handle_touch(); assert(strcmp(s_text,"[") == 0);
    } else if (name == "language") {
        open_editor(); queue_control(ACT_KB_LANG); queue_key('q'); drain_and_present();
        assert(s_entry.layout == KeyboardLayoutId::AzertyFr && !s_text[0]);
        queue_key('a'); handle_touch(); assert(strcmp(s_text,"a") == 0);
    } else if (name == "controls") {
        open_editor(); queue_key('a'); queue_key('b'); handle_touch();
        assert(strcmp(s_text,"ab") == 0);
        queue_control(ACT_KB_DEL); queue_key(QWERTY_KEY_SPACE); queue_key('q'); queue_control(ACT_KB_OK);
        drain_and_present();
        assert(s_page == WIFI && !s_text[0] && saved_ssid == "secured-ap" && saved_pass == "a q");
    } else if (name == "during_transition") {
        select_ap({},nullptr); queue_tap(120,560); handle_touch();
        assert(key_actions == 0 && !s_text[0]);
        render_page(); queue_key('a'); handle_touch();
        assert(key_actions == 1 && strcmp(s_text,"a") == 0);
    } else if (name == "orphan_release") {
        queue_tap(120,560); handle_touch();
        assert(ap_actions == 1 && s_page == TEXT);
        settings_back(); render_page();
        events.push_back({120,560,false,edge_us,s_presented_epoch.load()});
        handle_touch(); assert(ap_actions == 1 && s_page == WIFI);
    } else if (name == "swipe_then_tap") {
        uint32_t epoch = s_presented_epoch.load();
        events.push_back({100,100,true,0,epoch}); events.push_back({240,100,false,100000,epoch});
        queue_tap(120,560); handle_touch();
        assert(s_page == HOME && events.size() == 2);
        render_page(); handle_touch();
        assert(ap_actions == 0 && events.empty() && s_page == HOME);
    } else assert(false);
    free(s_text); s_text = nullptr;
}

using esp_err_t = int;
using spi_host_device_t = int;
static constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_NO_MEM = 2;
static constexpr int SPI2_HOST = 2, SPI_DMA_CH_AUTO = -1, MALLOC_CAP_DMA = 0;
static constexpr int GPIO_NUM_12 = 12, GPIO_NUM_13 = 13, GPIO_NUM_14 = 14;
struct spi_bus_config_t { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz; };
static spi_bus_config_t captured_bus;
static bool bus_exists;
static int bus_calls, forced_error, log_errors;
static esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config, int dma) {
    assert(host == SPI2_HOST && dma == SPI_DMA_CH_AUTO); bus_calls++;
    if (forced_error) return forced_error;
    if (bus_exists) return ESP_ERR_INVALID_STATE;
    captured_bus = *config; bus_exists = true; return ESP_OK;
}
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }
static const char *esp_err_to_name(esp_err_t) { return "synthetic-error"; }
static void log_error(const char *, const char *, const char *) { log_errors++; }
#define ESP_LOGE log_error
static const char *TAG = "freeink";
static constexpr spi_host_device_t kHost = SPI2_HOST;
static constexpr size_t kChunk = 4092;
static uint8_t *s_dma;
struct SPIClass { bool _ownsBus{}; void begin(int8_t,int8_t,int8_t,int8_t); };
#include "sticky_sd_bus.inc"
#include "freeink_spi_begin.inc"
static void sd_test(const std::string &name) {
    SPIClass spi;
    if (name == "shared_bus") {
        assert(sd_bus_init() == ESP_OK);
        assert(captured_bus.miso_io_num == 12 && captured_bus.mosi_io_num == 14 && captured_bus.sclk_io_num == 13);
        assert(captured_bus.max_transfer_sz >= static_cast<int>(kChunk));
        spi.begin(13,-1,14,-1);
        assert(bus_calls == 2 && captured_bus.miso_io_num == 12 && !spi._ownsBus && log_errors == 0);
    } else if (name == "bus_failure") {
        forced_error = ESP_ERR_NO_MEM;
        assert(sd_bus_init() == ESP_ERR_NO_MEM && !bus_exists);
    } else if (name == "panel_only") {
        spi.begin(13,-1,14,-1);
        assert(bus_calls == 1 && captured_bus.miso_io_num == -1 && spi._ownsBus);
    } else assert(false);
    free(s_dma);
}
int main(int argc, char **argv) {
    assert(argc == 2); const std::string name = argv[1];
    if (name == "shared_bus" || name == "bus_failure" || name == "panel_only") sd_test(name);
    else navigation_test(name);
}
