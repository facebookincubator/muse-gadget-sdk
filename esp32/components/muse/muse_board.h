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
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "lvgl.h"

#include "muse_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * What Muse needs from the hardware. Each file in boards/ fills one of these in
 * and returns it from muse_board_get(). Everything above this line (UI, voice
 * loop, settings, Hatch) is shared; Wi-Fi and BLE belong to Home Link.
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

/* Where a button's icon goes on screen: beside the button, inside the panel. */
typedef struct {
    lv_align_t align;
    int16_t x, y;
} muse_button_hint_t;

typedef struct {
    const char *name;
    int width, height;
    bool round;             /* circular panel: keep content inside the circle */
    bool touch;             /* no touch: no settings screen, set up over BLE */
    float diagonal_in;      /* screen size; under 2" typing uses a keypad with bigger keys */
    bool touch_talk;        /* on-screen mic supplies talk edges; no physical talk button */
    bool keyboard;          /* dedicated menu navigation keys */
    const char *talk_button;    /* where the buttons are, for captions: "top" */
    const char *aux_button;     /* "bottom" */
    muse_button_hint_t talk_hint;   /* mic icon; the menu's hints follow both */
    muse_button_hint_t aux_hint;    /* power or menu icon; left out, there's none */
    /* The menu turns a right-edge aux button's hint on end, which LVGL draws
     * through a ~14 KB layer; set this where that's too much RAM (the C3) and
     * the hint lies flat on the menu's bar, left of the talk button's. */
    bool flat_menu_hints;
    int frame_ms;           /* face animation period */
    int avatar_px;          /* optional avatar canvas size; 0 uses the UI default */

    /* Power rails, buses, expanders. Runs first. */
    esp_err_t (*init)(void);

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

    /* Codec handles for one duplex, 2-slot I2S bus, not yet opened. */
    esp_err_t (*audio_init)(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic);
    int mic_slot;           /* slot carrying the mic (0/1), or -1 to mix both */
    void (*set_mic_gain)(esp_codec_dev_handle_t mic, int db);   /* NULL: esp_codec_dev_set_in_gain */

    /* Called every 10 ms from the input task (50 ms while the display is
     * paused, unless wait_buttons is set); returns MUSE_BTN_* edges. */
    unsigned (*poll_buttons)(void);
    /* Display paused: returns once a button changes (waking the chip from
     * light sleep), the task is notified or timeout_ms passes, so the
     * buttons needn't be polled. NULL: polled. */
    void (*wait_buttons)(int timeout_ms);
    esp_err_t (*read_power)(muse_power_t *out);
    /* Turns the board off; returns only on failure. */
    esp_err_t (*power_off)(void);
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
