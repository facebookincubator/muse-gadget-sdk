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

/* Real LVGL widgets and production callbacks; no radio, NVS or hardware access. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the private callbacks visible without adding firmware test APIs. */
#include "../../components/muse/muse_settings_ui.c"
#include "src/widgets/label/lv_label_private.h"   /* dot_begin: where "..." replaced a label's letters */

/* The sweep and shot scenarios read whole pages: their readers answer then (else they fail this focused test). */
static bool sweep;
#define SWEEP_ONLY() do { if (!sweep) abort(); } while (0)

static muse_board_t test_board;
const muse_board_t *muse_board = &test_board;
static char saved_ssid[MUSE_SSID_MAX + 1], saved_pass[MUSE_PASS_MAX + 1];
static unsigned saves;
static int volume_setting = 70;
#if CONFIG_MUSE_UI_THEME
static muse_theme_mode_t theme_setting;
muse_theme_mode_t muse_settings_theme(void) {return theme_setting;}
esp_err_t muse_settings_set_theme(muse_theme_mode_t mode) {theme_setting=mode;return ESP_OK;}
#endif
#if CONFIG_MUSE_AVATAR_SYNC
static muse_avatar_source_t avatar_source;
static const char *avatar_status_text = "Built-in";
static unsigned avatar_refreshes, avatar_clears;
muse_avatar_source_t muse_avatar_source(void) { return avatar_source; }
void muse_avatar_select(muse_avatar_source_t source) { avatar_source=source; }
const char *muse_avatar_status(void) { return avatar_status_text; }
void muse_avatar_refresh(void) { avatar_refreshes++; }
void muse_avatar_clear(void) { avatar_clears++; }
#endif

void muse_voice_set_monitor(bool on) { (void)on; }
float muse_voice_monitor_db(void) { assert(muse_board->mic_init); return -40.0f; }   /* no meter without a microphone */
void muse_ui_set_swipe_enabled(bool on) { (void)on; }
/* Other settings pages are linked by the production navigation table. Any
 * accidental access to their device services fails this focused test. */
void muse_audio_set_mic_gain(int v) { (void)v; abort(); }
void muse_audio_set_volume(int v) { (void)v; abort(); }
void muse_battery_reset(void) { abort(); }
void muse_ble_forget_all(void) { abort(); }
void muse_hatch_test(void) { abort(); }
void muse_input_request_power_off(void) { abort(); }
void muse_input_request_restart(void) { abort(); }
void muse_link_reset_setup(void) { abort(); }
bool muse_link_sdk_token_set(const char *s) { (void)s; abort(); }
int muse_link_wifi_proxy_get(const char *s, network_proxy_config_t *p) { (void)s; (void)p; abort(); }
bool muse_link_wifi_proxy_set(const char *s, const network_proxy_config_t *p) { (void)s; (void)p; abort(); }
bool network_proxy_valid(const network_proxy_config_t *p) { (void)p; abort(); }
void network_proxy_wipe(void *data, unsigned len) { memset(data, 0, len); }
bool muse_settings_ble_on(void) { SWEEP_ONLY(); return true; }
int muse_settings_brightness(void) { return 60; }
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]) { SWEEP_ONLY(); strlcpy(out, "muse.example.com", MUSE_HOST_MAX + 1); }
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]) { SWEEP_ONLY(); strlcpy(out, "vm-1234", MUSE_VM_MAX + 1); }
int muse_settings_mic_gain(void) { return 30; }
void muse_settings_set_ble_on(bool v) { (void)v; abort(); }
void muse_settings_set_brightness(int v) { (void)v; abort(); }
bool muse_settings_set_hatch_host(const char *s) { (void)s; abort(); }
esp_err_t muse_settings_set_hatch_token(const char *s, bool v) { (void)s; (void)v; abort(); }
void muse_settings_set_hatch_vm(const char *s) { (void)s; abort(); }
void muse_settings_set_mic_gain(int v) { (void)v; abort(); }
void muse_settings_set_sleep_s(int v) { (void)v; abort(); }
void muse_settings_set_speaker_on(bool v) { (void)v; abort(); }
void muse_settings_set_volume(int v) { (void)v; abort(); }
void muse_settings_set_wifi_on(bool v) { (void)v; abort(); }
int muse_settings_sleep_s(void) { return 120; }
int muse_settings_text_size(void) { return MUSE_TEXT_SMALL; }
void muse_settings_set_text_size(int v) { (void)v; abort(); }
bool muse_settings_speaker_on(void) { return true; }
int muse_settings_volume(void) { return volume_setting; }
bool muse_settings_wifi_on(void) { return false; }
void muse_wifi_status(muse_wifi_status_t *out) { *out = (muse_wifi_status_t){.state=MUSE_WIFI_OFF}; }
void muse_ble_status(muse_ble_status_t *out) { *out = (muse_ble_status_t){.state=MUSE_BLE_OFF}; }
void muse_hatch_status(muse_hatch_status_t *out) { *out = (muse_hatch_status_t){0}; }
const char *muse_hatch_state_name(muse_hatch_state_t state) { (void)state; return "Offline"; }
bool muse_link_hatch_linked(void) { return false; }
const char *muse_link_sdk_token(void) { return NULL; }
bool muse_link_sdk_token_changed(void) { return false; }
muse_link_state_t muse_link_state(void) { return MUSE_LINK_UNPAIRED; }
const char *muse_link_state_name(muse_link_state_t state) { (void)state; return "Unpaired"; }
size_t muse_settings_hatch_token_len(void) { return 0; }
muse_power_t muse_state_power(void) { return (muse_power_t){.battery_pct=-1}; }
void muse_battery_read(muse_battery_t *out) { *out = (muse_battery_t){0}; }
bool muse_battery_drain(const muse_battery_t *b, int *rate, int *hours) { (void)b; (void)rate; (void)hours; return false; }
void muse_wifi_apply(void) { abort(); }
void muse_wifi_forget(const char *ssid) { (void)ssid; abort(); }
int muse_wifi_saved(muse_wifi_saved_t *out, int max)
{
    SWEEP_ONLY();
    assert(max >= 1);
    memset(out, 0, sizeof(*out));
    strlcpy(out->ssid, "A home network with a long name", sizeof(out->ssid));
    return 1;
}
int muse_wifi_scan_results(muse_wifi_ap_t *out, int max, uint32_t *gen)
{
    SWEEP_ONLY();
    assert(max >= 1);
    memset(out, 0, sizeof(*out));
    strlcpy(out->ssid, "The coffee shop's guest network", sizeof(out->ssid));
    out->secure = true;
    *gen = 1;
    return 1;
}
bool muse_wifi_scanning(void) { SWEEP_ONLY(); return false; }
void muse_state_set_asleep(bool v) { (void)v; abort(); }
void muse_ui_preview_brightness(int v) { (void)v; abort(); }
void muse_ui_show_face(void) { abort(); }
bool muse_ui_text_size_applies(void) { return true; }
void muse_voice_request_chirp(void) { abort(); }
esp_err_t muse_wifi_scan(void) { abort(); }
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type;
    static const uint8_t m[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9a };   /* Wi-Fi's MAC address row */
    memcpy(mac, m, sizeof(m));
    return ESP_OK;
}
int64_t esp_timer_get_time(void) { return 1000000; }   /* a page's tick reads the time */
void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    strlcpy(saved_ssid, ssid, sizeof(saved_ssid));
    strlcpy(saved_pass, pass, sizeof(saved_pass));
    saves++;
}
/* The card list's rows for the Muse Gadgets pages (Talk, Passcode, Watch face, Developer, Tests): the home reads
 * whether each shows; opening one fails this focused test. */
bool muse_settings_developer(void) { return false; }
void muse_settings_set_developer(bool v) { (void)v; abort(); }
bool muse_settings_haptics(void) { return true; }
void muse_settings_set_haptics(bool v) { (void)v; abort(); }
muse_talk_mode_t muse_settings_talk_mode(void) { return MUSE_TALK_BOTH; }
void muse_settings_set_talk_mode(muse_talk_mode_t mode) { (void)mode; abort(); }
bool muse_settings_sleep_when_charged(void) { SWEEP_ONLY(); return true; }
void muse_settings_set_sleep_when_charged(bool v) { (void)v; abort(); }
static bool pass_on;
bool muse_passcode_supported(void) { return sweep; }
bool muse_passcode_on(void) { return pass_on; }
muse_pass_lock_t muse_passcode_lock_mode(void) { return MUSE_PASS_LOCK_SCREEN_OFF; }
void muse_passcode_set_lock_mode(muse_pass_lock_t mode) { (void)mode; abort(); }
void muse_passcode_ui_flow(muse_pass_flow_t flow, void (*done)(bool changed)) { (void)flow; (void)done; abort(); }
bool muse_link_face_available(void) { return sweep; }
bool muse_link_face_size_small(void) { SWEEP_ONLY(); return false; }
void muse_link_face_size_set(bool small) { (void)small; abort(); }
void muse_link_face_label(char *out, size_t cap) { SWEEP_ONLY(); strlcpy(out, "My Muse", cap); }
void muse_link_face_pick(bool avatar) { (void)avatar; abort(); }
bool muse_link_face_avatar_picked(void) { SWEEP_ONLY(); return true; }
bool muse_link_image_test(void) { return false; }
bool muse_link_image_ask(char *why, size_t cap) { (void)why; (void)cap; abort(); }
bool muse_link_image_status(char *out, size_t cap) { SWEEP_ONLY(); strlcpy(out, "", cap); return false; }
bool muse_link_sdk_token_fp(unsigned *len, char hex[MUSE_LINK_FP_HEX]) { SWEEP_ONLY(); *len = 0; hex[0] = 0; return false; }
void muse_ble_phone_setup(bool on) { (void)on; abort(); }
bool muse_input_talk_choice(void) { return false; }
bool muse_state_on_battery_real(void) { abort(); }
bool mg_ble_haptic_test_caps(mght_caps_t *c) { (void)c; return false; }   /* no actuator: no Vibration tests */
mght_result_t mg_ble_haptic_test(const mght_cmd_t *c) { (void)c; abort(); }
void mg_ble_haptic_test_status(mght_status_t *st) { (void)st; abort(); }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; SWEEP_ONLY(); return 123456; }
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...) { (void)level; (void)tag; (void)format; }
size_t muse_sim_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t len = strlen(src);
    if (cap) {
        size_t n = len < cap ? len : cap - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

static void on_switch(lv_event_t *event) { (void)event; }
static esp_err_t fake_mic_init(void) { return ESP_OK; }
static void fake_buzz(int ms, int pct) { (void)ms; (void)pct; }
static esp_err_t fake_audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    (void)spk;
    (void)mic;
    return ESP_OK;
}

/* The panel's pixels (RGB565), for the screenshot rows. */
static uint16_t *s_frame;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    int w = area->x2 - area->x1 + 1;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(s_frame + (size_t)y * muse_board->width + area->x1, pixels + (size_t)(y - area->y1) * w * 2,
               (size_t)w * 2);
    }
    lv_display_flush_ready(display);
}

/* The frame as a PPM. */
static void screenshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%d %d\n255\n", muse_board->width, muse_board->height);
    for (size_t i = 0; i < (size_t)muse_board->width * muse_board->height; i++) {
        uint16_t p = s_frame[i];
        uint8_t rgb[3] = { (uint8_t)((p >> 11) * 255 / 31), (uint8_t)(((p >> 5) & 63) * 255 / 63),
                           (uint8_t)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, sizeof(rgb), f);
    }
    assert(fclose(f) == 0);
}

/* Pixels in the box that differ from its top-left one: drawn text. */
static int drawn(lv_area_t b)
{
    int n = 0;
    uint16_t bg = s_frame[(size_t)b.y1 * muse_board->width + b.x1];
    for (int y = b.y1; y <= b.y2; y++) {
        for (int x = b.x1; x <= b.x2; x++) {
            n += s_frame[(size_t)y * muse_board->width + x] != bg;
        }
    }
    return n;
}

static lv_area_t area(lv_obj_t *obj)
{
    lv_area_t bounds;
    lv_obj_update_layout(s_tile);
    lv_obj_get_coords(obj, &bounds);
    return bounds;
}

static void inside(lv_obj_t *obj)
{
    lv_area_t b = area(obj);
    assert(b.x1 >= 0 && b.y1 >= 0);
    assert(b.x2 < muse_board->width && b.y2 < muse_board->height);
    assert(b.x2 >= b.x1 && b.y2 >= b.y1);
}

static bool box_inside(lv_area_t b, lv_area_t in)
{
    return b.x1 >= in.x1 && b.y1 >= in.y1 && b.x2 <= in.x2 && b.y2 <= in.y2;
}

/* Every corner of the box inside the panel's circle. */
static bool in_circle(lv_area_t b)
{
    const int r = muse_board->width / 2;
    const int xs[] = { b.x1, b.x2 + 1 }, ys[] = { b.y1, b.y2 + 1 };
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++) {
            int dx = xs[i] - r, dy = ys[j] - r;
            if (dx * dx + dy * dy > r * r) {
                return false;
            }
        }
    }
    return true;
}

/* One bounded line: a label that wrapped would be taller than its font's line. */
static void one_line_in(lv_obj_t *l, lv_obj_t *card)
{
    lv_area_t b = area(l);
    assert(lv_obj_get_height(l) == lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
    lv_area_t c = area(card);
    c.x1 += lv_obj_get_style_pad_left(card, 0);
    c.x2 -= lv_obj_get_style_pad_right(card, 0);
    assert(box_inside(b, c));
}

static const char *child_text(lv_obj_t *parent, int i)
{
    return lv_label_get_text(lv_obj_get_child(parent, i));
}

/* An info row (info_row()), its value v: where they don't fit side by side (stacked_rows()) the label, then the
 * value on its own line under it, both inside the card and whole (no "..." in place of any letter); else each on
 * one bounded line beside the other. */
static void info_in(lv_obj_t *v, const char *label_text, const char *value)
{
    lv_obj_t *card = lv_obj_get_parent(v), *t = lv_obj_get_child(card, 0);
    if (!stacked_rows()) {
        one_line_in(t, card);
        one_line_in(v, card);
        return;
    }
    lv_area_t a = area(t), b = area(v), c = area(card);
    c.x1 += lv_obj_get_style_pad_left(card, 0);
    c.x2 -= lv_obj_get_style_pad_right(card, 0);
    assert(a.y2 < b.y1);
    assert(box_inside(a, c) && box_inside(b, c));
    assert(!strcmp(lv_label_get_text(t), label_text) && !strcmp(lv_label_get_text(v), value));
}

/* A page (page()): its title in a hit area (child 0), after the back chevron on a sub-page, then its list. */
static const char *page_title(lv_obj_t *p)
{
    const char *t = child_text(lv_obj_get_child(p, 0), 0);
    const char *chevron = LV_SYMBOL_LEFT " ";
    return strncmp(t, chevron, strlen(chevron)) ? t : t + strlen(chevron);
}

static lv_obj_t *page_back(lv_obj_t *p)
{
    return lv_obj_get_user_data(p);   /* the title's hit area, as the bench's "back" */
}

static lv_obj_t *page_list(lv_obj_t *p)
{
    return lv_obj_get_child(p, 1);
}

/* The label's text is this, a line break where a space was counting as the space. */
static bool says(lv_obj_t *l, const char *text)
{
    const char *t = lv_label_get_text(l);
    for (; *t && *text; t++, text++) {
        if (*t != *text && !(*t == '\n' && *text == ' ')) {
            return false;
        }
    }
    return !*t && !*text;
}

/* The label with this text under root, or NULL. */
static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && says(root, text)) {
        return root;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *l = find_label(lv_obj_get_child(root, (int32_t)i), text);
        if (l) {
            return l;
        }
    }
    return NULL;
}

/* "..." replaced some of the label's letters (LV_LABEL_LONG_MODE_DOTS ran out of room). */
static bool cut(lv_obj_t *l)
{
    return ((lv_label_t *)l)->dot_begin != 0xFFFFFFFF;
}

/* The list's child (a card, a note, a column) that holds obj. */
static lv_obj_t *card_of(lv_obj_t *list, lv_obj_t *obj)
{
    while (lv_obj_get_parent(obj) != list) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

/* Every label under o, in the page's list: whole (no "...") and inside its card. */
static void no_cut_under(lv_obj_t *list, lv_obj_t *o, const char *page_name)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        lv_obj_t *card = card_of(list, o);
        lv_area_t a = area(o), c = area(card);
        if (cut(o) || !box_inside(a, c)) {
            fprintf(stderr, "%s: \"%s\" %s (%d,%d %d,%d in %d,%d %d,%d)\n", page_name, lv_label_get_text(o),
                    cut(o) ? "cut" : "outside its card", (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2, (int)c.x1,
                    (int)c.y1, (int)c.x2, (int)c.y2);
            abort();
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        no_cut_under(list, lv_obj_get_child(o, (int32_t)i), page_name);
    }
}

/* A page's title and every label in its list: nothing cut. */
static void no_cut(lv_obj_t *p, const char *page_name)
{
    lv_obj_update_layout(s_tile);
    lv_obj_t *title = lv_obj_get_child(lv_obj_get_child(p, 0), 0);
    assert(!cut(title));
    lv_obj_t *list = page_list(p);
    for (uint32_t i = 0; i < lv_obj_get_child_count(list); i++) {
        no_cut_under(list, lv_obj_get_child(list, (int32_t)i), page_name);
    }
}

/* A row on the page named, as the panel draws it scrolled to the top of the list: its label (text) whole, on its
 * card, drawn. ppm: the screenshot (NULL: none). */
static void shot_row(lv_display_t *display, const char *page_name, const char *text, const char *ppm)
{
    sweep = true;   /* the page's readers answer */
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);   /* the stand-ins: not in the picture */
    lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    muse_settings_ui_build(s_tile);
    muse_settings_ui_open(page_name);
    assert(s_current != s_home);
    lv_obj_t *list = page_list(s_current), *l = find_label(list, text);
    assert(l);
    lv_obj_t *card = card_of(list, l);
    lv_obj_scroll_by_bounded(list, 0, -(area(card).y1 - area(list).y1), LV_ANIM_OFF);
    lv_obj_update_layout(s_tile);
    lv_refr_now(display);
    no_cut(s_current, page_name);
    assert(says(l, text));
    if (test_board.round) {
        assert(in_circle(area(card)));
    } else {
        inside(card);
    }
    assert(drawn(area(l)) > 0);
    if (ppm) {
        screenshot(ppm);
    }
}

static void check_entry(void)
{
    assert(s_current == s_text);
    lv_obj_t *back = lv_obj_get_child(s_text, 0);
    if (narrow_panel()) {
        inside(s_text_title);
        inside(s_text_ta);
        inside(back);
        inside(s_text_kp);
        lv_area_t title = area(s_text_title), field = area(s_text_ta);
        lv_area_t arrow = area(back), keypad = area(s_text_kp);
        assert(title.y1 == 8 && title.y2 < field.y1);
        assert(field.y1 == 36 && field.x1 == 56);
        assert(arrow.x1 == 4 && arrow.x2 < field.x1);
        assert(field.y2 < keypad.y1 && keypad.y1 == 92);
        assert(lv_obj_get_content_height(s_text_ta) >= lv_font_get_line_height(&lv_font_montserrat_20));
    } else {
        /* Existing round and full-keyboard geometry must remain unchanged. */
        lv_area_t field = area(s_text_ta), title = area(s_text_title);
        if (muse_board->diagonal_in < 2.0f) {
            assert(title.y1 == text_y(16));
            assert(field.y1 == text_y(44));
            assert(lv_obj_get_width(s_text_ta) == text_px(224));
            assert(lv_obj_get_width(back) == text_px(48));
        } else if (text_compact()) {
            /* Short or narrow flat panels: arrow and title in one row, then
             * the field, then the keyboard, all on screen and apart. */
            assert(s_text_kb && !s_text_kp);
            inside(s_text_title);
            inside(s_text_ta);
            inside(back);
            inside(s_text_kb);
            lv_area_t arrow = area(back), keys = area(s_text_kb);
            assert(arrow.x2 < title.x1 && arrow.y2 < field.y1 && title.y2 < field.y1);
            assert(field.y1 == TEXT_COMPACT_FIELD_Y && field.y2 < keys.y1);
            assert(lv_obj_get_content_height(s_text_ta) >= lv_font_get_line_height(&lv_font_montserrat_20));
            if (!lv_obj_has_flag(s_text_show, LV_OBJ_FLAG_HIDDEN)) {
                inside(s_text_show);
                lv_area_t show_b = area(s_text_show);
                assert(field.x2 < show_b.x1 && show_b.y1 == field.y1 && show_b.y2 == field.y2);
            }
        } else {
            assert(title.y1 == 40);
            assert(field.y1 == text_y(76));
            assert(lv_obj_get_width(s_text_ta) == text_px(300));
            assert(s_text_kb && !s_text_kp);
            /* Where this layout is kept, it has to fit on the screen. */
            inside(s_text_ta);
            assert(area(back).y2 < field.y1 && field.y2 < area(s_text_kb).y1);
        }
    }
}

static void key(unsigned id)
{
    uint32_t button = id;
    lv_obj_send_event(s_text_kp, LV_EVENT_VALUE_CHANGED, &button);
}

static void enter_digit(void)
{
    key(12);   /* letters -> digits */
    key(1);    /* 2 */
    assert(!strcmp(lv_textarea_get_text(s_text_ta), "2"));
}

int main(int argc, char **argv)
{
    assert(argc == 6 || argc == 7);   /* argv[6]: a scenario's screenshot (PPM) */
    test_board.width = atoi(argv[1]);
    test_board.height = atoi(argv[2]);
    test_board.round = atoi(argv[3]) != 0;
    test_board.touch = true;
    test_board.diagonal_in = (float)atof(argv[4]);
    const char *scenario = argv[5];
    lv_init();
    lv_display_t *display = lv_display_create(test_board.width, test_board.height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    size_t bytes = (size_t)test_board.width * 40 * 2;
    void *buffer = malloc(bytes);
    assert(buffer);
    lv_display_set_buffers(display, buffer, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    s_frame = calloc((size_t)test_board.width * test_board.height, sizeof(*s_frame));
    assert(s_frame);
    s_tile = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_tile);
    lv_obj_set_size(s_tile, lv_pct(100), lv_pct(100));
    s_home = lv_obj_create(s_tile);
    s_wifi = lv_obj_create(s_tile);
    s_current = s_wifi;
    /* As muse_settings_ui_build: the card list's geometry and fonts for this panel. */
    scale_geometry();
    if (test_board.round && test_board.height < 466) {
        s_text_scale = test_board.height;
    }
    const bool tiny = test_board.round && test_board.width < 300;
#if CONFIG_MUSE_AVATAR_SYNC
    if (!strcmp(scenario, "avatar")) {
        build_avatar_page(s_tile);show(s_avatar);lv_obj_update_layout(s_avatar);
        assert(!strcmp(lv_label_get_text(s_avatar_source),"Built-in"));
        assert(lv_obj_has_state(s_avatar_refresh,LV_STATE_DISABLED));
        lv_obj_send_event(row_card(s_avatar_source),LV_EVENT_CLICKED,NULL);
        tick_avatar();assert(avatar_source==MUSE_AVATAR_PERSONAL);
        assert(!lv_obj_has_state(s_avatar_refresh,LV_STATE_DISABLED));
        assert(!strcmp(lv_label_get_text(s_avatar_source),"My Muse"));
        const char *states[]={"Up to date","Using saved avatar","Up to date (not saved)","Waiting for Muse"};
        for(size_t i=0;i<sizeof(states)/sizeof(states[0]);i++) {
            avatar_status_text=states[i];tick_avatar();lv_obj_update_layout(s_avatar);
            if (stacked_rows()) info_in(s_avatar_status,"Status",states[i]);
            else one_line_in(s_avatar_status,lv_obj_get_parent(s_avatar_status));
        }
        lv_obj_t *list=page_list(s_avatar);
        lv_obj_send_event(lv_obj_get_child(list,2),LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_child(list,3),LV_EVENT_CLICKED,NULL);
        assert(avatar_refreshes==1&&avatar_clears==1);
        lv_obj_send_event(row_card(s_avatar_source),LV_EVENT_CLICKED,NULL);
        tick_avatar();assert(avatar_source==MUSE_AVATAR_BUILTIN);
        drop(s_avatar);s_current=NULL;assert(!s_avatar);
    } else
#endif
    if (!strcmp(scenario, "theme-navigation")) {
#if CONFIG_MUSE_UI_THEME
        muse_theme_lvgl_init();
        build_home(s_tile);
        show(s_home);
        for (unsigned visit = 0; visit < 200; ++visit) {
            lv_obj_send_event(row_card(s_home_appearance), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_appearance && lv_obj_is_valid(s_appearance));
            assert(!strcmp(page_title(s_appearance), "APPEARANCE"));
            for (unsigned change = 0; change < 3; ++change) {
#if CONFIG_MUSE_AVATAR_SYNC
                muse_avatar_source_t unchanged_source = avatar_source;
#endif
                muse_theme_mode_t wanted = (muse_theme_mode_t)((muse_theme_mode() + 1) % 3);
                lv_obj_send_event(row_card(s_appearance_mode), LV_EVENT_CLICKED, NULL);
                muse_theme_lvgl_tick();
                muse_settings_ui_tick(true);
                assert(muse_theme_mode() == wanted && theme_setting == wanted);
                assert(!strcmp(lv_label_get_text(s_appearance_mode), appearance_name()));
#if CONFIG_MUSE_AVATAR_SYNC
                assert(avatar_source == unchanged_source);
#endif
            }
            lv_obj_send_event(page_back(s_appearance), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_home);
            lv_tick_inc(1);
            lv_timer_handler();   /* run the production deferred deletion */
            assert(!s_appearance);
#if CONFIG_MUSE_AVATAR_SYNC
            muse_theme_mode_t previous_theme = muse_theme_mode();
            muse_avatar_source_t wanted_source = avatar_source == MUSE_AVATAR_PERSONAL ? MUSE_AVATAR_BUILTIN : MUSE_AVATAR_PERSONAL;
            lv_obj_send_event(row_card(s_home_avatar), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_avatar && lv_obj_is_valid(s_avatar));
            assert(!strcmp(page_title(s_avatar), "AVATAR"));
            lv_obj_send_event(row_card(s_avatar_source), LV_EVENT_CLICKED, NULL);
            muse_settings_ui_tick(true);
            assert(avatar_source == wanted_source && muse_theme_mode() == previous_theme);
            lv_obj_send_event(page_back(s_avatar), LV_EVENT_CLICKED, NULL);
            lv_tick_inc(1);
            lv_timer_handler();
            assert(s_current == s_home && !s_avatar);
#endif
        }
#if LV_USE_SNAPSHOT && CONFIG_MUSE_AVATAR_SYNC
        assert(muse_settings_ui_bench_open("appearance"));bench_open_pending();
        muse_theme_mode_t previous_theme=muse_theme_mode();
        muse_avatar_source_t previous_source=avatar_source;
        assert(!muse_settings_ui_bench_action("nope"));
        assert(muse_settings_ui_bench_action("primary"));
        assert(!muse_settings_ui_bench_action("primary")); /* one pending action */
        bench_action_pending();
        assert(muse_theme_mode()==(muse_theme_mode_t)((previous_theme+1)%3) && avatar_source==previous_source);
        assert(muse_settings_ui_bench_action("back"));bench_action_pending();
        assert(s_current==s_home && !s_appearance);
        assert(muse_settings_ui_bench_open("avatar"));bench_open_pending();
        previous_theme=muse_theme_mode();
        assert(muse_settings_ui_bench_action("primary"));bench_action_pending();
        assert(avatar_source!=previous_source && muse_theme_mode()==previous_theme);
        assert(muse_settings_ui_bench_action("back"));bench_action_pending();
        lv_tick_inc(1);lv_timer_handler();
        assert(s_current==s_home && !s_avatar);
#endif
#else
        abort();
#endif
    } else if (!strcmp(scenario, "volume")) {
        test_board.epaper = true;   /* no microphone meter services */
        s_vol_sl = lv_slider_create(s_tile);
        s_vol_val = lv_label_create(s_tile);
        lv_slider_set_value(s_vol_sl, 30, LV_ANIM_OFF);
        tick_sound();
        assert(lv_slider_get_value(s_vol_sl) == 70);
        assert(!strcmp(lv_label_get_text(s_vol_val), "70%"));
        volume_setting = 25;   /* the avatar gesture changed the saved volume */
        tick_sound();
        assert(lv_slider_get_value(s_vol_sl) == 25);
        assert(!strcmp(lv_label_get_text(s_vol_val), "25%"));
    } else if (!strcmp(scenario, "other")) {
        on_wifi_other(NULL);
        check_entry();
        assert(!lv_textarea_get_password_mode(s_text_ta));
        enter_digit();
        key(14);   /* submit SSID, opening the password page */
        check_entry();
        assert(lv_textarea_get_password_mode(s_text_ta));
        enter_digit();
        key(13);   /* show/hide password */
        assert(!lv_textarea_get_password_mode(s_text_ta));
        key(14);
        assert(saves == 1 && !strcmp(saved_ssid, "2") && !strcmp(saved_pass, "2"));
        assert(s_current == s_wifi);
    } else if (!strcmp(scenario, "secure") || !strcmp(scenario, "saved")) {
        strlcpy(s_aps[0].ssid, "A network with a long name", sizeof(s_aps[0].ssid));
        s_aps[0].secure = true;
        if (!strcmp(scenario, "saved")) {
            s_saved_n = 1;
            strlcpy(s_saved[0].ssid, s_aps[0].ssid, sizeof(s_saved[0].ssid));
        }
        lv_obj_t *ap = lv_button_create(s_wifi);
        lv_obj_add_event_cb(ap, on_wifi_ap, LV_EVENT_CLICKED, (void *)(intptr_t)0);
        lv_obj_send_event(ap, LV_EVENT_CLICKED, NULL);
        check_entry();
        assert(lv_textarea_get_password_mode(s_text_ta));
        enter_digit();
        key(14);
        assert(saves == 1 && !strcmp(saved_ssid, s_aps[0].ssid) && !strcmp(saved_pass, "2"));
    } else if (!strcmp(scenario, "back")) {
        on_wifi_other(NULL);
        check_entry();
        enter_digit();
        lv_obj_t *ta = s_text_ta;
        lv_obj_send_event(lv_obj_get_child(s_text, 0), LV_EVENT_CLICKED, NULL);
        assert(s_current == s_wifi && saves == 0);
        assert(!strcmp(lv_textarea_get_text(ta), ""));
    } else if (!strcmp(scenario, "password")) {
        /* A secured network on the full keyboard: the field and Show button. */
        strlcpy(s_aps[0].ssid, "A network with a long name", sizeof(s_aps[0].ssid));
        s_aps[0].secure = true;
        lv_obj_t *ap = lv_button_create(s_wifi);
        lv_obj_add_event_cb(ap, on_wifi_ap, LV_EVENT_CLICKED, (void *)(intptr_t)0);
        lv_obj_send_event(ap, LV_EVENT_CLICKED, NULL);
        assert(s_text_kb && !lv_obj_has_flag(s_text_show, LV_OBJ_FLAG_HIDDEN));
        assert(lv_textarea_get_password_mode(s_text_ta));
        check_entry();
        lv_obj_send_event(lv_obj_get_child(s_text, 0), LV_EVENT_CLICKED, NULL);
        assert(s_current == s_wifi && saves == 0);
    } else if (!strcmp(scenario, "switch")) {
        /* A switch row's card: the label, then the switch, apart and inside the card. */
        lv_obj_t *list;
        lv_obj_t *p = page(s_tile, "WI-FI", true, &list);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *sw = switch_row(list, "Wi-Fi", true, on_switch);
        lv_obj_t *card = lv_obj_get_parent(sw);
        lv_obj_t *text = lv_obj_get_child(card, 0);
        assert(lv_obj_check_type(text, &lv_label_class) && !strcmp(lv_label_get_text(text), "Wi-Fi"));
        assert(lv_obj_has_state(sw, LV_STATE_CHECKED));
        lv_area_t t = area(text), w = area(sw), c = area(card);
        assert(t.x2 < w.x1 && box_inside(t, c) && box_inside(w, c));
    } else if (!strcmp(scenario, "rows")) {
        /* Long values stay inside their cards: on bounded lines, or whole under their label where the two don't fit
         * side by side. */
        lv_obj_t *list;
        lv_obj_t *p = page(s_tile, "BLUETOOTH", true, &list);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *mac = info_row(list, "MAC address");
        set_text(mac, "02:12:34:56:78:9A");
        info_in(mac, "MAC address", "02:12:34:56:78:9A");
        lv_obj_t *batt = info_row(list, "Also kept awake by a long list of reasons");
        set_text(batt, "Wi-Fi, Bluetooth, voice, the screen and a long tail");
        info_in(batt, "Also kept awake by a long list of reasons", "Wi-Fi, Bluetooth, voice, the screen and a long tail");
        lv_obj_t *card, *value;
        card = row(list, LV_SYMBOL_BLUETOOTH, "Bluetooth", &value, on_switch, NULL);
        set_text(value, "Connected");
        assert(row_card(value) == card);
        if (stacked_rows()) {
            /* The label, then its value under it, whole and inside the card. */
            lv_obj_t *t = find_label(card, "Bluetooth");
            lv_area_t a = area(t), b = area(value), c = area(card);
            assert(t && a.y2 < b.y1 && box_inside(a, c) && box_inside(b, c) && !cut(t) && !cut(value));
        } else {
            one_line_in(lv_obj_get_child(card, 1), card);
            one_line_in(value, card);
        }
        if (tiny) {
            /* The cards and the title, the back chevron in it, stay inside the circle; the title is the back
             * control. */
            assert(in_circle(area(lv_obj_get_parent(mac))));
            lv_obj_t *title = lv_obj_get_child(p, 0);
            assert(page_back(p) == title && lv_obj_has_flag(title, LV_OBJ_FLAG_CLICKABLE));
            assert(!strcmp(page_title(p), "BLUETOOTH"));
            assert(in_circle(area(lv_obj_get_child(title, 0))));
        }
    } else if (!strcmp(scenario, "info")) {
        /* Wi-Fi's MAC address row as the panel draws it, scrolled to the top of the list: where a label and its
         * value don't fit side by side, the address on its own line under the label, whole and on one line;
         * else one line, the address whole. argv[6]: the screenshot. */
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);   /* the stand-in Settings page: not in the picture */
        build_wifi_page(s_tile);
        show(s_wifi);
        lv_obj_t *list = page_list(s_wifi), *mac = NULL;
        for (uint32_t i = 0; i < lv_obj_get_child_count(list) && !mac; i++) {
            lv_obj_t *c = lv_obj_get_child(list, (int32_t)i);
            lv_obj_t *l = lv_obj_get_child_count(c) ? lv_obj_get_child(c, 0) : NULL;
            if (l && lv_obj_check_type(l, &lv_label_class) && !strcmp(lv_label_get_text(l), "MAC address")) {
                mac = lv_obj_get_child(c, 1);
            }
        }
        assert(mac);
        lv_obj_t *card = lv_obj_get_parent(mac);
        lv_obj_scroll_by_bounded(list, 0, -(area(card).y1 - area(list).y1), LV_ANIM_OFF);
        lv_obj_update_layout(s_tile);
        lv_refr_now(display);
        info_in(mac, "MAC address", "02:12:34:56:78:9A");
        assert(!strcmp(lv_label_get_text(mac), "02:12:34:56:78:9A"));
        assert(lv_obj_get_height(mac) == lv_font_get_line_height(lv_obj_get_style_text_font(mac, 0)));
        if (test_board.round) {
            assert(in_circle(area(card)));
        } else {
            inside(card);
        }
        assert(drawn(area(lv_obj_get_child(card, 0))) > 0 && drawn(area(mac)) > 0);
        if (argc == 7) {
            screenshot(argv[6]);
        }
    } else if (!strcmp(scenario, "shot-other")) {
        /* Wi-Fi's "Other network..." row (argv[6]: its screenshot). */
        shot_row(display, "wifi", "Other network...", argc == 7 ? argv[6] : NULL);
    } else if (!strcmp(scenario, "shot-talk")) {
        /* Talk's longest choice, "Screen and button" (argv[6]: its screenshot). */
        test_board.talk_button = "side";
        shot_row(display, "talk", "Screen and button", argc == 7 ? argv[6] : NULL);
    } else if (!strcmp(scenario, "sweep")) {
        /* Every Settings page as this panel draws it, with long names and values: no label cut short with "...",
         * none outside its card. */
        sweep = true;
        test_board.mic_init = fake_mic_init;
        test_board.audio_init = fake_audio_init;
        test_board.power_off = fake_mic_init;
        test_board.talk_button = "side";
        test_board.aux_button = "bottom";
        test_board.buzz = fake_buzz;
        test_board.charged = MUSE_CHARGED_DONE;
        test_board.name = "Waveshare ESP32-S3 Touch AMOLED 1.75";
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
        muse_settings_ui_build(s_tile);
        muse_settings_ui_tick(true);
        no_cut(s_home, "home");
        const char *pages[] = { "sound", "display", "talk", "battery", "bluetooth", "wifi", "muse", "about",
                                "developer", "passcode", "watchface", "tests",
#if CONFIG_MUSE_UI_THEME
                                "appearance",
#endif
#if CONFIG_MUSE_AVATAR_SYNC
                                "avatar",
#endif
        };
        for (int on = 0; on < 2; on++) {
            pass_on = on;   /* Passcode: off, then on */
            for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
                muse_settings_ui_open(pages[i]);
                assert(s_current != s_home);
                muse_settings_ui_tick(true);
                no_cut(s_current, pages[i]);
                muse_settings_ui_go_home();
            }
        }
    } else if (!strcmp(scenario, "screen") || !strcmp(scenario, "mic")) {
        /* Only a board with neither speaker nor microphone loses the mic controls: no Sound row or page, and the
         * brightness on Display for every board. */
        test_board.mic_init = !strcmp(scenario, "mic") ? fake_mic_init : NULL;
        build_home(s_tile);
        show(s_home);
        assert((find_label(page_list(s_home), "Sound") != NULL) == (test_board.mic_init != NULL));
        muse_settings_ui_open("sound");
        if (test_board.mic_init) {
            assert(s_current == s_sound && !strcmp(page_title(s_sound), "Sound") && s_gain_sl && s_mic_bar);
        } else {
            assert(s_current == s_home && !s_sound && !s_gain_sl && !s_mic_bar);
        }
        muse_settings_ui_open("display");
        assert(s_current == s_sleep && !strcmp(page_title(s_sleep), "Display") && s_bright_sl);
        assert(!strcmp(lv_label_get_text(s_bright_val), "60%"));
    } else if (!strcmp(scenario, "bench")) {
        /* Opening a page from text entry drops the entry and the page under it. */
        test_board.talk_button = "screen";
        on_wifi_other(NULL);
        check_entry();
        lv_textarea_set_text(s_text_ta, "2");
        lv_obj_t *ta = s_text_ta;
        build_home(s_tile);   /* Settings, which the bench goes back to first */
        assert(muse_settings_ui_bench_open("battery"));
        assert(!muse_settings_ui_bench_open("nope"));
        bench_open_pending();   /* what the next visible tick does first */
        assert(s_current == s_battery && !s_wifi && !s_text);
        assert(!strcmp(lv_textarea_get_text(ta), ""));
        assert(!strcmp(page_title(s_battery), "Battery"));
        /* Back from the page: Settings, the page dropped. */
        lv_obj_send_event(page_back(s_battery), LV_EVENT_CLICKED, NULL);
        assert(s_current == s_home && !s_battery);
    } else {
        on_wifi_other(NULL);
        check_entry();
    }
#if CONFIG_MUSE_UI_THEME
    if (!strcmp(scenario,"theme")) {
        /* The preceding default case opened a real SSID entry field. */
        lv_textarea_set_text(s_text_ta,"Unfinished network name");
        lv_obj_t *entry=s_text_ta;bool password=lv_textarea_get_password_mode(entry);
        muse_theme_request_mode(MUSE_THEME_LIGHT);muse_theme_lvgl_tick();
        assert(s_text_ta==entry && !strcmp(lv_textarea_get_text(entry),"Unfinished network name"));
        assert(lv_textarea_get_password_mode(entry)==password);
        assert((lv_color_to_u32(lv_obj_get_style_bg_color(s_text,0))&0xffffff)==0xffffff);
        muse_theme_request_mode(MUSE_THEME_DARK);muse_theme_lvgl_tick();
        assert(s_text_ta==entry && !strcmp(lv_textarea_get_text(entry),"Unfinished network name"));
        close_text();
        build_appearance(s_tile);show(s_appearance);tick_appearance();
        assert(!strcmp(lv_label_get_text(s_appearance_mode),"Dark"));
        on_appearance(NULL);muse_theme_lvgl_tick();tick_appearance();
        assert(!strcmp(lv_label_get_text(s_appearance_mode),"Light"));
        on_appearance(NULL);muse_theme_lvgl_tick();tick_appearance();
        assert(!strcmp(lv_label_get_text(s_appearance_mode),"Automatic"));
    }
#endif
    lv_timer_handler();
    lv_display_delete(display);
    lv_deinit();
    free(buffer);
    free(s_frame);
    printf("PASS %dx%d %s\n", test_board.width, test_board.height, scenario);
    return 0;
}
