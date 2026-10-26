/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations.
 */
#include "muse_eink_priv.h"
#include <assert.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
using namespace freeink::ui;
#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define pdTRUE 1
#define SWIPE_PX 80
#define LONG_PRESS_MS 600
#define ESP_OK 0
#define ESP_MAC_WIFI_STA 0
#include "wifi_types.inc"
#include "status_types.inc"
namespace muse_eink {
#include "wifi_page_types.inc"
struct Target final : DrawTarget {
    Size measureText(FontId, const char *text, TextStyle) const override {
        int n = 0;
        for (const char *p = text; *p; p += utf8_scalar_bytes(p)) n++;
        return {static_cast<int16_t>(n * 8), 16};
    }
    int16_t lineHeight(FontId) const override { return 16; }
    void fill(Rect, Paint, uint8_t, uint8_t) override {}
    void stroke(Rect, Paint, uint8_t, uint8_t, uint8_t) override {}
    void line(Point, Point, uint8_t, Paint) override {}
    void triangle(Point, Point, Point, Paint) override {}
    void text(Rect, const char *, TextStyle) override {}
    void bitmap(Rect, BitmapRef, BitmapMode, Paint, Rotation) override {}
};
App *g_app;
int g_w = 480, g_h = 800;
static Target *s_target;
static bool s_settings = true, s_changed, draw_requested, s_render_image;
static Page s_page = WIFI;
static std::atomic<bool> s_image_shown;
static std::atomic<uint32_t> s_navigation_epoch{}, s_presented_epoch{};
using QueueHandle_t = int;
static QueueHandle_t s_touch_q;
#include "wifi_touch_types.inc"
#include "wifi_epoch.inc"
#include "wifi_globals.inc"
#include "status_globals.inc"
static std::deque<TouchEvent> events;
static std::vector<muse_wifi_saved_t> stored;
static std::vector<muse_wifi_ap_t> scanned;
static std::vector<std::string> forgotten;
static uint32_t scan_generation = 1;
static int64_t now_us = 1000000, edge_us;
static std::string joined;
static muse_wifi_status_t wifi_state{};
static bool scanning;
static std::function<void()> during_present;
static int64_t esp_timer_get_time() { return now_us; }
#define strlcpy muse_test_strlcpy
static size_t muse_test_strlcpy(char *out, const char *text, size_t cap) {
    size_t n = strlen(text);
    if (cap) {
        size_t copy = n < cap ? n : cap - 1;
        memcpy(out, text, copy); out[copy] = 0;
    }
    return n;
}
#define strlcat muse_test_strlcat
static size_t muse_test_strlcat(char *out, const char *src, size_t cap) {
    size_t n = strlen(out), len = strlen(src);
    if (n < cap) strlcpy(out + n, src, cap - n);
    return n + len;
}
// Ordinary synthetic display records only; no real settings, radio or NVS calls.
static const char *muse_text_showable(const char *text, char *, size_t) { return text; }
static void muse_wifi_status(muse_wifi_status_t *out) { *out = wifi_state; }
static bool muse_wifi_scanning() { return scanning; }
static void muse_wifi_apply() {}
static int muse_wifi_saved(muse_wifi_saved_t *out, int cap) {
    int n = static_cast<int>(stored.size()); if (n > cap) n = cap;
    if (n) memcpy(out, stored.data(), n * sizeof(*out));
    return n;
}
static int muse_wifi_scan_results(muse_wifi_ap_t *out, int cap, uint32_t *generation) {
    int n = static_cast<int>(scanned.size()); if (n > cap) n = cap;
    if (n) memcpy(out, scanned.data(), n * sizeof(*out));
    *generation = scan_generation;
    return n;
}
static void muse_wifi_forget(const char *ssid) {
    forgotten.emplace_back(ssid);
    for (auto i = stored.begin(); i != stored.end(); ++i) if (!strcmp(i->ssid, ssid)) { stored.erase(i); break; }
}
static void muse_settings_set_wifi(const char *ssid, const char *) { joined = ssid; }
static void on_wifi_pass(const char *) {}
static void open_text(const char *title, const char *, bool, size_t, const char *, void(*)(const char *), Page) {
    joined = title; s_page = TEXT; navigation_changed();
}
static int esp_read_mac(uint8_t *out, int) { memset(out, 0, 6); return ESP_OK; }
static bool sdk_changed;
static muse_hatch_status_t chat_state{};
static muse_ble_status_t ble_state{};
static muse_battery_t battery_state{};
static muse_power_t power_state{-1, 0, false, false};
static int pairing_resets, phone_forgets, battery_resets;
static bool muse_link_hatch_linked() { return true; }
static bool muse_link_sdk_token_changed() { return sdk_changed; }
static int muse_link_state() { return 0; }
static const char *muse_link_state_name(int) { return "Ready"; }
static void muse_hatch_status(muse_hatch_status_t *out) { *out = chat_state; }
static const char *muse_hatch_state_name(muse_hatch_state_t) { return "Ready"; }
static void muse_settings_hatch_host(char *out) { out[0] = 0; }
static void muse_settings_hatch_vm(char *out) { out[0] = 0; }
static size_t muse_settings_hatch_token_len() { return 0; }
static const char *muse_link_sdk_token() { return nullptr; }
static void muse_ble_status(muse_ble_status_t *out) { *out = ble_state; }
static bool muse_settings_ble_on() { return true; }
static void muse_battery_read(muse_battery_t *out) { *out = battery_state; }
static muse_power_t muse_state_power() { return power_state; }
static bool muse_battery_drain(const muse_battery_t *, int *, int *) { return false; }
static void muse_link_reset_setup() { pairing_resets++; }
static void muse_ble_forget_all() { phone_forgets++; }
static void muse_battery_reset() { battery_resets++; }
void request_draw() { draw_requested = true; }
#include "wifi_setters.inc"
#include "wifi_drawing_pipeline.inc"
#include "wifi_drawing.inc"
#include "wifi_tick.inc"
#include "status_tick.inc"
bool settings_tick() {
    switch (s_page) {
    case WIFI: tick_wifi(); break;
    case HATCH: tick_hatch(); break;
    case BLE: tick_ble(); break;
    case BATTERY: tick_battery(); break;
    default: break;
    }
    return s_changed;
}
bool settings_typing() { return s_page == TEXT; }
bool settings_back() { return false; }
bool settings_keyboard_changed() { return false; }
void settings_scroll(int) {}
static void set_page(bool value) { s_settings = value; navigation_changed(); }
static int xQueueReceive(QueueHandle_t, TouchEvent *out, int) {
    if (events.empty()) return 0;
    *out = events.front(); events.pop_front();
    return pdTRUE;
}
#include "wifi_input.inc"
static void on_wifi_action(const ActionEvent &e, void *) {
    int64_t now = esp_timer_get_time();
    switch (e.action) {
#include "wifi_actions.inc"
    default: assert(false);
    }
}
static void on_status_action(const ActionEvent &e, void *) {
    int64_t now = esp_timer_get_time();
    switch (e.action) {
#include "status_actions.inc"
    default: assert(false);
    }
    settings_tick();
}
#include "wifi_page.inc"
#include "status_page.inc"
static void screen(Screen &s, void *) {
    Column c{&s, g_w / 2 - LIST_W / 2, LIST_TOP};
    switch (s_page) {
    case WIFI: page_wifi(c); break;
    case HATCH: page_hatch(c); break;
    case BLE: page_ble(c); break;
    case BATTERY: page_battery(c); break;
    default: break;
    }
}
struct Panel {
    uint8_t fb[480 * 800 / 8]{};
    uint8_t *getFrameBuffer() { return fb; }
    int getDisplayWidthBytes() { return 480 / 8; }
    int getDisplayHeight() { return 800; }
};
static Panel s_display;
static bool present_image() { return false; }
static void present() { if (during_present) during_present(); }
#include "wifi_draw.inc"
static void render_page() {
    draw(InputSnapshot{});
    assert(!g_app->interactionOverflowed() && !g_app->handlerOverflowed());
}
static void queue_tap(int x, int y) {
    uint32_t epoch = s_presented_epoch.load();
    events.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), true, edge_us, epoch}); edge_us += 100000;
    events.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y), false, edge_us, epoch}); edge_us += 100000;
}
static Rect published(ActionId action, int value = 0) {
    for (int y = LIST_TOP; y < g_h; y++) {
        Interaction i;
        if (g_app->hitPublished(g_w / 2, y, action, i) && i.value == value) return i.rect;
    }
    assert(false); return {};
}
static void tap(Rect r) { queue_tap(r.x + r.width / 2, r.y + 5); }
static void tap(ActionId action, int value = 0) { tap(published(action, value)); }
static muse_wifi_saved_t saved(const char *ssid) {
    muse_wifi_saved_t out{}; strlcpy(out.ssid, ssid, sizeof(out.ssid)); return out;
}
static muse_wifi_ap_t ap(const char *ssid, bool secure = true) {
    muse_wifi_ap_t out{}; strlcpy(out.ssid, ssid, sizeof(out.ssid)); out.secure = secure; return out;
}
static void short_status() { wifi_state = {}; wifi_state.state = MUSE_WIFI_NO_NETWORK; }
static void tall_status() {
    wifi_state = {}; wifi_state.state = MUSE_WIFI_CONNECTING;
    strlcpy(wifi_state.ssid, "synthetic-long-network-name-ABCDE", sizeof(wifi_state.ssid));
    strlcpy(wifi_state.detail, "Joining synthetic network while waiting for a local test response", sizeof(wifi_state.detail));
}
static void assert_no_delete() {
    assert(forgotten.empty() && stored.size() == 2);
    assert(!strcmp(stored[0].ssid, "A") && !strcmp(stored[1].ssid, "B"));
}
static void status_resize(bool growth) {
    if (!growth) { tall_status(); tick_wifi(); render_page(); }
    Rect old_a = published(ACT_WIFI_SAVED, 0), old_b = published(ACT_WIFI_SAVED, 1);
    tap(old_a); handle_touch(); assert(s_forget_armed == 0);
    tap(old_b);  // B on the physically shown layout, queued before the tick.
    uint32_t shown = s_presented_epoch.load();
    if (growth) tall_status(); else short_status();
    tick_wifi();
    assert(s_navigation_epoch.load() == shown + 1 && s_presented_epoch.load() == shown);
    assert(s_forget_armed == -1 && s_forget_armed_us == 0 && strcmp(s_saved_vals[0], "Tap to forget"));
    during_present = [=] {
        Rect new_a = published(ACT_WIFI_SAVED, 0);
        assert(growth ? new_a.y > old_a.y : new_a.y < old_a.y);
        if (growth) {
            Interaction hit;
            assert(g_app->hitPublished(old_b.x + old_b.width / 2, old_b.y + 5, ACT_WIFI_SAVED, hit));
            assert(hit.value == 0);  // Old B really overlaps new A; not a fixed-coordinate surrogate.
        }
        assert(s_presented_epoch.load() == shown);
        tap(old_b);  // New table published, but the physical panel still shows old B.
        assert(events.back().epoch == shown);
    };
    render_page(); during_present = {};
    assert(s_presented_epoch.load() == s_navigation_epoch.load());
    handle_touch(); assert(events.empty() && s_forget_armed == -1); assert_no_delete();
    tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(s_forget_armed == 0); assert_no_delete();
    tap(ACT_WIFI_SAVED, 0); handle_touch();
    assert(forgotten == std::vector<std::string>{"A"} && stored.size() == 1 && !strcmp(stored[0].ssid, "B"));
}
static void status_page_resize(Page page, ActionId action, const std::function<void()> &change) {
    s_page = page; settings_tick(); render_page();
    Rect old = published(action);
    if (page == HATCH) { tap(old); handle_touch(); assert(s_link_reset_armed_us); }
    uint32_t shown = s_presented_epoch.load(); tap(old);
    change(); settings_tick();
    assert(s_navigation_epoch.load() == shown + 1 && s_presented_epoch.load() == shown);
    if (page == HATCH) assert(s_link_reset_armed_us == 0 && !strcmp(s_link_reset_lbl, "Reset pairing"));
    during_present = [=] {
        assert(published(action).y != old.y && s_presented_epoch.load() == shown); tap(old);
    };
    render_page(); during_present = {}; handle_touch();
    assert(events.empty() && pairing_resets == 0 && phone_forgets == 0 && battery_resets == 0);
    tap(action); handle_touch();
    if (page == HATCH) { tap(action); handle_touch(); }
    assert(page == HATCH ? pairing_resets == 1 : page == BLE ? phone_forgets == 1 : battery_resets == 1);
}
} // namespace muse_eink
using namespace muse_eink;
int main(int argc, char **argv) {
    assert(argc == 2); std::string name = argv[1];
    Target target; s_target = &target;
    DeviceContext device; device.width = g_w; device.height = g_h; device.hasTouch = true;
    App app(target, device); g_app = &app; app.setScreen(screen);
    app.on(ACT_WIFI_SAVED, on_wifi_action); app.on(ACT_WIFI_AP, on_wifi_action);
    app.on(ACT_HATCH_RESET, on_status_action); app.on(ACT_BLE_FORGET, on_status_action);
    app.on(ACT_BATT_RESET, on_status_action);
    stored = {saved("A"), saved("B")}; scanned = {ap("scan-A"), ap("scan-B")};
    short_status(); tick_wifi(); render_page();
    if (name == "delete_owner") {
        Rect a = published(ACT_WIFI_SAVED, 0);
        tap(a); tap(a); tap(a); tap(a); handle_touch();
        assert(forgotten == std::vector<std::string>{"A"} && stored.size() == 1 && !strcmp(stored[0].ssid, "B"));
        render_page(); handle_touch();
        assert(events.empty() && forgotten.size() == 1 && s_forget_armed == -1);
        tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(s_forget_armed == 0 && forgotten.size() == 1);
        tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(forgotten == std::vector<std::string>({"A", "B"}));
    } else if (name == "saved_reorder") {
        tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(s_forget_armed == 0);
        tap(ACT_WIFI_SAVED, 0); stored = {saved("B"), saved("A")}; tick_wifi();
        assert(s_forget_armed == -1); render_page(); handle_touch();
        assert(forgotten.empty() && events.empty());
        tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(s_forget_armed == 0 && forgotten.empty());
    } else if (name == "saved_membership") {
        tap(ACT_WIFI_SAVED, 0); handle_touch(); tap(ACT_WIFI_SAVED, 0);
        stored = {saved("C"), saved("B")}; tick_wifi(); render_page(); handle_touch();
        assert(forgotten.empty() && s_forget_armed == -1);
    } else if (name == "scan_reorder") {
        tap(ACT_WIFI_AP, 0); scanned = {ap("scan-B"), ap("scan-A")}; tick_wifi();
        render_page(); handle_touch(); assert(joined.empty() && events.empty());
        tap(ACT_WIFI_AP, 0); handle_touch(); assert(joined == "scan-B" && s_page == TEXT);
    } else if (name == "scan_status_only") {
        tap(ACT_WIFI_AP, 0); scanned[0].rssi = -30; scan_generation++; tick_wifi();
        render_page(); handle_touch(); assert(joined == "scan-A");
    } else if (name == "bounds") {
        on_wifi_action(ActionEvent{ACT_WIFI_SAVED, -1}, nullptr);
        on_wifi_action(ActionEvent{ACT_WIFI_AP, static_cast<int16_t>(MAX_APS)}, nullptr);
        assert(forgotten.empty() && joined.empty());
    } else if (name == "status_growth" || name == "status_shrink") {
        status_resize(name == "status_growth");
    } else if (name == "status_same_height") {
        wifi_state.state = MUSE_WIFI_CONNECTING;
        strlcpy(wifi_state.ssid, "test", sizeof(wifi_state.ssid));
        strlcpy(wifi_state.detail, "Waiting", sizeof(wifi_state.detail));
        tick_wifi(); render_page();
        Rect a = published(ACT_WIFI_SAVED, 0); tap(a); handle_touch(); tap(a);
        uint32_t epoch = s_navigation_epoch.load();
        strlcpy(wifi_state.ssid, "other", sizeof(wifi_state.ssid));
        strlcpy(wifi_state.detail, "Still waiting", sizeof(wifi_state.detail));
        tick_wifi();
        assert(s_navigation_epoch.load() == epoch && s_forget_armed == 0);
        render_page(); assert(published(ACT_WIFI_SAVED, 0).y == a.y);
        handle_touch();
        assert(forgotten == std::vector<std::string>{"A"} && stored.size() == 1 && !strcmp(stored[0].ssid, "B"));
    } else if (name == "scan_empty" || name == "wifi_section") {
        scanned.clear(); scan_generation = 0; tick_wifi(); render_page();
        for (bool added : {true, false}) {
            Rect old_other = published(ACT_WIFI_OTHER);
            tap(ACT_WIFI_SAVED, 0); handle_touch(); assert(s_forget_armed == 0);
            uint32_t shown = s_presented_epoch.load(); tap(old_other);
            if (name == "scan_empty") scan_generation = added ? 1 : 0;
            else if (added) wifi_state.state = MUSE_WIFI_OFF;
            else short_status();
            tick_wifi(); assert(s_navigation_epoch.load() == shown + 1 && s_forget_armed == -1);
            during_present = [=] {
                Rect next = published(ACT_WIFI_OTHER);
                assert(next.y != old_other.y && s_presented_epoch.load() == shown); tap(old_other);
            };
            render_page(); during_present = {}; handle_touch();
            assert(events.empty() && joined.empty()); assert_no_delete();
        }
    } else if (name == "scanning_label") {
        tap(ACT_WIFI_SAVED, 0); handle_touch(); tap(ACT_WIFI_SAVED, 0);
        uint32_t epoch = s_navigation_epoch.load(); scanning = true; tick_wifi();
        assert(s_navigation_epoch.load() == epoch && s_forget_armed == 0);
        render_page(); handle_touch(); assert(forgotten == std::vector<std::string>{"A"});
    } else if (name == "link_status") {
        status_page_resize(HATCH, ACT_HATCH_RESET, [] { sdk_changed = true; });
    } else if (name == "ble_status") {
        status_page_resize(BLE, ACT_BLE_FORGET, [] { ble_state.state = MUSE_BLE_CONNECTED; });
    } else if (name == "battery_status") {
        status_page_resize(BATTERY, ACT_BATT_RESET, [] {
            s_batt_shown_us = 0; battery_state.started = true;
            strlcpy(battery_state.awake, "synthetic wifi 4%, synthetic bt 1%", sizeof(battery_state.awake));
        });
    } else if (name == "note_extent") {
        char note_text[96] = "Off";
        assert(!SET_NOTE(note_text, "On"));
        assert(SET_NOTE(note_text, "Phone connected\nPaired"));
        assert(!SET_NOTE(note_text, "Phone connected\nWaiting for pairing"));
        assert(SET_NOTE(note_text, ""));
        assert(SET_NOTE(note_text, "Muse app: paired\nReady"));
        assert(!SET_NOTE(note_text, "Muse app: paired\nBusy"));
    } else assert(false);
}
