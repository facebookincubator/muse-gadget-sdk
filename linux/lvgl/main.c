#define _POSIX_C_SOURCE 200809L
#include <SDL.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "muse_pixel.h"
#include "sim_platform.h"
#include "chat_client.h"
#include "media_client.h"

#define WIDTH 1120
#define HEIGHT 740
#define ART_SIZE 320
static volatile sig_atomic_t quit;
static lv_obj_t *input, *send_button, *mic_button, *camera_button, *history, *reply_label, *status_label, *face_label, *avatar;
static lv_display_t *display;
static lv_image_dsc_t image;
static uint32_t pixels[ART_SIZE * ART_SIZE];
static chat_update_t update;
static bool busy, spinning;
static uint32_t spin_start;
static char token[65], session[65];
static const char *host, *port;
static const char *media_directory;
static bool voice_turn, speech_queued;
static bool recording, capturing;
static char last_reply[CHAT_TEXT_MAX + 1];
static lv_obj_t *message(const char *role, const char *text, bool user);
static void set_busy(bool value);
static bool send_attachment_file(const char *path, bool voice)
{
    if (busy || !token[0]) { lv_label_set_text(status_label, "Wait for Muse, and make sure the connector is online."); return false; }
    FILE *file = fopen(path, "rb");
    if (!file) { lv_label_set_text(status_label, voice ? "Record a voice note first." : "Capture a camera photo first."); return false; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return false; }
    long size = ftell(file);
    if (size < (voice ? 44 : 4) || size > 180000) { fclose(file); lv_label_set_text(status_label, "The media file must fit within 180 KB."); return false; }
    rewind(file);
    unsigned char *raw = malloc((size_t)size);
    char *encoded = malloc(4 * (((size_t)size + 2) / 3) + 2);
    if (!raw || !encoded) { free(raw); free(encoded); fclose(file); return false; }
    bool ok = fread(raw, 1, (size_t)size, file) == (size_t)size; fclose(file);
    if (ok) {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t out = 1; encoded[0] = voice ? '\x1f' : '\x1c';
        for (size_t i = 0; i < (size_t)size; i += 3) {
            uint32_t n = (uint32_t)raw[i] << 16;
            if (i + 1 < (size_t)size) n |= (uint32_t)raw[i + 1] << 8;
            if (i + 2 < (size_t)size) n |= raw[i + 2];
            encoded[out++] = alphabet[(n >> 18) & 63]; encoded[out++] = alphabet[(n >> 12) & 63];
            encoded[out++] = i + 1 < (size_t)size ? alphabet[(n >> 6) & 63] : '=';
            encoded[out++] = i + 2 < (size_t)size ? alphabet[n & 63] : '=';
        }
        encoded[out] = 0;
        ok = chat_start(host, port, token, session, encoded);
    }
    free(raw); free(encoded);
    if (!ok) { lv_label_set_text(status_label, "Could not send the media file."); return false; }
    voice_turn = voice;
    message("You", voice ? "[Voice note sent directly to Muse]" : "[Camera photo sent to Muse for analysis]", true);
    reply_label = message("Muse", voice ? "Listening to your voice note..." : "Analyzing your camera photo...", false);
    set_busy(true);
    lv_label_set_text(status_label, voice ? "WAV voice note sent directly to Muse. Its reply will play automatically." : "Camera JPEG sent to Muse. Analysis will appear in this conversation.");
    return true;
}

static lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }
static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color(rgb), 0);
    return obj;
}
static void flat(lv_obj_t *obj, uint32_t bg)
{
    lv_obj_set_style_bg_color(obj, color(bg), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 20, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
static lv_obj_t *message(const char *role, const char *text, bool user)
{
    while (lv_obj_get_child_count(history) >= 20) lv_obj_delete(lv_obj_get_child(history, 0));
    lv_obj_t *bubble = lv_obj_create(history);
    flat(bubble, user ? 0x25243a : 0x191c28);
    lv_obj_set_width(bubble, LV_PCT(100));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 16, 0);
    lv_obj_set_style_pad_row(bubble, 8, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    label(bubble, role, &lv_font_montserrat_14, user ? 0xb7a4ee : 0xf0d9a4);
    lv_obj_t *body = label(bubble, text, &lv_font_montserrat_16, 0xe8e7ee);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
    return body;
}
static void set_busy(bool value)
{
    busy = value;
    if (value) {
        lv_obj_add_state(send_button, LV_STATE_DISABLED);
        lv_obj_add_state(input, LV_STATE_DISABLED);
        if (mic_button) lv_obj_add_state(mic_button, LV_STATE_DISABLED);
        if (camera_button) lv_obj_add_state(camera_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(send_button, LV_STATE_DISABLED);
        lv_obj_remove_state(input, LV_STATE_DISABLED);
        if (mic_button) lv_obj_remove_state(mic_button, LV_STATE_DISABLED);
        if (camera_button) lv_obj_remove_state(camera_button, LV_STATE_DISABLED);
    }
}
static void capture_photo(lv_event_t *event)
{
    (void)event;
    if (busy || media_busy()) { lv_label_set_text(status_label, "Wait for the current conversation or media operation."); return; }
    if (!token[0]) { lv_label_set_text(status_label, "Connect to your Muse first."); return; }
    media_start("camera", "");
    if (media_busy()) {
        capturing = true; set_busy(true);
        lv_label_set_text(status_label, "Capturing a photo, then uploading it to Muse for analysis...");
    } else lv_label_set_text(status_label, "Camera unavailable. Check the local hardware bridge.");
}
static void record_voice(lv_event_t *event)
{
    (void)event;
    if (busy || media_busy()) { lv_label_set_text(status_label, "Wait for the current conversation or media operation."); return; }
    if (!token[0]) { lv_label_set_text(status_label, "Connect to your Muse first."); return; }
    media_start("microphone", "");
    if (media_busy()) {
        recording = true; set_busy(true);
        lv_label_set_text(status_label, "Recording for 5 seconds. Speak now; your voice note sends automatically.");
    } else lv_label_set_text(status_label, "Microphone unavailable. Check the local hardware bridge.");
}
static void send_message(lv_event_t *event)
{
    (void)event;
    const char *text = lv_textarea_get_text(input);
    if (busy || !text[0]) return;
    if (!token[0]) {
        lv_label_set_text(status_label, "Start the Muse connection bridge to chat.");
        return;
    }
    if (!chat_start(host, port, token, session, text)) {
        lv_label_set_text(status_label, "Could not start the conversation. Try again.");
        return;
    }
    message("You", text, true);
    reply_label = message("Muse", "Thinking...", false);
    lv_textarea_set_text(input, "");
    set_busy(true);
}
static void toggle_spin(lv_event_t *event)
{
    spinning = !spinning;
    spin_start = lv_tick_get();
    lv_image_set_rotation(avatar, 0);
    lv_obj_t *button = lv_event_get_target_obj(event);
    lv_label_set_text(lv_obj_get_child(button, 0), spinning ? "Stop spinning" : "Spin mascot");
}
static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (chat_poll(&update)) {
        lv_label_set_text(status_label, update.status);
        if (reply_label && update.text[0]) {
            lv_label_set_text(reply_label, update.text);
            memcpy(last_reply, update.text, sizeof(last_reply));
            lv_obj_scroll_to_view(lv_obj_get_parent(reply_label), LV_ANIM_OFF);
        }
        if (update.done) {
            if (update.failed && reply_label) {
                lv_label_set_text(reply_label, update.text[0] ? update.text : update.status);
            }
            set_busy(false);
            fprintf(stdout, "%s\n%s\n", update.failed ? "CHAT_ERROR" : "CHAT_DONE",
                    update.failed ? update.status : update.text);
            fflush(stdout);
            if (!update.failed && voice_turn && update.text[0]) speech_queued = true;
            voice_turn = false;
        }
    }
    media_poll();
    if (!media_busy() && speech_queued) {
        speech_queued = false;
        if (!media_start("speak", last_reply)) lv_label_set_text(status_label, "Reply received; speech playback unavailable.");
    }
    float t = lv_tick_get() / 1000.0f;
    muse_pose_t pose = {.t = t, .mode_t = t,
        .mode = recording ? MUSE_MODE_LISTENING : busy ? (update.text[0] ? MUSE_MODE_SPEAKING : MUSE_MODE_THINKING) : MUSE_MODE_IDLE,
        .level = busy && update.text[0] ? (sinf(t * 12) + 1) * 0.2f : 0};
    pose.happy = muse_state_happiness();
    muse_pixel_render(&pose);
    uint16_t row[ART_SIZE];
    for (int y = 0; y < ART_SIZE; y++) {
        muse_pixel_scale(row, ART_SIZE, 0, ART_SIZE - 1, y, y);
        for (int x = 0; x < ART_SIZE; x++) {
            unsigned c = row[x], r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            pixels[y * ART_SIZE + x] = c ? 0xff000000u | ((r * 255 / 31) << 16) |
                ((g * 255 / 63) << 8) | (b * 255 / 31) : 0;
        }
    }
    lv_image_set_rotation(avatar, spinning ? (int32_t)((lv_tick_get() - spin_start) % 3000) * 3600 / 3000 : 0);
    lv_obj_invalidate(avatar);
    lv_label_set_text(face_label, recording ? "Listening" : busy ? (update.text[0] ? "Replying" : "Thinking") : "Ready to talk");
}
static void media_completed(const char *kind, bool ok, const char *text)
{
    if (!strcmp(kind, "camera") || !strcmp(kind, "microphone")) {
        bool voice = !strcmp(kind, "microphone");
        recording = capturing = false; set_busy(false);
        if (!ok) { lv_label_set_text(status_label, text); return; }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", media_directory, voice ? "microphone.wav" : "camera.jpg");
        if (!send_attachment_file(path, voice)) lv_label_set_text(status_label, "Could not upload the capture. Please try again.");
    } else if (!ok) lv_label_set_text(status_label, text);
}
static void build_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    flat(screen, 0x0d1018);
    lv_obj_set_style_radius(screen, 0, 0);
    lv_obj_t *title = label(screen, "Muse", &lv_font_montserrat_28, 0xf3eee3);
    lv_obj_set_pos(title, 30, 24);
    lv_obj_t *sub = label(screen, "Your companion", &lv_font_montserrat_14, 0x9a9baa);
    lv_obj_set_pos(sub, 130, 36);
    lv_obj_t *left = lv_obj_create(screen);
    flat(left, 0x151824); lv_obj_set_pos(left, 24, 84); lv_obj_set_size(left, 380, 630);
    lv_obj_set_style_bg_opa(left, 210, 0);
    lv_obj_set_style_pad_all(left, 20, 0);
    lv_obj_t *name = label(left, "Your Muse", &lv_font_montserrat_20, 0xece6d9);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 14);
    image.header.magic = LV_IMAGE_HEADER_MAGIC; image.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image.header.w = ART_SIZE; image.header.h = ART_SIZE; image.header.stride = ART_SIZE * 4;
    image.data_size = sizeof(pixels); image.data = (const uint8_t *)pixels;
    muse_pixel_set_size(ART_SIZE);
    avatar = lv_image_create(left); lv_image_set_src(avatar, &image);
    lv_image_set_pivot(avatar, ART_SIZE / 2, ART_SIZE / 2);
    lv_obj_align(avatar, LV_ALIGN_CENTER, 0, -20);
    face_label = label(left, "Ready to talk", &lv_font_montserrat_20, 0xf0d9a4);
    lv_obj_align(face_label, LV_ALIGN_BOTTOM_MID, 0, -126);
    lv_obj_t *spin = lv_button_create(left); flat(spin, 0x333047);
    lv_obj_set_size(spin, 190, 48); lv_obj_align(spin, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_center(label(spin, "Spin mascot", &lv_font_montserrat_16, 0xece6f5));
    lv_obj_add_event_cb(spin, toggle_spin, LV_EVENT_CLICKED, NULL);

    lv_obj_t *heading = label(screen, "Conversation", &lv_font_montserrat_20, 0xe8e7ee);
    lv_obj_set_pos(heading, 436, 92);
    status_label = label(screen, "Send a message to connect to your Muse", &lv_font_montserrat_14, 0xaaa7bb);
    lv_obj_set_pos(status_label, 436, 128); lv_obj_set_width(status_label, 644);
    history = lv_obj_create(screen); flat(history, 0x0d1018);
    lv_obj_set_style_bg_opa(history, 170, 0);
    lv_obj_set_pos(history, 422, 166); lv_obj_set_size(history, 678, 454);
    lv_obj_set_style_pad_all(history, 12, 0); lv_obj_set_style_pad_row(history, 14, 0);
    lv_obj_set_flex_flow(history, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(history, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(history, LV_DIR_VER);
    message("Get started", "Send a message below to begin a conversation with your Muse.", false);
    input = lv_textarea_create(screen);
    lv_obj_set_pos(input, 434, 644); lv_obj_set_size(input, 310, 58);
    lv_textarea_set_one_line(input, true); lv_textarea_set_max_length(input, 4096);
    lv_textarea_set_placeholder_text(input, "Message your Muse...");
    lv_obj_set_style_bg_color(input, color(0x1d2030), 0);
    lv_obj_set_style_border_color(input, color(0x41405a), 0);
    lv_obj_set_style_text_color(input, color(0xf1edf5), 0);
    lv_obj_set_style_text_font(input, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(input, send_message, LV_EVENT_READY, NULL);
    camera_button = lv_button_create(screen); flat(camera_button, 0x333047);
    lv_obj_set_pos(camera_button, 758, 644); lv_obj_set_size(camera_button, 94, 58);
    lv_obj_center(label(camera_button, "Camera", &lv_font_montserrat_16, 0xeee7f4));
    lv_obj_add_event_cb(camera_button, capture_photo, LV_EVENT_CLICKED, NULL);
    mic_button = lv_button_create(screen); flat(mic_button, 0x333047);
    lv_obj_set_pos(mic_button, 858, 644); lv_obj_set_size(mic_button, 116, 58);
    lv_obj_center(label(mic_button, "Mic (5s)", &lv_font_montserrat_16, 0xeee7f4));
    lv_obj_add_event_cb(mic_button, record_voice, LV_EVENT_CLICKED, NULL);
    send_button = lv_button_create(screen); flat(send_button, 0xdbc58f);
    lv_obj_set_pos(send_button, 982, 644); lv_obj_set_size(send_button, 108, 58);
    lv_obj_center(label(send_button, "Send", &lv_font_montserrat_16, 0x25202a));
    lv_obj_add_event_cb(send_button, send_message, LV_EVENT_CLICKED, NULL);
    lv_group_t *group = lv_group_create(); lv_group_set_default(group);
    lv_group_add_obj(group, input); lv_group_add_obj(group, camera_button); lv_group_add_obj(group, mic_button); lv_group_add_obj(group, send_button); lv_group_add_obj(group, spin);
    lv_indev_t *keyboard = lv_sdl_keyboard_create(); lv_indev_set_group(keyboard, group);
    lv_sdl_mouse_create(); lv_group_focus_obj(input);
    lv_timer_create(tick, 40, NULL);
}
static int events(void *unused, SDL_Event *event)
{
    (void)unused;
    if (event->type == SDL_QUIT || (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE)) quit = 1;
    return 1;
}
static void signal_handler(int value) { (void)value; quit = 1; }
static bool screenshot(const char *path)
{
    lv_refr_now(display);
    SDL_Renderer *renderer = lv_sdl_window_get_renderer(display);
    uint8_t *rgb = malloc(WIDTH * HEIGHT * 3);
    if (!rgb) return false;
    bool ok = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB24, rgb, WIDTH * 3) == 0;
    FILE *file = ok ? fopen(path, "wb") : NULL;
    if (file) {
        fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
        ok = fwrite(rgb, 3, WIDTH * HEIGHT, file) == WIDTH * HEIGHT;
        if (fclose(file)) ok = false;
    } else ok = false;
    free(rgb); return ok;
}
int main(int argc, char **argv)
{
    bool headless = false;
    const char *shot = NULL, *prompt = NULL, *voice_file = NULL, *camera_file = NULL;
    uint32_t run_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--spin")) spinning = true;
        else if (!strcmp(argv[i], "--voice-file") && i + 1 < argc) voice_file = argv[++i];
        else if (!strcmp(argv[i], "--camera-file") && i + 1 < argc) camera_file = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--prompt") && i + 1 < argc) prompt = argv[++i];
        else if (!strcmp(argv[i], "--run-ms") && i + 1 < argc) {
            char *end; unsigned long value = strtoul(argv[++i], &end, 10);
            if (!argv[i][0] || *end || value > 3600000) return 2;
            run_ms = (uint32_t)value;
        } else { fprintf(stderr, "Usage: %s [--headless] [--spin] [--run-ms N] [--screenshot FILE.ppm] [--prompt TEXT]\n", argv[0]); return 2; }
    }
    if (headless) setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (headless && !run_ms) run_ms = 800;
    host = getenv("MUSE_CHAT_HOST"); if (!host) host = "127.0.0.1";
    port = getenv("MUSE_CHAT_PORT"); if (!port) port = "8765";
    media_directory = getenv("MUSE_MEDIA_DIR"); if (!media_directory) media_directory = "/run/muse/media";
    const char *token_path = getenv("MUSE_CHAT_TOKEN_FILE");
    FILE *file = token_path ? fopen(token_path, "r") : NULL;
    if (file) { if (!fgets(token, sizeof(token), file)) token[0] = 0; fclose(file); token[strcspn(token, "\r\n")] = 0; }
    file = fopen("/dev/urandom", "rb"); uint32_t random[4];
    if (!file) return 1;
    size_t count = fread(random, sizeof(*random), 4, file); fclose(file);
    if (count != 4) return 1;
    snprintf(session, sizeof(session), "%08x-%04x-%04x-%04x-%04x%08x", random[0], random[1] >> 16,
             (random[1] & 0x0fff) | 0x4000, ((random[2] >> 16) & 0x3fff) | 0x8000,
             random[2] & 0xffff, random[3]);
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, signal_handler); signal(SIGINT, signal_handler);
    sim_time_reset(); sim_time_use_realtime(true);
    lv_init(); muse_state_init();
    display = lv_sdl_window_create(WIDTH, HEIGHT);
    if (!display) { fprintf(stderr, "Cannot open the LVGL display: %s\n", SDL_GetError()); return 1; }
    lv_sdl_window_set_title(display, "Muse - Linux LVGL");
    lv_sdl_window_set_resizeable(display, false);
    lv_tick_set_cb(sim_time_tick_ms);
    media_configure(media_completed, media_directory);
    build_ui(); SDL_AddEventWatch(events, NULL);
    if (voice_file && !send_attachment_file(voice_file, true)) return 1;
    if (camera_file && !send_attachment_file(camera_file, false)) return 1;
    if (prompt) { lv_textarea_set_text(input, prompt); send_message(NULL); }
    fprintf(stdout, "Native LVGL app ready (%dx%d).\n", WIDTH, HEIGHT); fflush(stdout);
    uint32_t started = lv_tick_get();
    while (!quit && (!run_ms || lv_tick_get() - started < run_ms)) { lv_timer_handler(); SDL_Delay(5); }
    bool ok = !shot || screenshot(shot);
    chat_stop(); SDL_DelEventWatch(events, NULL); lv_deinit();
    return ok ? 0 : 1;
}
