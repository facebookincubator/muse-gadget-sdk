/* SPDX-License-Identifier: Apache-2.0 */
/* The button menu's real LVGL layout on a screen size, in English or (built
 * with CONFIG_MUSE_UI_LANG_VI) Vietnamese: the title clears the list, pages
 * stay above the hints and every line of a long page can be reached. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the private views visible without adding firmware test APIs. */
#include "../../components/muse/muse_menu.c"

static muse_board_t test_board = {
    .talk_button = "talk",
    .aux_button = "aux",
    .talk_hint = { .align = LV_ALIGN_BOTTOM_RIGHT },
    .aux_hint = { .align = LV_ALIGN_BOTTOM_LEFT },
};
const muse_board_t *muse_board = &test_board;
static int power_offs, resets;

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = { .version = "1.2.3-45-gabcdef0" };
    return &desc;
}
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    (void)level;
    (void)tag;
    (void)format;
}
int64_t esp_timer_get_time(void) { static int64_t t; return t += 2000000; }
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    (void)length;
    (void)item_size;
    static StaticQueue_t *q;
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t) { (void)q; (void)item; (void)t; return 0; }
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t) { (void)q; (void)item; (void)t; return 0; }

void muse_battery_read(muse_battery_t *b)
{
    memset(b, 0, sizeof(*b));
    b->started = b->running = true;
    b->secs = 3 * 3600 + 25 * 60;
    b->pct_start = 100;
    b->pct_now = 62;
    b->screen_off_pm = 801;
    b->slept_pm = 734;
    b->sleeps = 1234;
    b->busy_pm = 52;
}
bool muse_battery_drain(const muse_battery_t *b, int *rate10, int *full_h)
{
    (void)b;
    *rate10 = 112;
    *full_h = 9;
    return true;
}
void muse_ble_status(muse_ble_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = MUSE_BLE_ADVERTISING;
    strlcpy(out->name, "MuseGadget-A1B2C3", sizeof(out->name));
}
void muse_hatch_status(muse_hatch_status_t *out) { memset(out, 0, sizeof(*out)); out->state = MUSE_HATCH_REACHABLE; }
const char *muse_hatch_state_name(muse_hatch_state_t s) { (void)s; return "Online"; }
muse_link_state_t muse_link_state(void) { return (muse_link_state_t)0; }
const char *muse_link_state_name(muse_link_state_t s) { (void)s; return "Connected"; }
void muse_wifi_status(muse_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = MUSE_WIFI_CONNECTED;
    strlcpy(out->ssid, "Nhà Minh Tầng 2", sizeof(out->ssid));
    strlcpy(out->ip, "192.168.100.123", sizeof(out->ip));
}
muse_power_t muse_state_power(void) { return (muse_power_t){ .battery_pct = 62 }; }
void muse_state_set_caption(const char *fmt, ...) { (void)fmt; }
void muse_state_set_asleep(bool v) { (void)v; }
void muse_input_request_power_off(void) { power_offs++; }
void muse_link_reset_setup(void) { resets++; }
void muse_voice_request_chirp(void) {}
int muse_settings_volume(void) { return 70; }
bool muse_settings_speaker_on(void) { return true; }
int muse_settings_brightness(void) { return 80; }
int muse_settings_mic_gain(void) { return 24; }
int muse_settings_sleep_s(void) { return 60; }
bool muse_settings_ble_on(void) { return false; }
bool muse_settings_wifi_on(void) { return true; }
void muse_settings_set_volume(int v) { (void)v; }
void muse_settings_set_speaker_on(bool v) { (void)v; }
void muse_settings_set_brightness(int v) { (void)v; }
void muse_settings_set_mic_gain(int v) { (void)v; }
void muse_settings_set_sleep_s(int v) { (void)v; }
void muse_settings_set_ble_on(bool v) { (void)v; }
void muse_settings_set_wifi_on(bool v) { (void)v; }
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

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static lv_area_t coords(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_update_layout(s_root);
    lv_obj_get_coords(obj, &a);
    return a;
}

static int rule_top;   /* the hints' rule */

/* Steps a page from its top to its end with Down: every line is in the window
 * at some step, Down never leaves while there's more, then Down goes back. */
static int read_page(view_t view)
{
    show(view);
    assert(s_view == view);
    lv_area_t box = coords(s_page_box), title = coords(s_title);
    assert(box.y1 >= title.y2 && box.y2 < rule_top);
    assert(title.x1 >= 0 && title.x2 < test_board.width);   /* the whole title shows */
    int line = lv_font_get_line_height(lv_obj_get_style_text_font(s_page, 0)) +
               lv_obj_get_style_text_line_space(s_page, 0);
    int window = lv_obj_get_content_height(s_page_box);
    assert(window >= line);
    int steps = 0, seen = 0;
    if (getenv("MENU_LAYOUT_VERBOSE")) {
        lv_obj_update_layout(s_page_box);
        printf("--- window %d, page %d, line %d\n%s\n", window, (int)lv_obj_get_height(s_page), line,
               lv_label_get_text(s_page));
    }
    for (;;) {
        lv_obj_update_layout(s_page_box);
        int y = lv_obj_get_scroll_y(s_page_box), more = lv_obj_get_scroll_bottom(s_page_box);
        assert(y >= 0 && y <= seen);   /* no line skipped */
        seen = y + window - line;      /* every line wholly above this has been shown whole */
        if (more <= 0) {
            break;
        }
        assert(!strcmp(lv_label_get_text(s_hint_down), muse_lang_menu(s_down_text)));
        handle(MUSE_MENU_DOWN);
        assert(s_view == view && ++steps < 32);
    }
    handle(MUSE_MENU_DOWN);
    assert(s_view == VIEW_LIST);
    return steps;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    test_board.width = atoi(argv[1]);
    test_board.height = atoi(argv[2]);
    lv_init();
    lv_display_t *display = lv_display_create(test_board.width, test_board.height);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    size_t bytes = (size_t)test_board.width * 40 * 2;
    void *buffer = malloc(bytes);
    assert(buffer);
    lv_display_set_buffers(display, buffer, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

#if CONFIG_MUSE_LATIN_FONT
    muse_text_keep_latin();   /* as muse_app_run() does */
#endif
    muse_menu_build(lv_screen_active(), test_board.width, test_board.height);
    bool small = test_board.height < 200 || test_board.width < 200;
    rule_top = test_board.height - (small ? 17 : 44);
    handle(MUSE_MENU_DOWN);   /* opens the menu */
    assert(s_view == VIEW_LIST);
    lv_area_t title = coords(s_title), list = coords(s_list);
    assert(title.y1 >= 0 && title.y2 < list.y1);
    assert(list.y2 < rule_top);

    int status = read_page(VIEW_STATUS), battery = read_page(VIEW_BATTERY);
    s_view = VIEW_LIST;
    int power = read_page(VIEW_POWER), reset = read_page(VIEW_RESET);
    assert(!power_offs && !resets);   /* an arrow never confirms */
    printf("%dx%d: status %d, battery %d, power %d, reset %d scrolls\n", test_board.width, test_board.height,
           status, battery, power, reset);
#if CONFIG_MUSE_UI_LANG_VI
    /* The case that overflowed: a 128 px screen's pages, 16 px a line. */
    assert(lv_obj_get_style_text_font(s_page, 0) != s_fine);
    if (test_board.height <= 128) {
        assert(status > 0 && battery > 0);
    }
#endif
    return 0;
}
