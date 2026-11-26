#define _POSIX_C_SOURCE 200809L

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

#include <SDL.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"

#include "mgcommands.h"
#include "muse_board.h"
#include "muse_call.h"
#include "muse_home.h"
#include "muse_link.h"
#include "muse_mic_ring.h"
#include "muse_turn_dot.h"
#include "muse_state.h"
#include "muse_settings.h"
#include "muse_passcode.h"
#include "muse_settings_ui.h"
#include "muse_subs.h"
#include "muse_ui.h"
#include "muse_options.h"
#include "mg_haptic_test.h"
#include "esp_heap_caps.h"
#include "sim_board.h"
#include "sim_platform.h"
#include "sim_services.h"

#define FRAME_STEP_MS 5

static volatile sig_atomic_t s_quit;
static bool s_ready;
static float s_level;
static float s_progress;
static muse_power_t s_power = {
    .battery_pct = 72,
    .battery_mv = 3970,
    .charging = false,
    .usb = true,
};
static muse_ble_state_t s_ble_state = MUSE_BLE_CONNECTED;
static uint32_t s_passkey;
static char s_ble_name[32] = "MuseGadget-SIM001";

static void usage(FILE *out, const char *argv0)
{
    fprintf(out,
            "Usage: %s [--headless] [--scenario FILE] [--run-ms N] "
            "[--screenshot FILE.ppm]\n"
            "\n"
            "Scenario lines are key=value. Supported keys:\n"
            "  face=boot|idle|listening|thinking|speaking|error|off|happy\n"
            "  caption=TEXT       level=0..1        progress=0..1\n"
            "  subs=TEXT|off      subs.more=TEXT    subs.ms=N (a muted reply's subtitles)\n"
            "  subs.unproven=true|false (the next subtitles come from an unproven central)\n"
            "  subs.holds=1       (@holds 0|1: the band holds the panel lit)\n"
            "  reconfirm=true|false (link=confirm is a re-setup's prompt: @confirm on Confirm, @dismiss on a swipe)\n"
            "  battery=-1..100    battery_mv=MV     usb=true|false\n"
            "  charging=true|false                  asleep=true|false\n"
            "  wifi=off|no_network|connecting|connected|failed|not_nearby\n"
            "  ble=off|advertising|connected         passkey=0..999999\n"
            "  paired=true|false  link=boot|unpaired|pairing|confirm|connecting|online|offline|error\n"
            "  ble_name=MuseGadget-M5SW-43A0E4 (the advertised name)\n"
            "  speaker=true|false brightness=10..100 advance=MILLISECONDS\n"
            "  image=RRGGBB|hide  (a test picture drawn through muse_ui_image_*)\n"
            "  bench.out=true|false (the bench switch on for the central: \"Bench\" in the top row)\n"
            "  talk=both|screen|button (Settings > Talk)\n"
            "  call=true|false    (a Wi-Fi voice call: muse_voice_in_call, muted reply captions)\n"
            "  call=asked|end|idle|connecting|live|ended|failed|available|unavailable (Live call)\n"
            "  tap=X,Y            (a finger down and up there)\n"
            "  sdk_token=true|false (false: a gadget without its SDK token)\n"
            "  developer=true|false (Developer mode)\n"
            "  avatar_face=FILE.ppm|none (the avatar face: a panel-sized picture as the\n"
            "                      firmware draws it; Settings > Watch face appears)\n"
            "  avatar_face.pick=avatar|pixel  avatar_face.size=large|small (that page's state)\n"
            "  press=NAME         (a tap on a named control: about.fw, developer, dev.switch,\n"
            "                      dev.tests, tests.vibration, vib.pulse<MS>, vib.<preset>,\n"
            "                      vib.stop, vib.minus, vib.plus, watchface, face.pixel,\n"
            "                      face.avatar, face.size)\n"
            "  where=NAME         (where a named control is: @where NAME x,y)\n"
            "  show=NAME          (the named control scrolled into view)\n"
            "  key=any            (a hardware key: stops a vibration test)\n"
            "  vib=WORD           (a console vib word: vib=pulse,50 / vib=preset,ring / vib=stop / vib?)\n"
            "  update=true|false  turn=true|false (the haptics gates)\n"
            "  dump=1             (the settings page on show and its visible labels)\n"
            "  dump=all|sys|every (also the top layer, the system layer, or both)\n"
            "\n"
            "Interactive keys: F1..F7 select face states, H is happy, Space is\n"
            "push-to-talk, +/- change level, [/] change progress, S sleeps,\n"
            "P writes muse-simulator.ppm, Esc quits. Mouse input is touch.\n",
            argv0);
}

static void on_signal(int signum)
{
    (void)signum;
    s_quit = 1;
}

static void render_for(uint32_t duration_ms, bool real_time)
{
    uint32_t elapsed = 0;
    while (!s_quit && elapsed < duration_ms) {
        uint32_t step = duration_ms - elapsed;
        if (step > FRAME_STEP_MS) {
            step = FRAME_STEP_MS;
        }
        sim_time_advance_us((int64_t)step * 1000);
        muse_call_poll();   /* the input task's part: a call no one answers */
        sim_haptics_poll(); /* the haptics engine's timer */
        lv_timer_handler();
        if (real_time) {
            SDL_Delay(step);
        }
        elapsed += step;
    }
}

static bool s_vib_updating, s_vib_turn;

/* Visible on the panel: the object and its parents not hidden, and on screen. */
static bool shown(lv_obj_t *o)
{
    for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p)) {
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) {
            return false;
        }
    }
    return true;
}

/* dump=: every label of the settings page on show (scrolled out of view too),
 * in order, so a test can say which rows a page has. */
static void dump_labels(lv_obj_t *o)
{
    if (!shown(o)) {
        return;
    }
    if (lv_obj_check_type(o, &lv_label_class)) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        if (a.x2 >= 0 && a.x1 < muse_board->width) {   /* the settings tile, not the face beside it */
            printf("@label ");
            for (const char *c = lv_label_get_text(o); *c; c++) {
                fputs(*c == '\n' ? " / " : (char[]){ *c, 0 }, stdout);   /* a line break shows as " / " */
            }
            printf("\n");
        }
    }
    if (lv_obj_check_type(o, &lv_switch_class)) {
        printf("@switch %s\n", lv_obj_has_state(o, LV_STATE_CHECKED) ? "on" : "off");
    }
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        dump_labels(lv_obj_get_child(o, (int32_t)i));
    }
}

/* A finger down at x, y for 80 ms and up, through SDL like the mouse. */
static void tap(int x, int y, bool real_time)
{
    lv_display_t *display = sim_board_display();
    SDL_Window *window = display ? lv_sdl_window_get_window(display) : NULL;
    if (!window) {
        return;
    }
    float zoom = lv_sdl_window_get_zoom(display);
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.windowID = SDL_GetWindowID(window);
    e.motion.x = (int)((float)x * zoom);
    e.motion.y = (int)((float)y * zoom);
    SDL_PushEvent(&e);
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.windowID = SDL_GetWindowID(window);
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = SDL_PRESSED;
    e.button.x = (int)((float)x * zoom);
    e.button.y = (int)((float)y * zoom);
    SDL_PushEvent(&e);
    render_for(80, real_time);
    e.type = SDL_MOUSEBUTTONUP;
    e.button.state = SDL_RELEASED;
    SDL_PushEvent(&e);
    render_for(80, real_time);
}

/* stroke=: a finger down at the first point, through the rest 30 ms apart,
 * lifted at the end if the last item is "up"; left down otherwise, until a
 * later stroke=up lifts it (a hold). */
static bool s_stroke_down;
static int s_stroke_x, s_stroke_y;
static bool stroke(const char *value, bool real_time)
{
    lv_display_t *display = sim_board_display();
    SDL_Window *window = display ? lv_sdl_window_get_window(display) : NULL;
    if (!window) {
        return false;
    }
    float zoom = lv_sdl_window_get_zoom(display);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", value);
    int n = 0, x = 0, y = 0;
    bool up = false;
    if (!strcmp(buf, "up")) {
        /* stroke=up alone: lift a finger an earlier stroke left down, where it was. */
        if (!s_stroke_down) {
            return false;
        }
        x = s_stroke_x, y = s_stroke_y, n = 1, up = true;
    }
    for (char *tok = up ? NULL : strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        if (!strcmp(tok, "up")) {
            up = true;
            break;
        }
        if (sscanf(tok, "%d,%d", &x, &y) != 2) {
            return false;
        }
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = SDL_MOUSEMOTION;
        e.motion.windowID = SDL_GetWindowID(window);
        e.motion.x = (int)((float)x * zoom);
        e.motion.y = (int)((float)y * zoom);
        e.motion.state = n ? SDL_BUTTON_LMASK : 0;
        SDL_PushEvent(&e);
        if (!n) {
            memset(&e, 0, sizeof(e));
            e.type = SDL_MOUSEBUTTONDOWN;
            e.button.windowID = SDL_GetWindowID(window);
            e.button.button = SDL_BUTTON_LEFT;
            e.button.state = SDL_PRESSED;
            e.button.x = (int)((float)x * zoom);
            e.button.y = (int)((float)y * zoom);
            SDL_PushEvent(&e);
        }
        render_for(30, real_time);
        n++;
    }
    s_stroke_down = n > 0 && !up;
    s_stroke_x = x, s_stroke_y = y;
    if (up && n) {
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = SDL_MOUSEBUTTONUP;
        e.button.windowID = SDL_GetWindowID(window);
        e.button.button = SDL_BUTTON_LEFT;
        e.button.state = SDL_RELEASED;
        e.button.x = (int)((float)x * zoom);
        e.button.y = (int)((float)y * zoom);
        SDL_PushEvent(&e);
        render_for(80, real_time);
    }
    return n > 0;
}

/* measure=MS: run MS of simulated time and print how often the screen was
 * redrawn and how much of it (an idle screen should redraw only what
 * changes, e.g. the not-set-up home). */
static unsigned s_refr_frames;
static uint64_t s_refr_px;
static int s_refr_text_bottom;      /* the home's date line's bottom: above it, the top row and the texts */
static unsigned s_refr_above;       /* invalidations entirely above it */
static void on_refr_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_INVALIDATE_AREA) {
        const lv_area_t *a = lv_event_get_param(e);
        if (a) {
            s_refr_px += (uint64_t)lv_area_get_width(a) * (uint64_t)lv_area_get_height(a);
            s_refr_above += a->y2 <= s_refr_text_bottom;
        }
    } else if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        s_refr_frames++;
    }
}

static void measure_refresh(uint32_t ms, bool real_time)
{
    lv_display_t *d = sim_board_display();
    render_for(200, real_time);   /* settle what the scenario changed */
    s_refr_frames = 0;
    s_refr_px = 0;
    s_refr_above = 0;
    muse_home_geom_t g;
    muse_home_geom(lv_display_get_horizontal_resolution(d), lv_display_get_vertical_resolution(d), muse_board->round,
                   muse_board->corner_px, &g);
    s_refr_text_bottom = g.date_top + g.date_caps + 8;
    lv_display_add_event_cb(d, on_refr_event, LV_EVENT_ALL, NULL);
    render_for(ms, real_time);
    lv_display_remove_event_cb_with_user_data(d, on_refr_event, NULL);
    printf("@refresh %u frames in %u ms (%.2f Hz), %llu px invalidated, %u above the Muse\n", s_refr_frames,
           (unsigned)ms, s_refr_frames * 1000.0 / ms, (unsigned long long)s_refr_px, s_refr_above);
    fflush(stdout);
}

static bool write_snapshot(const char *path)
{
    lv_display_t *display = sim_board_display();
    if (!display) {
        fprintf(stderr, "simulator display is not ready\n");
        return false;
    }
    lv_refr_now(display);
    SDL_Renderer *renderer = lv_sdl_window_get_renderer(display);
    int width = 0;
    int height = 0;
    if (!renderer || SDL_GetRendererOutputSize(renderer, &width, &height) != 0
        || width <= 0 || height <= 0) {
        fprintf(stderr, "could not read simulator renderer: %s\n", SDL_GetError());
        return false;
    }
    size_t row_bytes = (size_t)width * 3;
    uint8_t *pixels = malloc(row_bytes * (size_t)height);
    if (!pixels) {
        fprintf(stderr, "could not allocate simulator screenshot\n");
        return false;
    }
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB24, pixels,
                             (int)row_bytes) != 0) {
        fprintf(stderr, "could not capture simulator screen: %s\n", SDL_GetError());
        free(pixels);
        return false;
    }
    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        free(pixels);
        return false;
    }
    fprintf(out, "P6\n%d %d\n255\n", width, height);
    if (fwrite(pixels, row_bytes, (size_t)height, out) != (size_t)height) {
        fprintf(stderr, "%s: write failed\n", path);
        fclose(out);
        free(pixels);
        return false;
    }
    bool ok = fclose(out) == 0;
    free(pixels);
    if (ok) {
        fprintf(stderr, "%s: %dx%d\n", path, width, height);
    }
    return ok;
}

static bool parse_bool(const char *text, bool *out)
{
    if (!strcmp(text, "true") || !strcmp(text, "on") || !strcmp(text, "1")) {
        *out = true;
        return true;
    }
    if (!strcmp(text, "false") || !strcmp(text, "off") || !strcmp(text, "0")) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_long(const char *text, long min, long max, long *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_float(const char *text, float min, float max, float *out)
{
    char *end = NULL;
    errno = 0;
    float value = strtof(text, &end);
    if (errno || end == text || *end || !isfinite(value) || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

static void select_mode(muse_mode_t mode)
{
    /* Production shutdown permits only Idle next. Preview controls should
     * still be able to select any state after showing Off. */
    if (muse_state_mode(NULL) == MUSE_MODE_OFF && mode != MUSE_MODE_OFF) {
        muse_state_set_mode(MUSE_MODE_IDLE);
    }
    muse_state_set_mode(mode);
}

static bool set_face(const char *value)
{
    static const struct {
        const char *name;
        muse_mode_t mode;
    } modes[] = {
        { "boot", MUSE_MODE_BOOT },
        { "idle", MUSE_MODE_IDLE },
        { "listening", MUSE_MODE_LISTENING },
        { "thinking", MUSE_MODE_THINKING },
        { "speaking", MUSE_MODE_SPEAKING },
        { "error", MUSE_MODE_ERROR },
        { "off", MUSE_MODE_OFF },
    };
    if (!strcmp(value, "happy")) {
        select_mode(MUSE_MODE_IDLE);
        muse_state_make_happy();
        return true;
    }
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (!strcmp(value, modes[i].name)) {
            select_mode(modes[i].mode);
            return true;
        }
    }
    return false;
}

static bool set_wifi(const char *value)
{
    static const struct {
        const char *name;
        muse_wifi_state_t state;
    } states[] = {
        { "off", MUSE_WIFI_OFF },
        { "no_network", MUSE_WIFI_NO_NETWORK },
        { "connecting", MUSE_WIFI_CONNECTING },
        { "connected", MUSE_WIFI_CONNECTED },
        { "failed", MUSE_WIFI_FAILED },
        { "not_nearby", MUSE_WIFI_NOT_NEARBY },
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (!strcmp(value, states[i].name)) {
            sim_services_set_wifi(states[i].state,
                                  states[i].state == MUSE_WIFI_CONNECTED ? "Muse Simulator" : "");
            return true;
        }
    }
    return false;
}

static bool set_ble(const char *value)
{
    static const struct {
        const char *name;
        muse_ble_state_t state;
    } states[] = {
        { "off", MUSE_BLE_OFF },
        { "advertising", MUSE_BLE_ADVERTISING },
        { "connected", MUSE_BLE_CONNECTED },
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (!strcmp(value, states[i].name)) {
            s_ble_state = states[i].state;
            sim_services_set_ble(s_ble_state, s_ble_name, s_passkey);
            return true;
        }
    }
    return false;
}

static bool set_link(const char *value)
{
    static const struct {
        const char *name;
        muse_link_state_t state;
    } states[] = {
        { "boot", MUSE_LINK_BOOT },
        { "unpaired", MUSE_LINK_UNPAIRED },
        { "pairing", MUSE_LINK_PAIRING },
        { "confirm", MUSE_LINK_CONFIRM },
        { "connecting", MUSE_LINK_CONNECTING },
        { "online", MUSE_LINK_ONLINE },
        { "offline", MUSE_LINK_OFFLINE },
        { "error", MUSE_LINK_ERROR },
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (!strcmp(value, states[i].name)) {
            sim_services_set_link_state(states[i].state);
            return true;
        }
    }
    return false;
}

static bool draw_test_image(const char *value)
{
    if (!strcmp(value, "hide")) {
        muse_ui_image_hide();
        return true;
    }
    if (strlen(value) != 6) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long rgb = strtoul(value, &end, 16);
    if (errno || end != value + 6) {
        return false;
    }
    int w = 0, h = 0;
    if (!muse_ui_image_size(&w, &h) || w <= 0 || h <= 0) {
        return false;
    }
    unsigned r = (rgb >> 16) & 0xff;
    unsigned g = (rgb >> 8) & 0xff;
    unsigned b = rgb & 0xff;
    uint8_t *row = malloc((size_t)w * 2);
    if (!row) {
        return false;
    }
    bool ok = true;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned shade = (unsigned)((x + y) & 0x1f);
            unsigned rx = r + shade > 255 ? 255 : r + shade;
            unsigned gx = g + shade > 255 ? 255 : g + shade;
            uint16_t px565 = (uint16_t)(((rx >> 3) << 11) | ((gx >> 2) << 5) | (b >> 3));
            row[2 * x] = (uint8_t)(px565 >> 8);
            row[2 * x + 1] = (uint8_t)(px565 & 0xff);
        }
        if (!muse_ui_image_draw(0, y, w, 1, (const uint16_t *)row)) {
            ok = false;
            break;
        }
    }
    free(row);
    if (ok) {
        muse_ui_image_done();
    }
    return ok;
}

/*
 * bench=N: N full-screen redraws with the mic ring off, then on, timed on the
 * host CPU (the ring must cost <= 1 % of a
 * frame). Host numbers: the ratio is what carries over, not the milliseconds.
 */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double bench_frames(int n)
{
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
    }
    return (now_ms() - t0) / n;
}

static void bench_ring(int n)
{
    /* Five rounds, off and on in turn; the fastest of each (a busy host only adds). */
    double off = 1e9, on = 1e9;
    bench_frames(n / 4);   /* warm up */
    for (int round = 0; round < 5; round++) {
        muse_mic_ring_set(false);
        render_for(50, false);
        double t = bench_frames(n);
        off = t < off ? t : off;
        muse_mic_ring_set(true);
        render_for(50, false);
        t = bench_frames(n);
        on = t < on ? t : on;
    }
    /* The ring's own cost, alone: its spans painted over a whole frame. */
    const muse_board_t *b = sim_board_get();
    size_t stride = (size_t)b->width * 2;
    uint8_t *frame = calloc(stride, (size_t)b->height);
    muse_mic_ring_set(true);
    double t0 = now_ms();
    int reps = n * 10;
    for (int i = 0; i < reps; i++) {
        muse_mic_ring_apply(frame, (int)stride, 0, 0, b->width, b->height);
    }
    double paint = (now_ms() - t0) / reps;
    free(frame);
    muse_mic_ring_set(false);
    render_for(100, false);
    printf("bench %s: full redraw %.3f ms ring off, %.3f ms ring on (%+.1f %%, host noise included); "
           "ring spans alone %.4f ms per full frame = %.2f %% of a redraw\n",
           b->name, off, on, (on - off) * 100.0 / off, paint, paint * 100.0 / off);
}

/*
 * dotbench=N: what one breath step of the turn dot costs where it
 * shows (set face=thinking and a page without Muse first): its box
 * invalidated and redrawn, N times, against N full redraws; and its paint
 * alone over its box. The host's flush (SDL's whole-window upload and
 * present, a fixed cost per refresh that a panel doesn't have) is timed and
 * taken out: what's left is LVGL's rendering, whose ratio carries over to a
 * device; the panel's own transfer is in proportion to the pixels sent.
 */
static double s_flush_t0, s_flush_ms;

static void bench_flush_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_START) {
        s_flush_t0 = now_ms();
    } else {
        s_flush_ms += now_ms() - s_flush_t0;
    }
}

static double bench_render(int n, const lv_area_t *area)
{
    s_flush_ms = 0;
    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        if (area) {
            lv_obj_invalidate_area(lv_layer_sys(), area);
        } else {
            lv_obj_invalidate(lv_screen_active());
        }
        lv_refr_now(NULL);
    }
    return (now_ms() - t0 - s_flush_ms) / n;
}

static void bench_dot(int n)
{
    const muse_board_t *b = sim_board_get();
    render_for(100, false);
    int x0, y0, x1, y1;
    muse_turn_dot_box(&x0, &y0, &x1, &y1);
    lv_area_t box = { x0, y0, x1 - 1, y1 - 1 };
    lv_display_t *disp = lv_display_get_default();
    /* After muse_ui's own FLUSH_START handler (the ring's and the dot's paint), so that's in the render. */
    lv_display_add_event_cb(disp, bench_flush_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(disp, bench_flush_cb, LV_EVENT_FLUSH_FINISH, NULL);
    double full = 1e9, step = 1e9;
    bench_render(n / 4, NULL);   /* warm up */
    for (int round = 0; round < 5; round++) {
        double t = bench_render(n, NULL);
        full = t < full ? t : full;
        t = bench_render(n, &box);
        step = t < step ? t : step;
    }
    lv_display_remove_event_cb_with_user_data(disp, bench_flush_cb, NULL);
    size_t stride = (size_t)(x1 - x0) * 2;
    uint8_t *px = calloc(stride, (size_t)(y1 - y0));
    double t0 = now_ms();
    int reps = n * 50;
    for (int i = 0; i < reps; i++) {
        muse_turn_dot_paint(px, (int)stride, x0, y0, x1 - x0, y1 - y0, false);
    }
    double paint = (now_ms() - t0) / reps;
    free(px);
    double pixels = (double)(x1 - x0) * (y1 - y0), all = (double)b->width * b->height;
    printf("dotbench %s: dot %s, box %dx%d at (%d,%d) = %.2f %% of the panel's pixels; rendering (flush taken out): "
           "full redraw %.4f ms, dot step %.4f ms = %.2f %% of a full redraw; its paint alone %.5f ms; "
           "12.5 steps/s = %.3f %% of one full redraw's rendering per second\n",
           b->name, muse_turn_dot_shown() == MUSE_TURN_DOT_OFF ? "OFF" : "shown", x1 - x0, y1 - y0, x0, y0,
           pixels * 100.0 / all, full, step, step * 100.0 / full, paint, 12.5 * step * 100.0 / full);
}

/* subs=TEXT: a muted reply as the phone sends it: state 1 in ms
 * timing, the playout clock from 0, the words subs.ms=N apart (0: all at
 * once), in messages of up to 60 words; subs.more=TEXT: the same reply's
 * next words, shown at once (a reply streaming in, to watch it scroll);
 * subs=off: state 0. */
static int s_subs_ms;
static uint16_t s_subs_reply = 2000;
static uint16_t s_subs_off;
static bool s_subs_gap;   /* the last word had no space after it (a scenario line's end is trimmed) */
static bool s_subs_unproven;   /* subs.unproven=true: from a central whose token proof didn't match */
static void sim_subs_words(const char *text)
{
    if (s_subs_gap) {
        s_subs_off++;   /* the reply's text has the space there: the offsets' gap says so */
    }
    mg_subs_word_t words[60];
    const char *p = text;
    int index = 0;
    while (*p) {
        int count = 0;
        uint16_t off = s_subs_off;
        while (*p && count < 60) {
            const char *e = p;
            while (*e && *e != ' ') e++;
            while (*e == ' ') e++;
            int len = (int)(e - p);
            words[count].utf8 = (const uint8_t *)p;
            words[count].len = (uint8_t)(len > 255 ? 255 : len);
            words[count].show_at = s_subs_ms > 0 ? (uint32_t)(index * s_subs_ms) : 0xFFFFFFFFu;
            s_subs_off = (uint16_t)(s_subs_off + words[count].len);
            s_subs_gap = len == 0 || p[len - 1] != ' ';
            count++;
            index++;
            p = e;
        }
        mg_subs_event_t w = { .sub = 3, .reply_id = s_subs_reply, .flags = 0, .char_offset = off,
                              .count = (uint8_t)count, .words = words, .proven = !s_subs_unproven };
        muse_subs_event(&w);
    }
}

static void sim_subs(const char *text, bool more)
{
    if (!strcmp(text, "off")) {
        mg_subs_event_t st = { .sub = 2, .active = 0, .timing = 1, .proven = !s_subs_unproven };
        muse_subs_event(&st);
        return;
    }
    if (!more) {
        s_subs_reply++;
        s_subs_off = 0;
        s_subs_gap = false;
        mg_subs_event_t st = { .sub = 2, .active = 1, .timing = 1, .proven = !s_subs_unproven };
        muse_subs_event(&st);
    }
    sim_subs_words(text);
    if (!more) {
        mg_subs_event_t clk = { .sub = 6, .reply_id = s_subs_reply, .playout_ms = 0, .proven = !s_subs_unproven };
        muse_subs_event(&clk);
    }
}

static bool apply_setting(const char *key, const char *value, bool real_time)
{
    bool flag;
    long number;
    float scalar;
    if (!strcmp(key, "face")) {
        return set_face(value);
    }
    if (!strcmp(key, "caption")) {
        muse_state_set_caption("%s", value);
        return true;
    }
    if (!strcmp(key, "options")) {
        muse_options_t q = { .selected = -1 };
        snprintf(q.widget_id, sizeof(q.widget_id), "sim-options");
        snprintf(q.message_id, sizeof(q.message_id), "sim-message");
        snprintf(q.question, sizeof(q.question), "Which lunch would you like?");
        const char *p = value;
        while (*p && q.count < MUSE_OPTIONS_MAX) {
            const char *end = strchr(p, '|');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (!len || len >= MUSE_OPTION_BYTES) return false;
            memcpy(q.labels[q.count++], p, len);
            if (!end) return muse_options_publish(&q);
            p = end + 1;
        }
        return false;
    }
    if (!strcmp(key, "level") && parse_float(value, 0.0f, 1.0f, &scalar)) {
        s_level = scalar;
        muse_state_set_level(s_level);
        return true;
    }
    if (!strcmp(key, "progress") && parse_float(value, 0.0f, 1.0f, &scalar)) {
        s_progress = scalar;
        muse_state_set_progress(s_progress);
        return true;
    }
    if (!strcmp(key, "battery") && parse_long(value, -1, 100, &number)) {
        s_power.battery_pct = (int)number;
        muse_state_set_power(&s_power);
        return true;
    }
    if (!strcmp(key, "battery_mv") && parse_long(value, 0, 6000, &number)) {
        s_power.battery_mv = (int)number;
        muse_state_set_power(&s_power);
        return true;
    }
    if (!strcmp(key, "talk")) {
        /* Settings > Talk: both, screen or button (the firmware's default; the simulator's is both). */
        muse_talk_mode_t t = !strcmp(value, "both") ? MUSE_TALK_BOTH : !strcmp(value, "screen") ? MUSE_TALK_SCREEN
                           : !strcmp(value, "button") ? MUSE_TALK_BUTTON : (muse_talk_mode_t)-1;
        if ((int)t < 0) {
            return false;
        }
        muse_settings_set_talk_mode(t);
        return true;
    }
    if (!strcmp(key, "bench.out") && parse_bool(value, &flag)) {
        muse_home_set_bench(flag);   /* the bench switch on for the central: "Bench" in the top row */
        return true;
    }
    if (!strcmp(key, "usb") && parse_bool(value, &flag)) {
        s_power.usb = flag;
        muse_state_set_power(&s_power);
        return true;
    }
    if (!strcmp(key, "charging") && parse_bool(value, &flag)) {
        s_power.charging = flag;
        muse_state_set_power(&s_power);
        return true;
    }
    if (!strcmp(key, "sdk_token") && parse_bool(value, &flag)) {
        extern bool g_sim_no_sdk_token;
        g_sim_no_sdk_token = !flag;
        return true;
    }
    if (!strcmp(key, "asleep") && parse_bool(value, &flag)) {
        muse_state_set_asleep(flag);
        return true;
    }
    if (!strcmp(key, "wifi")) {
        return set_wifi(value);
    }
    if (!strcmp(key, "ssid") && value[0]) {   /* ssid=NAME: connected to that network (the Status row's name) */
        sim_services_set_wifi(MUSE_WIFI_CONNECTED, value);
        return true;
    }
    if (!strcmp(key, "ble")) {
        return set_ble(value);
    }
    if (!strcmp(key, "ble_name") && value[0] && strlen(value) < sizeof(s_ble_name)) {
        snprintf(s_ble_name, sizeof(s_ble_name), "%s", value);
        sim_services_set_ble(s_ble_state, s_ble_name, s_passkey);
        return true;
    }
    if (!strcmp(key, "passkey") && parse_long(value, 0, 999999, &number)) {
        s_passkey = (uint32_t)number;
        sim_services_set_ble(s_ble_state, s_ble_name, s_passkey);
        return true;
    }
    if (!strcmp(key, "paired") && parse_bool(value, &flag)) {
        sim_services_set_paired(flag);
        return true;
    }
    if (!strcmp(key, "link")) {
        return set_link(value);
    }
    if (!strcmp(key, "call") && parse_bool(value, &flag)) {
        sim_services_set_call(flag);
        return true;
    }
    if (!strcmp(key, "speaker") && parse_bool(value, &flag)) {
        sim_services_set_speaker(flag);
        return true;
    }
    if (!strcmp(key, "brightness") && parse_long(value, 10, 100, &number)) {
        sim_services_set_brightness((int)number);
        return true;
    }
    if (!strcmp(key, "image")) {
        return draw_test_image(value);
    }
    if (!strcmp(key, "subs") || !strcmp(key, "subs.more")) {
        sim_subs(value, key[4] == '.');
        return true;
    }
    if (!strcmp(key, "subs.unproven") && parse_bool(value, &flag)) {
        s_subs_unproven = flag;
        return true;
    }
    if (!strcmp(key, "subs.holds")) {
        printf("@holds %d\n", muse_subs_band_holds() ? 1 : 0);
        return true;
    }
    if (!strcmp(key, "reconfirm") && parse_bool(value, &flag)) {
        extern bool g_sim_reconfirm;
        g_sim_reconfirm = flag;
        return true;
    }
    if (!strcmp(key, "subs.ms") && parse_long(value, 0, 10000, &number)) {
        s_subs_ms = (int)number;
        return true;
    }
    if (!strcmp(key, "clock")) {
        /* "10:09", "22:09/24", or "none" (never synced). */
        int hh, mm;
        if (!strcmp(value, "none")) {
            sim_stubs_set_clock(-1, 0, false);
            return true;
        }
        if (sscanf(value, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            sim_stubs_set_clock(hh, mm, strstr(value, "/24") != NULL);
            return true;
        }
        return false;
    }
    if (!strcmp(key, "call")) {
        /* The production muse_call.c: the user asks for or ends a call, the
         * client (the phone) answers with its states, or the client that can
         * take a call goes and comes back. */
        static const struct {
            const char *name;
            int state;
        } CLIENT[] = {
            { "idle", mg_live_call_state_idle },     { "connecting", mg_live_call_state_connecting },
            { "live", mg_live_call_state_live },     { "ended", mg_live_call_state_ended },
            { "failed", mg_live_call_state_failed },
        };
        if (!strcmp(value, "asked")) {
            muse_call_start_from("scenario");
            return true;
        }
        if (!strcmp(value, "end")) {
            muse_call_end();
            return true;
        }
        if (!strcmp(value, "unavailable") || !strcmp(value, "available")) {
            sim_stubs_set_call_available(value[0] == 'a');
            return true;
        }
        for (size_t i = 0; i < sizeof(CLIENT) / sizeof(CLIENT[0]); i++) {
            if (!strcmp(value, CLIENT[i].name)) {
                muse_call_client_state((uint8_t)CLIENT[i].state);
                return true;
            }
        }
        return false;
    }
    if (!strcmp(key, "tap")) {
        int x, y;
        if (sscanf(value, "%d,%d", &x, &y) != 2 || x < 0 || y < 0) {
            return false;
        }
        tap(x, y, real_time);
        return true;
    }
    /* The passcode lock: passcode=DDDD sets a code (on, unlocked),
     * passcode=off turns it off; passlock=true locks as the console's
     * `pass lock` does; stroke=X,Y;X,Y;...[;up] drags a finger through the
     * points 30 ms apart (lifted at the end only with "up"). */
    if (!strcmp(key, "passcode")) {
        if (!strcmp(value, "off")) {
            muse_passcode_turn_off();
        } else if (strlen(value) != 4 || !muse_passcode_set(value)) {
            return false;
        }
        return true;
    }
    if (!strcmp(key, "passlock") && parse_bool(value, &flag)) {
        if (flag) muse_passcode_lock_now();
        return true;
    }
    if (!strcmp(key, "passenter") && strlen(value) == MUSE_PASS_DIGITS) {   /* passenter=DDDD: the keypad's code */
        printf("@passenter %d\n", (int)muse_passcode_check(value));
        return true;
    }
    if (!strcmp(key, "stroke")) {
        return stroke(value, real_time);
    }
    if (!strcmp(key, "developer") && parse_bool(value, &flag)) {
        muse_settings_set_developer(flag);
        sim_stubs_settings_writes = 0;   /* the scenario's own, not the UI's */
        return true;
    }
    if (!strcmp(key, "avatar_face")) {
        return sim_stubs_face_load(value);
    }
    if (!strcmp(key, "avatar_face.pick") && (!strcmp(value, "avatar") || !strcmp(value, "pixel"))) {
        muse_link_face_pick(value[0] == 'a');
        return true;
    }
    if (!strcmp(key, "avatar_face.size") && (!strcmp(value, "large") || !strcmp(value, "small"))) {
        muse_link_face_size_set(value[0] == 's');
        return true;
    }
    if (!strcmp(key, "press")) {
        lv_obj_t *o = muse_settings_ui_find(value);
        if (!o) {
            printf("@press %s: not on the screen now\n", value);
            return true;
        }
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        printf("@press %s\n", value);
        tap((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, real_time);
        return true;
    }
    if (!strcmp(key, "where")) {   /* where=NAME: a named control's center, scrolled into view, untouched */
        lv_obj_t *o = muse_settings_ui_find(value);
        if (!o) {
            printf("@where %s: not on the screen now\n", value);
            return true;
        }
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        printf("@where %s %d,%d\n", value, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
        return true;
    }
    if (!strcmp(key, "show")) {   /* show=NAME: a named control scrolled into view, untouched */
        printf("@show %s%s\n", value, muse_settings_ui_find(value) ? "" : ": not on the screen now");
        return true;
    }
    if (!strcmp(key, "key")) {
        printf("@key %s: %s\n", value, mg_ble_haptic_test_key() ? "stopped a test" : "nothing to stop");
        return true;
    }
    if (!strcmp(key, "vib")) {
        char line[64], out[192];
        snprintf(line, sizeof(line), "vib%s%s", value[0] == '?' ? "" : "=", value);
        mg_ble_haptic_console(line, out, sizeof(out));
        printf("@console %s\n", out);
        return true;
    }
    if (!strcmp(key, "update") && parse_bool(value, &flag)) {
        s_vib_updating = flag;
        sim_haptics_set_gates(s_vib_updating, s_vib_turn);
        return true;
    }
    if (!strcmp(key, "turn") && parse_bool(value, &flag)) {
        s_vib_turn = flag;
        sim_haptics_set_gates(s_vib_updating, s_vib_turn);
        return true;
    }
    if (!strcmp(key, "dump")) {
        lv_timer_handler();
        printf("@page %s\n", muse_settings_ui_page_name());
        dump_labels(lv_screen_active());
        if (!strcmp(value, "all") || !strcmp(value, "every")) {
            dump_labels(lv_layer_top());   /* dump=all: the overlays too (the volume card) */
        }
        if (!strcmp(value, "sys") || !strcmp(value, "every")) {
            dump_labels(lv_layer_sys());   /* dump=sys: the system layer too (the subtitles' tall band) */
        }
        printf("@settings_writes %u\n", sim_stubs_settings_writes);
        return true;
    }
    if (!strcmp(key, "volume") && parse_long(value, 0, 100, &number)) {
        sim_stubs_set_volume((int)number);
        return true;
    }
    /* A speaker change made from outside, as the gadget gets one: the
     * phone's set_settings speaker_volume (0: the speaker off, the level kept;
     * else the level, then on, as mg_glue.c), or the console's vol=N. */
    if ((!strcmp(key, "phone.volume") || !strcmp(key, "bench.volume") || !strcmp(key, "device.volume")) &&
        parse_long(value, 0, 100, &number)) {
        /* device.volume=N: the gadget's own change (a key, a slider), for comparison */
        muse_vol_by_t by = key[0] == 'p' ? MUSE_VOL_BY_PHONE : key[0] == 'b' ? MUSE_VOL_BY_BENCH : MUSE_VOL_BY_DEVICE;
        if (number == 0) {
            muse_settings_set_speaker_on_by(false, by);
        } else {
            muse_settings_set_volume_by((int)number, by);
            muse_settings_set_speaker_on_by(true, by);
        }
        return true;
    }
    if (!strcmp(key, "ring") && parse_bool(value, &flag)) {
        muse_mic_ring_set(flag);   /* the mic captures for the app */
        return true;
    }
    if (!strcmp(key, "dotbench") && parse_long(value, 1, 100000, &number)) {
        bench_dot((int)number);
        return true;
    }
    if (!strcmp(key, "bench") && parse_long(value, 1, 100000, &number)) {
        bench_ring((int)number);
        return true;
    }
    if (!strcmp(key, "page")) {
        muse_ui_goto(value);
        return true;
    }
    if (!strcmp(key, "scroll") && parse_long(value, 0, 100000, &number)) {
        /* scroll=N: the scrollable object under the panel's center (a
         * page's card list) N px down, as a drag up would (lower cards) */
        lv_point_t mid = { muse_board->width / 2, muse_board->height / 2 };
        lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &mid);
        if (!o) o = lv_indev_search_obj(lv_screen_active(), &mid);
        while (o && !(lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_dir(o) == LV_DIR_VER)) {
            o = lv_obj_get_parent(o);
        }
        if (o) {
            lv_obj_scroll_by(o, 0, -(int32_t)number, LV_ANIM_OFF);
        }
        return true;
    }
    if (!strcmp(key, "advance") && parse_long(value, 0, 3600000, &number)) {
        render_for((uint32_t)number, real_time);
        return true;
    }
    if (!strcmp(key, "measure") && parse_long(value, 1, 3600000, &number)) {
        measure_refresh((uint32_t)number, real_time);
        return true;
    }
    return false;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) {
        text[--n] = '\0';
    }
    return text;
}

static bool run_scenario(const char *path, bool real_time)
{
    FILE *in = fopen(path, "r");
    if (!in) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return false;
    }
    char line[1024];
    unsigned line_no = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof(line), in)) {
        line_no++;
        char *entry = trim(line);
        if (!*entry || *entry == '#') {
            continue;
        }
        char *equals = strchr(entry, '=');
        if (!equals) {
            fprintf(stderr, "%s:%u: expected key=value\n", path, line_no);
            ok = false;
            break;
        }
        *equals = '\0';
        char *key = trim(entry);
        char *value = trim(equals + 1);
        if (!apply_setting(key, value, real_time)) {
            fprintf(stderr, "%s:%u: unsupported or invalid setting: %s=%s\n",
                    path, line_no, key, value);
            ok = false;
        }
    }
    if (ferror(in)) {
        fprintf(stderr, "%s: read failed\n", path);
        ok = false;
    }
    fclose(in);
    return ok;
}

static int event_watch(void *userdata, SDL_Event *event)
{
    (void)userdata;
    if (event->type == SDL_QUIT) {
        s_quit = 1;
        return 1;
    }
    if (!s_ready || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP)) {
        return 1;
    }
    bool down = event->type == SDL_KEYDOWN;
    if (down && event->key.repeat) {
        return 1;
    }
    SDL_Keycode key = event->key.keysym.sym;
    if (down && key == SDLK_ESCAPE) {
        s_quit = 1;
    } else if (down && key >= SDLK_F1 && key <= SDLK_F7) {
        static const muse_mode_t modes[] = {
            MUSE_MODE_BOOT, MUSE_MODE_IDLE, MUSE_MODE_LISTENING, MUSE_MODE_THINKING,
            MUSE_MODE_SPEAKING, MUSE_MODE_ERROR, MUSE_MODE_OFF,
        };
        select_mode(modes[key - SDLK_F1]);
    } else if (down && key == SDLK_h) {
        select_mode(MUSE_MODE_IDLE);
        muse_state_make_happy();
    } else if (key == SDLK_SPACE) {
        select_mode(down ? MUSE_MODE_LISTENING : MUSE_MODE_THINKING);
    } else if (down && (key == SDLK_PLUS || key == SDLK_EQUALS || key == SDLK_KP_PLUS)) {
        s_level = s_level < 0.9f ? s_level + 0.1f : 1.0f;
        muse_state_set_level(s_level);
    } else if (down && (key == SDLK_MINUS || key == SDLK_KP_MINUS)) {
        s_level = s_level > 0.1f ? s_level - 0.1f : 0.0f;
        muse_state_set_level(s_level);
    } else if (down && key == SDLK_RIGHTBRACKET) {
        s_progress = s_progress < 0.9f ? s_progress + 0.1f : 1.0f;
        muse_state_set_progress(s_progress);
    } else if (down && key == SDLK_LEFTBRACKET) {
        s_progress = s_progress > 0.1f ? s_progress - 0.1f : 0.0f;
        muse_state_set_progress(s_progress);
    } else if (down && key == SDLK_s) {
        muse_state_set_asleep(!muse_state_asleep());
    } else if (down && key == SDLK_p) {
        (void)write_snapshot("muse-simulator.ppm");
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *scenario = NULL;
    const char *screenshot = NULL;
    uint32_t run_ms = 1000;
    bool headless = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) {
            headless = true;
        } else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) {
            scenario = argv[++i];
        } else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (!strcmp(argv[i], "--run-ms") && i + 1 < argc) {
            long value;
            if (!parse_long(argv[++i], 0, 3600000, &value)) {
                fprintf(stderr, "invalid --run-ms value\n");
                return 2;
            }
            run_ms = (uint32_t)value;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout, argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
            usage(stderr, argv[0]);
            return 2;
        }
    }

    if (headless) {
        (void)setenv("SDL_VIDEODRIVER", "dummy", 1);
        (void)setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    sim_time_reset();
    sim_services_reset();
    lv_init();
    muse_state_init();
    muse_state_set_power(&s_power);
    muse_board = sim_board_get();
    int pre_w = 0, pre_h = 0;
    /* Images need PSRAM: a board without it (the C6) has none, as on the device. */
    bool images = muse_ui_image_size(&pre_w, &pre_h);
    if (images != (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0)
        || (images && (pre_w != muse_board->width || pre_h != muse_board->height))) {
        fprintf(stderr, "muse_ui_image_size failed before muse_ui_start\n");
        return 1;
    }
    if (muse_board->init && muse_board->init() != ESP_OK) {
        fprintf(stderr, "simulator board initialization failed\n");
        return 1;
    }
    muse_passcode_init();   /* no passcode at the start of a run (RAM record) */
    if (muse_ui_start() != ESP_OK) {
        fprintf(stderr, "UI initialization failed\n");
        return 1;
    }
    SDL_AddEventWatch(event_watch, NULL);
    s_ready = true;

    if (scenario && !run_scenario(scenario, !headless)) {
        return 2;
    }
    if (headless || screenshot) {
        render_for(run_ms, false);
        if (screenshot && !write_snapshot(screenshot)) {
            return 1;
        }
    } else {
        while (!s_quit) {
            render_for(FRAME_STEP_MS, true);
        }
    }

    SDL_DelEventWatch(event_watch, NULL);
    lv_deinit();
    return 0;
}
