/* SPDX-License-Identifier: Apache-2.0 */
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
float muse_voice_monitor_db(void) { abort(); }   /* no meter without a microphone */
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
bool muse_settings_ble_on(void) { abort(); }
int muse_settings_brightness(void) { return 60; }
void muse_settings_hatch_host(char out[MUSE_HOST_MAX + 1]) { (void)out; abort(); }
void muse_settings_hatch_vm(char out[MUSE_VM_MAX + 1]) { (void)out; abort(); }
int muse_settings_mic_gain(void) { return 30; }
void muse_settings_set_ble_on(bool v) { (void)v; abort(); }
void muse_settings_set_brightness(int v) { (void)v; abort(); }
void muse_settings_set_hatch_host(const char *s) { (void)s; abort(); }
esp_err_t muse_settings_set_hatch_token(const char *s, bool v) { (void)s; (void)v; abort(); }
void muse_settings_set_hatch_vm(const char *s) { (void)s; abort(); }
void muse_settings_set_mic_gain(int v) { (void)v; abort(); }
void muse_settings_set_sleep_s(int v) { (void)v; abort(); }
void muse_settings_set_speaker_on(bool v) { (void)v; abort(); }
void muse_settings_set_volume(int v) { (void)v; abort(); }
void muse_settings_set_wifi_on(bool v) { (void)v; abort(); }
int muse_settings_sleep_s(void) { abort(); }
int muse_settings_text_size(void) { return MUSE_TEXT_SMALL; }
void muse_settings_set_text_size(int v) { (void)v; abort(); }
bool muse_settings_speaker_on(void) { return true; }
int muse_settings_volume(void) { return volume_setting; }
bool muse_settings_wifi_on(void) { abort(); }
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
const esp_app_desc_t *esp_app_get_description(void) { static const esp_app_desc_t desc = {.version="test"}; return &desc; }
void muse_wifi_apply(void) { abort(); }
void muse_wifi_forget(const char *ssid) { (void)ssid; abort(); }
int muse_wifi_saved(muse_wifi_saved_t *out, int max) { (void)out; (void)max; abort(); }
int muse_wifi_scan_results(muse_wifi_ap_t *out, int max, uint32_t *gen) { (void)out; (void)max; (void)gen; abort(); }
bool muse_wifi_scanning(void) { abort(); }
void muse_state_set_asleep(bool v) { (void)v; abort(); }
void muse_ui_preview_brightness(int v) { (void)v; abort(); }
void muse_ui_show_face(void) { abort(); }
bool muse_ui_text_size_applies(void) { return true; }
void muse_voice_request_chirp(void) { abort(); }
esp_err_t muse_wifi_scan(void) { abort(); }
esp_err_t esp_read_mac(uint8_t *mac, int type) { (void)mac; (void)type; abort(); }
int64_t esp_timer_get_time(void) { abort(); }
void muse_settings_set_wifi(const char *ssid, const char *pass)
{
    strlcpy(saved_ssid, ssid, sizeof(saved_ssid));
    strlcpy(saved_pass, pass, sizeof(saved_pass));
    saves++;
}
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

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
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
    assert(argc == 6);
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
    s_tile = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_tile);
    lv_obj_set_size(s_tile, lv_pct(100), lv_pct(100));
    s_home = lv_obj_create(s_tile);
    s_wifi = lv_obj_create(s_tile);
    s_current = s_wifi;
    if (test_board.round && test_board.height < 466) {
        s_text_scale = test_board.height;
    }
    s_tiny = test_board.round && test_board.width < 300;   /* as muse_settings_ui_build */
#if CONFIG_MUSE_AVATAR_SYNC
    if (!strcmp(scenario, "avatar")) {
        build_avatar_page(s_tile);show(s_avatar);lv_obj_update_layout(s_avatar);
        assert(!strcmp(lv_label_get_text(s_avatar_source),"Built-in"));
        assert(lv_obj_has_state(s_avatar_refresh,LV_STATE_DISABLED));
        lv_obj_send_event(lv_obj_get_parent(s_avatar_source),LV_EVENT_CLICKED,NULL);
        tick_avatar();assert(avatar_source==MUSE_AVATAR_PERSONAL);
        assert(!lv_obj_has_state(s_avatar_refresh,LV_STATE_DISABLED));
        assert(!strcmp(lv_label_get_text(s_avatar_source),"My Muse"));
        const char *states[]={"Up to date","Using saved avatar","Up to date (not saved)","Waiting for Muse"};
        for(size_t i=0;i<sizeof(states)/sizeof(states[0]);i++) {
            avatar_status_text=states[i];tick_avatar();lv_obj_update_layout(s_avatar);
            one_line_in(s_avatar_status,lv_obj_get_parent(s_avatar_status));
        }
        lv_obj_t *list=lv_obj_get_child(s_avatar,2);
        lv_obj_send_event(lv_obj_get_child(list,2),LV_EVENT_CLICKED,NULL);
        lv_obj_send_event(lv_obj_get_child(list,3),LV_EVENT_CLICKED,NULL);
        assert(avatar_refreshes==1&&avatar_clears==1);
        lv_obj_send_event(lv_obj_get_parent(s_avatar_source),LV_EVENT_CLICKED,NULL);
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
            lv_obj_send_event(lv_obj_get_parent(s_home_appearance), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_appearance && lv_obj_is_valid(s_appearance));
            assert(!strcmp(child_text(s_appearance, 0), "APPEARANCE"));
            for (unsigned change = 0; change < 3; ++change) {
#if CONFIG_MUSE_AVATAR_SYNC
                muse_avatar_source_t unchanged_source = avatar_source;
#endif
                muse_theme_mode_t wanted = (muse_theme_mode_t)((muse_theme_mode() + 1) % 3);
                lv_obj_send_event(lv_obj_get_parent(s_appearance_mode), LV_EVENT_CLICKED, NULL);
                muse_theme_lvgl_tick();
                muse_settings_ui_tick(true);
                assert(muse_theme_mode() == wanted && theme_setting == wanted);
                assert(!strcmp(lv_label_get_text(s_appearance_mode), appearance_name()));
#if CONFIG_MUSE_AVATAR_SYNC
                assert(avatar_source == unchanged_source);
#endif
            }
            lv_obj_send_event(lv_obj_get_child(s_appearance, 1), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_home);
            lv_tick_inc(1);
            lv_timer_handler();   /* run the production deferred deletion */
            assert(!s_appearance);
#if CONFIG_MUSE_AVATAR_SYNC
            muse_theme_mode_t previous_theme = muse_theme_mode();
            muse_avatar_source_t wanted_source = avatar_source == MUSE_AVATAR_PERSONAL ? MUSE_AVATAR_BUILTIN : MUSE_AVATAR_PERSONAL;
            lv_obj_send_event(lv_obj_get_parent(s_home_avatar), LV_EVENT_CLICKED, NULL);
            assert(s_current == s_avatar && lv_obj_is_valid(s_avatar));
            assert(!strcmp(child_text(s_avatar, 0), "AVATAR"));
            lv_obj_send_event(lv_obj_get_parent(s_avatar_source), LV_EVENT_CLICKED, NULL);
            muse_settings_ui_tick(true);
            assert(avatar_source == wanted_source && muse_theme_mode() == previous_theme);
            lv_obj_send_event(lv_obj_get_child(s_avatar, 1), LV_EVENT_CLICKED, NULL);
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
        lv_obj_t *sw = switch_row(s_wifi, "Wi-Fi", true, on_switch);
        lv_obj_t *text = lv_obj_get_child(lv_obj_get_parent(sw), 0);
        assert(lv_label_get_long_mode(text) ==
               (stacked_rows() ? LV_LABEL_LONG_MODE_DOTS : LV_LABEL_LONG_MODE_WRAP));
    } else if (!strcmp(scenario, "rows")) {
        /* Long values stay on bounded lines inside their cards. */
        lv_obj_t *list;
        lv_obj_t *p = page(s_tile, "BLUETOOTH", true, &list);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *mac = info_row(list, "MAC address");
        set_text(mac, "02:12:34:56:78:9A");
        lv_obj_t *card = lv_obj_get_parent(mac);
        one_line_in(lv_obj_get_child(card, 0), card);
        one_line_in(mac, card);
        lv_obj_t *batt = info_row(list, "Also kept awake by a long list of reasons");
        set_text(batt, "Wi-Fi, Bluetooth, voice, the screen and a long tail");
        card = lv_obj_get_parent(batt);
        one_line_in(lv_obj_get_child(card, 0), card);
        one_line_in(batt, card);
        lv_obj_t *value;
        card = row(list, LV_SYMBOL_BLUETOOTH, "Bluetooth", &value, on_switch, NULL);
        set_text(value, "Connected");
        one_line_in(lv_obj_get_child(card, 1), card);
        one_line_in(value, card);
        if (s_tiny) {
            /* The cards and the back arrow stay inside the circle, clear of the title. */
            assert(in_circle(area(lv_obj_get_parent(mac))));
            lv_obj_t *title = lv_obj_get_child(p, 0), *back = lv_obj_get_child(p, 1);
            lv_area_t arrow = area(lv_obj_get_child(back, 0)), t = area(title);
            assert(in_circle(arrow));
            assert(arrow.x2 < t.x1);
            assert(in_circle(t));
        }
    } else if (!strcmp(scenario, "screen") || !strcmp(scenario, "mic")) {
        /* Only a board with neither speaker nor microphone loses the mic controls. */
        test_board.mic_init = !strcmp(scenario, "mic") ? fake_mic_init : NULL;
        build_sound_page(s_tile);
        const char *title = child_text(s_sound, 0);
        if (test_board.mic_init) {
            assert(!strcmp(title, "SOUND") && s_gain_sl && s_mic_bar && s_bright_sl);
        } else {
            assert(!strcmp(title, "SCREEN") && !s_gain_sl && !s_mic_bar && s_bright_sl);
            assert(!strcmp(lv_label_get_text(s_bright_val), "60%"));
            s_current = s_sound;
            tick_sound();   /* must not touch the absent meter */
        }
    } else if (!strcmp(scenario, "bench")) {
        /* Opening a page from text entry drops the entry and the page under it. */
        test_board.talk_button = "screen";
        on_wifi_other(NULL);
        check_entry();
        lv_textarea_set_text(s_text_ta, "2");
        lv_obj_t *ta = s_text_ta;
        assert(muse_settings_ui_bench_open("power"));
        assert(!muse_settings_ui_bench_open("nope"));
        bench_open_pending();   /* what the next visible tick does first */
        assert(s_current == s_power && !s_wifi && !s_text);
        assert(!strcmp(lv_textarea_get_text(ta), ""));
        lv_obj_t *list = lv_obj_get_child(s_power, 2);
        /* The question, then Power off, Restart and Cancel, then how to wake it. */
        assert(lv_obj_get_child_count(list) == 5);
        assert(!strcmp(child_text(lv_obj_get_child(list, 1), 0), LV_SYMBOL_POWER "  Power off"));
        assert(!strcmp(child_text(lv_obj_get_child(list, 2), 0), LV_SYMBOL_REFRESH "  Restart"));
        assert(!strcmp(child_text(lv_obj_get_child(list, 3), 0), "Cancel"));
        const char *wake = child_text(list, (int)lv_obj_get_child_count(list) - 1);
        assert(!strcmp(wake, "Tap the screen to turn it back on."));
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
    printf("PASS %dx%d %s\n", test_board.width, test_board.height, scenario);
    return 0;
}
