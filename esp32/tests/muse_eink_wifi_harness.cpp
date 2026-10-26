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
#include <FreeInkApp.h>
#include <assert.h>
#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
using namespace freeink::ui;
using App = FreeInkApp<160,96>;
#define MUSE_SSID_MAX 32
#define MUSE_PASS_MAX 64
#define pdTRUE 1
#define SWIPE_PX 80
#define LONG_PRESS_MS 600
static constexpr ActionId ACTION_SETTINGS_BASE = 100;
#include "wifi_types.inc"
#include "wifi_page_types.inc"
struct Target final : DrawTarget {
    Size measureText(FontId,const char *text,TextStyle) const override { return {static_cast<int16_t>(strlen(text)*8),16}; }
    int16_t lineHeight(FontId) const override { return 16; }
    void fill(Rect,Paint,uint8_t,uint8_t) override {}
    void stroke(Rect,Paint,uint8_t,uint8_t,uint8_t) override {}
    void line(Point,Point,uint8_t,Paint) override {}
    void triangle(Point,Point,Point,Paint) override {}
    void text(Rect,const char *,TextStyle) override {}
    void bitmap(Rect,BitmapRef,BitmapMode,Paint,Rotation) override {}
};
static App *g_app;
static bool s_settings = true, s_changed, draw_requested;
static Page s_page = WIFI;
static std::atomic<bool> s_image_shown;
static std::atomic<uint32_t> s_navigation_epoch{},s_presented_epoch{};
using QueueHandle_t = int;
static QueueHandle_t s_touch_q;
#include "wifi_touch_types.inc"
#include "wifi_epoch.inc"
#include "wifi_globals.inc"
static std::deque<TouchEvent> events;
static std::vector<muse_wifi_saved_t> stored;
static std::vector<muse_wifi_ap_t> scanned;
static std::vector<std::string> forgotten;
static uint32_t scan_generation = 1;
static int64_t now_us = 1000000, edge_us;
static std::string joined;
static int64_t esp_timer_get_time() { return now_us; }
#define strlcpy muse_test_strlcpy
static size_t muse_test_strlcpy(char *out,const char *text,size_t cap) {
    size_t n=strlen(text),copy=n<cap?n:cap-1;
    if(cap) { memcpy(out,text,copy); out[copy]=0; }
    return n;
}
static void set_text(char *out,size_t cap,const char *text) {
    if(strcmp(out,text)) { strlcpy(out,text,cap); s_changed=true; }
}
#define SET(dst,src) set_text(dst,sizeof(dst),src)
static void set_flag(bool *out,bool value) { if(*out!=value) { *out=value; s_changed=true; } }
static void muse_wifi_status(muse_wifi_status_t *out) { *out={}; out->state=MUSE_WIFI_OFF; }
static bool muse_wifi_scanning() { return false; }
static void muse_wifi_apply() {}
static int muse_wifi_saved(muse_wifi_saved_t *out,int cap) {
    int n=static_cast<int>(stored.size()); if(n>cap)n=cap;
    if(n) memcpy(out,stored.data(),n*sizeof(*out));
    return n;
}
static int muse_wifi_scan_results(muse_wifi_ap_t *out,int cap,uint32_t *generation) {
    int n=static_cast<int>(scanned.size()); if(n>cap)n=cap;
    if(n) memcpy(out,scanned.data(),n*sizeof(*out));
    *generation=scan_generation;
    return n;
}
static void muse_wifi_forget(const char *ssid) {
    forgotten.emplace_back(ssid);
    for(auto i=stored.begin();i!=stored.end();++i)if(!strcmp(i->ssid,ssid)) { stored.erase(i); break; }
}
static void muse_settings_set_wifi(const char *ssid,const char *) { joined=ssid; }
static void on_wifi_pass(const char *) {}
static void open_text(const char *title,const char *,bool,size_t,const char *,void(*)(const char *),Page) {
    joined=title; s_page=TEXT; navigation_changed();
}
static void request_draw() { draw_requested=true; }
#include "wifi_tick.inc"
static bool settings_tick() { tick_wifi(); return s_changed; }
static bool settings_typing() { return s_page==TEXT; }
static bool settings_back() { return false; }
static bool settings_keyboard_changed() { return false; }
static void settings_scroll(int) {}
static void set_page(bool value) { s_settings=value; navigation_changed(); }
static int xQueueReceive(QueueHandle_t,TouchEvent *out,int) {
    if(events.empty()) return 0;
    *out=events.front();
    events.pop_front();
    return pdTRUE;
}
#include "wifi_input.inc"
static void on_wifi_action(const ActionEvent &e,void *) {
    int64_t now = esp_timer_get_time();
    switch(e.action) {
#include "wifi_actions.inc"
    default: assert(false);
    }
}
static void screen(App::ScreenType &s,void *) {
    if(s_page!=WIFI)return;
    for(int i=0;i<s_saved_n;i++)s.frame().hit(Rect{20,static_cast<int16_t>(100+i*60),300,58},ACT_WIFI_SAVED,static_cast<int16_t>(i));
    for(int i=0;i<s_ap_n;i++)s.frame().hit(Rect{20,static_cast<int16_t>(500+i*60),300,58},ACT_WIFI_AP,static_cast<int16_t>(i));
}
static void render_page() { g_app->render(); s_presented_epoch=s_navigation_epoch.load(); }
static void queue_tap(int x,int y) {
    uint32_t epoch=s_presented_epoch.load();
    events.push_back({static_cast<int16_t>(x),static_cast<int16_t>(y),true,edge_us,epoch}); edge_us+=100000;
    events.push_back({static_cast<int16_t>(x),static_cast<int16_t>(y),false,edge_us,epoch}); edge_us+=100000;
}
static muse_wifi_saved_t saved(const char *ssid) { muse_wifi_saved_t out{}; strlcpy(out.ssid,ssid,sizeof(out.ssid)); return out; }
static muse_wifi_ap_t ap(const char *ssid,bool secure=true) { muse_wifi_ap_t out{}; strlcpy(out.ssid,ssid,sizeof(out.ssid)); out.secure=secure; return out; }
int main(int argc,char **argv) {
    assert(argc==2); std::string name=argv[1];
    Target target; DeviceContext device; device.width=480; device.height=800; device.hasTouch=true;
    App app(target,device); g_app=&app; app.setScreen(screen);
    app.on(ACT_WIFI_SAVED,on_wifi_action); app.on(ACT_WIFI_AP,on_wifi_action);
    stored={saved("A"),saved("B")}; scanned={ap("scan-A"),ap("scan-B")};
    tick_wifi(); render_page();
    if(name=="delete_owner") {
        queue_tap(40,120); queue_tap(40,120); queue_tap(40,120); queue_tap(40,120);
        handle_touch();
        assert(forgotten==std::vector<std::string>{"A"} && stored.size()==1 && !strcmp(stored[0].ssid,"B"));
        render_page(); handle_touch();
        assert(events.empty() && forgotten.size()==1 && s_forget_armed==-1);
        queue_tap(40,120); handle_touch(); assert(s_forget_armed==0 && forgotten.size()==1);
        queue_tap(40,120); handle_touch(); assert(forgotten==std::vector<std::string>({"A","B"}));
    } else if(name=="saved_reorder") {
        queue_tap(40,120); handle_touch(); assert(s_forget_armed==0);
        queue_tap(40,120); stored={saved("B"),saved("A")}; tick_wifi();
        assert(s_forget_armed==-1); render_page(); handle_touch();
        assert(forgotten.empty() && events.empty());
        queue_tap(40,120); handle_touch(); assert(s_forget_armed==0 && forgotten.empty());
    } else if(name=="saved_membership") {
        queue_tap(40,120); handle_touch(); queue_tap(40,120);
        stored={saved("C"),saved("B")}; tick_wifi(); render_page(); handle_touch();
        assert(forgotten.empty() && s_forget_armed==-1);
    } else if(name=="scan_reorder") {
        queue_tap(40,520); scanned={ap("scan-B"),ap("scan-A")}; tick_wifi();
        render_page(); handle_touch(); assert(joined.empty() && events.empty());
        queue_tap(40,520); handle_touch(); assert(joined=="scan-B" && s_page==TEXT);
    } else if(name=="scan_status_only") {
        queue_tap(40,520); scanned[0].rssi=-30; scan_generation++; tick_wifi();
        render_page(); handle_touch(); assert(joined=="scan-A");
    } else if(name=="bounds") {
        on_wifi_action(ActionEvent{ACT_WIFI_SAVED,-1},nullptr);
        on_wifi_action(ActionEvent{ACT_WIFI_AP,static_cast<int16_t>(MAX_APS)},nullptr);
        assert(forgotten.empty() && joined.empty());
    } else assert(false);
}
