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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_UI_FREEINK
/* FreeInk boards build without LVGL; its types stay opaque here. */
typedef struct _lv_display_t lv_display_t;
typedef struct _lv_indev_t lv_indev_t;
typedef uint8_t lv_align_t;
#else
#include "lvgl.h"
#endif

#include "muse_charge_dark.h"
#include "muse_charged.h"
#include "muse_state.h"
#if CONFIG_MUSE_MOTION_GESTURES
#include "muse_motion_core.h"
#endif
#include "muse_board_win.h"   /* muse_board_t.draw_windows */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What Muse needs from the hardware. Each file in boards/ fills one of these in
 * and returns it from muse_board_get(). Everything above this line (UI, voice
 * loop, settings, Muse chat) is shared; Wi-Fi and BLE belong to Home Link.
 */

/* Button edges reported by poll_buttons(). */
#define MUSE_BTN_TALK_PRESS   (1u << 0)
#define MUSE_BTN_TALK_RELEASE (1u << 1)
#define MUSE_BTN_AUX_PRESS    (1u << 2)
#define MUSE_BTN_AUX_RELEASE  (1u << 3)
/* Keyboard navigation presses, independent of the two-button controls. */
#define MUSE_BTN_UP           (1u << 4)
#define MUSE_BTN_DOWN         (1u << 5)
#define MUSE_BTN_LEFT         (1u << 6)
#define MUSE_BTN_RIGHT        (1u << 7)
#define MUSE_BTN_ENTER        (1u << 8)
#define MUSE_BTN_ESCAPE       (1u << 9)
/* Dedicated controls; legacy two-button boards keep AUX's existing role.
 * POWER_LONG is a validated hold/PMIC long event, never a fabricated level. */
#define MUSE_BTN_POWER_PRESS  (1u << 10)
#define MUSE_BTN_POWER_RELEASE (1u << 11)
#define MUSE_BTN_POWER_LONG   (1u << 12)
#define MUSE_BTN_SPEAKER_PRESS (1u << 13)
#define MUSE_BTN_SPEAKER_RELEASE (1u << 14)
/* Rotary reply navigation; these are steps, not held buttons. */
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER
#define MUSE_BTN_REVIEW_PREV  (1u << 15)
#define MUSE_BTN_REVIEW_NEXT  (1u << 16)
#endif
/* With TALK_PRESS: reported once a hold passes its threshold (a finger held on
 * the face to talk), not at the press's own edge, so it never confirms pairing. */
#define MUSE_BTN_TALK_HELD    (1u << 17)

/* Where a button's icon goes on screen: beside the button, inside the panel. */
typedef struct {
    lv_align_t align;
    int16_t x, y;
} muse_button_hint_t;

typedef struct {
    const char *name;
    int width, height;
    bool round;             /* circular panel: keep content inside the circle */
    uint16_t corner_px;     /* a rectangular panel's rounded corners' radius (0: square), for the mic ring */
    bool epaper;            /* slow e-paper refresh: settings poll less often and skip live text */
    bool no_ring;           /* square panel: progress as a bar along the bottom edge, no ring around Muse */
    bool touch;             /* no touch: no settings screen, set up over BLE */
    float diagonal_in;      /* screen size; under 2" typing uses a keypad with bigger keys */
    /* The active area's width in mm (round: the diameter), for the menu's
     * millimeter minimums: a menu whose cards or text fall below them grows
     * (muse_settings_ui.c, scale_geometry()). 0: not measured, the pixel layout. */
    double panel_w_mm;
    bool touch_talk;        /* on-screen mic supplies talk edges, alone or beside a talk button */
    bool keyboard;          /* dedicated menu navigation keys */
    const char *talk_button;    /* where the buttons are, for captions: "top" */
    const char *aux_button;     /* "bottom" */
    const char *wake_button;    /* wakes it from power off, if not aux_button: "K0" */
    muse_button_hint_t talk_hint;   /* mic icon; the menu's hints follow both */
    muse_button_hint_t aux_hint;    /* power or menu icon; left out, there's none */
    /* Optional dedicated POWER and speaker controls, independent of AUX. */
    const char *power_button;
    const char *speaker_button;
    muse_button_hint_t power_hint;
    muse_button_hint_t speaker_hint;
    /* 466 px round-panel layout: dedicated 26-36 px rim icons, safe header chord
     * and central read-reply column. Requires all three hint positions/names. */
    bool rim_controls;
    /* The menu turns a right-edge aux button's hint on end, which LVGL draws
     * through a ~14 KB layer; set this where that's too much RAM (the C3) and
     * the hint lies flat on the menu's bar, left of the talk button's. */
    bool flat_menu_hints;
    int frame_ms;           /* face animation period */
    /* A quiet speaker: makeup gain for a musegadgets client's audio
     * (stream_audio) at full volume, tenths of a dB, behind a peak limiter
     * (mg_boost_t). 0: none. */
    int speaker_boost_db10;
    /* A small speaker that needs speech made loud for it: the speech
     * loudness chain (muse_spk_dsp.h: 700 Hz high-pass, compressor, -1 dBFS
     * limiter) on everything it plays, the musegadgets player's replies
     * (stream_audio, push-to-talk and Live call) and the device's own sounds
     * (muse_audio_write). The ESP-Mosaico (its volume curve tops at DAC -5
     * dB, the amp's clip). false: audio reaches the codec as it comes. */
    bool speaker_speech_chain;
    int avatar_px;          /* optional avatar canvas size; 0 uses the UI default */

    /* Power rails, buses, expanders. Runs first. */
    esp_err_t (*init)(void);
    /* SAFE mode's minimum, in place of init (which SAFE skips):
     * whatever keeps the board from resetting or powering itself off with
     * nothing else running (the StopWatch: its power chip's watchdog and I2C
     * sleep off). May be NULL. */
    esp_err_t (*safe_init)(void);
    /* SAFE mode's way out: the talk or boot
     * key's state now, read without init (a raw GPIO, or what safe_init set
     * up), polled every 50 ms; held 5 s, the gadget restarts normally. NULL:
     * no key hold on this board (the consoles still do it). */
    bool (*safe_key)(void);
    /* SAFE mode's dark panel (the C6: an AMOLED keeps its frame
     * across the CPU reset, so a SAFE gadget showed a frozen home): after
     * safe_init, the panel dark without its display init or LVGL: its reset
     * held, its backlight or rail off, a GPIO or one register write.
     * NULL: the board can't (the reTerminal Sticky's e-paper keeps its image
     * unpowered and needs its full display init to change it). */
    void (*safe_display_off)(void);

    /* Starts the panel, LVGL and its task; *touch may be left NULL. */
    lv_display_t *(*display_start)(lv_indev_t **touch);
    bool (*display_lock)(int timeout_ms);   /* -1 waits forever */
    void (*display_unlock)(void);
    void (*set_brightness)(int pct);        /* 0 = off */
    void (*panel_sleep)(bool sleep);        /* screen off; NULL: backlight only */
    /* Screen off with nothing to draw: stops LVGL's task and tick, and touch,
     * so the chip can light-sleep. Buttons still wake it. NULL: LVGL keeps
     * running. */
    void (*display_pause)(bool pause);
    /* FreeInk boards (CONFIG_MUSE_UI_FREEINK): the touch point while a
     * finger is down, in screen coordinates (width x height, as above).
     * FreeInk drives the panel, so these boards leave display_start and the
     * display hooks above NULL. */
    bool (*touch_read)(int16_t *x, int16_t *y);
    /*
     * Video (CONFIG_MUSE_VIDEO): draws a rectangle (end exclusive) straight
     * to the panel, bypassing LVGL, in pieces of `rows` rows that
     * fill(dst, stride, row, rows, arg) writes in the panel's byte order
     * (dst 16-byte aligned). Returns once it's all gone. NULL: no video.
     */
    void (*draw_frame)(int x1, int y1, int x2, int y2, int rows,
                       void (*fill)(uint8_t *dst, int stride, int row, int rows, void *arg), void *arg);
    /* Columns a draw_frame window has to start and end on: a power of two,
     * 4 on the SPD2010 (SenseCAP Watcher). 0: 2, any even column. */
    uint8_t frame_x_align;
    /*
     * Video, optional (changed-only redraw of streamed video,
     * muse_vdirty.h): draws n windows of a picture whose top left is
     * (x0, y0) on the panel, top to bottom, each window's x, y relative to it
     * (x and w on frame_x_align, y and h even), each in pieces of rows rows
     * (the last may be fewer) that fill(dst, stride, x, y, w, rows, arg)
     * writes (x, y relative to (x0, y0) too; dst 16-byte aligned, stride
     * w * 2 bytes), a window's pieces in order; returns once it's all gone.
     * The rest of the panel keeps what it showed. Each piece gets the mic
     * ring and the turn dot as draw_frame's do.
     * panel_writes: a count every write to the panel bumps (LVGL's flushes,
     * each draw_frame and draw_windows call once, a piece given up once
     * more, a panel sleep), so the video knows nothing else drew since its
     * own last picture. NULL (both): every picture drawn whole.
     */
    void (*draw_windows)(int x0, int y0, const muse_board_win_t *win, int n, int rows,
                         void (*fill)(uint8_t *dst, int stride, int x, int y, int w, int rows, void *arg), void *arg);
    uint32_t (*panel_writes)(void);
    /* The stack the panel's own send task never used yet, in bytes (the 2 s av video line's spanel=); NULL: the
     * panel has no task of its own (its writes run on the video's show task). */
    uint32_t (*panel_stack_free)(void);

    /* Speaker codec and mic codec, not yet opened. A mic without a codec (a
     * PDM mic) leaves *mic NULL and is read through mic_read below. */
    esp_err_t (*audio_init)(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic);
    /* A microphone without a codec or speaker (a PDM mic), in place of
     * audio_init: set up once, powered on and off with the audio
     * (muse_audio_power), read as mono at MUSE_AUDIO_RATE.
     * A board with mic_init has no speaker: replies are read, not heard.
     * mic_read also serves a board whose audio_init returns a speaker but
     * no mic codec, which sets the mic up there (mic_init and mic_power
     * stay NULL). muse_audio applies the mic gain to what mic_read returns,
     * in both cases. */
    esp_err_t (*mic_init)(void);
    esp_err_t (*mic_power)(bool on);
    esp_err_t (*mic_read)(int16_t *mono, size_t frames);
    int mic_slot;           /* slot carrying the mic (0/1), or -1 to mix both */
    void (*set_mic_gain)(esp_codec_dev_handle_t mic, int db);   /* NULL: esp_codec_dev_set_in_gain */
    /* set_mic_gain's level 100 (mgcommands.h), dB: the highest
     * capture gain at which normal speech about 30 cm away doesn't clip on
     * this board (level 50 is the tuned gain, MUSE_MIC_GAIN_TUNED; level 1
     * the codec's lowest step, 0 dB). MUSE_MIC_GAIN_TUNED (or 0) where the
     * board has no headroom above it, or until its 30 cm clip check is
     * measured: then 50-100 all capture at the tuned gain. */
    uint8_t mic_gain_speech_max_db;
    /* Audio power the board can switch, such as an amp's enable: on once
     * the codecs are open, off before they close to rest. NULL: none. */
    void (*audio_power)(bool on);
    /* The codecs' own supply, where the board switches it: on before they
     * open (the first time too, before audio_init), off once both have
     * closed to rest. Off, nothing reaches them: muse_audio skips the volume
     * and gain setters until they open again, and reapplies the settings
     * then. NULL: always powered. */
    void (*codec_power)(bool on);
    /* The audio_init I2S channels' dma_desc_num and dma_frame_num where the
     * board changes ESP-IDF's defaults (6 and 240): muse_audio opens the
     * codecs only when the DMA buffers that takes fit (muse_audio_dma.h).
     * 0: the defaults. */
    uint16_t i2s_dma_desc;
    uint16_t i2s_dma_frames;
    /* audio_init leaves both I2S channels set up, their DMA buffers allocated,
     * in the format muse_audio opens them in (16-bit stereo at the DMA sizes
     * above): opening never needs new DMA memory, from the first open on (the
     * StopWatch on battery rests before its first open). */
    bool i2s_ready_at_init;

    /* Called every 10 ms from the input task (50 ms while the display is
     * paused, unless wait_buttons is set); returns MUSE_BTN_* edges. */
    unsigned (*poll_buttons)(void);
    /* Display paused: returns once a button changes (waking the chip from
     * light sleep), the task is notified or timeout_ms passes, so the
     * buttons needn't be polled. NULL: polled. */
    void (*wait_buttons)(int timeout_ms);
#if CONFIG_MUSE_MOTION_GESTURES
    /* Owned by the input task; read returns ESP_ERR_NOT_FINISHED for no new sample. */
    esp_err_t (*motion_enable)(bool enable);
    esp_err_t (*motion_read)(muse_motion_sample_t *out);
#endif
    esp_err_t (*read_power)(muse_power_t *out);
    /* What "charged" is for Display > Sleep when charged
     * (muse_charged.h): MUSE_CHARGED_DONE where read_power fills charge_done
     * from the charger's done or termination status, MUSE_CHARGED_PERCENT
     * where the charger has no done signal (90 % takes, 85 % lets go; listed
     * in esp32/AGENTS.md), MUSE_CHARGED_NONE (the default) where there's no
     * battery reading or no USB sense: the setting is hidden. */
    muse_charged_src_t charged;
    /* Turns the board off; returns only on failure. NULL: it can't from
     * software (a physical switch does), and Settings has no Power off. */
    esp_err_t (*power_off)(void);
    /* power_off works only on battery (USB power holds the board on, as
     * the ESP-Mosaico's PWR_SW latch): offered to a client only then. */
    bool power_off_battery_only;
    /* The power key is still held from the press that switched the board on
     * (the screen says "Let go", since the StopWatch's power chip
     * enters ROM download mode after a 4 s hold). May be NULL. */
    bool (*power_held_from_boot)(void);
    /* Low-battery shutdown: on battery, at or under
     * this many mV for a few readings in a row, Muse powers the board off
     * cleanly before it browns out (muse_battery.h). 0: no automatic
     * shutdown (no trusted voltage reading, or not measured yet). */
    int battery_shutdown_mv;
    /* Charge dark (muse_charge_dark.h). early_power
     * reads the power chip from app_main, before Link, BLE, the display or
     * init(): USB now, whether USB caused this power-up, whether a key did
     * (or is down now), and the battery in mV. It brings up only what that
     * reading needs (init() may reuse it), changes no rail, and logs the
     * power chip's power-on source the first time. early_power_off turns the
     * board off from that state (returns only on failure). The hold boots at
     * battery_shutdown_mv (the board's one low-cell level) plus
     * MUSE_CD_CHARGE_MARGIN_MV on charge; no early_power or no
     * battery_shutdown_mv: no charge dark. */
    esp_err_t (*early_power)(muse_cd_reading_t *out);
    esp_err_t (*early_power_off)(void);
    /* Bench (charge-dark tests): arms the power chip to turn the board on in secs,
     * a way back from a power-off under test. Armed only by the console's
     * power.wake=N; never by the firmware itself. NULL: the chip can't. */
    esp_err_t (*power_on_timer)(int secs);
    /* A vibration motor: buzz for ms at pct strength, without blocking
     * (once at the talk threshold and at setup confirm,
     * honoring haptics). NULL: no motor. */
    void (*buzz)(int ms, int pct);   /* ms <= 0: off at once */
    /* Video without PSRAM: LVGL's draw buffers in strips of
     * `lines` rows (0: the board's own height), swapped under LVGL's lock
     * with no flush in flight. A failure keeps what LVGL has (never no UI);
     * a failed way back up is logged and counted. Returns the lines in use.
     * NULL: fixed buffers. */
    int (*draw_lines)(int lines);
    /* A rectangular panel that video fills (a
     * rectangular panel fills the rectangle): the picture scaled to cover the panel, the
     * overflow cropped evenly (muse_video_fit's fill), rather than two times
     * upscaled and centered with black around it. */
    bool video_fill;
} muse_board_t;

/* The running board, set by muse_app_run(). */
extern const muse_board_t *muse_board;

/* Starts everything and returns; Home Link's app_main() calls it. */
void muse_app_run(const muse_board_t *board);

/* The board selected by CONFIG_MUSE_BOARD_* (boards/). */
const muse_board_t *muse_board_get(void);

/* Debounced GPIO button, for poll_buttons() implementations. */
typedef struct {
    gpio_num_t gpio;
    bool active_high;
    bool pressed;
    uint8_t stable;
} muse_gpio_button_t;

/* Active low, with the internal pull-up where the pad has one. */
esp_err_t muse_gpio_button_init(muse_gpio_button_t *b, gpio_num_t gpio);
/* A SAFE key read raw (muse_board_t.safe_key: no init, from the boot's start): active low, an input with its
 * pull-up turned on at the first read (pull false: the board's own pull-up only, e.g. an ADC ladder's node). */
bool muse_gpio_safe_key(gpio_num_t gpio, bool pull);
/* Drives pin to level and keeps it there (safe_display_off's backlights and
 * resets). Nothing for GPIO_NUM_NC. */
void muse_board_pin_hold(gpio_num_t pin, int level);
/* Active high and driven both ways by the board, so no internal pull. */
esp_err_t muse_gpio_button_init_high(muse_gpio_button_t *b, gpio_num_t gpio);
/* Returns MUSE_BTN_TALK_PRESS / MUSE_BTN_TALK_RELEASE on an edge; shift as needed. */
unsigned muse_gpio_button_poll(muse_gpio_button_t *b);
/* For wait_buttons(): blocks until one of the n buttons leaves its debounced
 * state, the calling task is notified (xTaskNotifyGive) or timeout_ms passes. */
void muse_gpio_buttons_wait(muse_gpio_button_t *const *b, int n, int timeout_ms);

#ifdef __cplusplus
}
#endif
