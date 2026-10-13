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

typedef enum {
    LED_STATE_BOOT,
    LED_STATE_SETUP_IDLE,
    LED_STATE_BLE_ADVERTISING,
    LED_STATE_BLE_CONNECTED,
    LED_STATE_PAIRING_CONFIRM_REQUIRED,
    LED_STATE_WIFI_CONNECTING,
    LED_STATE_WIFI_CONNECTED,
    LED_STATE_AUTH_OK,
    LED_STATE_VM_SWITCHING,
    LED_STATE_VM_OK,
    LED_STATE_WS_CONNECTED,
    LED_STATE_WS_DISCONNECTED,
    LED_STATE_UNPAIRED,
    LED_STATE_ERROR,
} led_state_t;

bool led_status_init(void);
void led_status_set_state(led_state_t state);
// Show a short title (the agent's name) on backends with a display; NULL or ""
// clears it. Other backends ignore it.
void led_status_set_title(const char *title);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G
// Save and request the next clockwise orientation of the status screen.
void epaper_154g_status_rotate(void);
#if CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G && CONFIG_HOMEHUB_VOICE
void epaper_154g_status_next_page(void);
#endif
// The status screen's battery strip: -1 means no reading.
void epaper_154g_status_set_battery(int percent);
#endif

// Display backends only: the screen size in pixels. Returns false without a
// display.
bool led_status_display_info(int *width, int *height);
// Bits per pixel the screen shows: 16 for the RGB565 colour LCDs, 4 for
// six-colour e-paper, 2 for four-colour e-paper, 1 for black and white, 0 without
// a display.
int led_status_display_bits(void);
// Draw w x h pixels at (x, y). `pixels` holds RGB565, 2 bytes each with the
// high byte first, left to right and top to bottom; at most 23 full rows'
// worth per call. The first call replaces the animation and title until
// led_status_show_animation(); the status bars and dot stay on top. E-paper
// dithers colour to its inks and shows it only at led_status_draw_done().
bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels);
// The image is complete: e-paper refreshes now, and returns once it has
// (a second or two in black and white, about 30 s in six colours, about 20 s
// in four). LCDs have shown each draw already.
void led_status_draw_done(void);
// Clear the image and bring back the animation and title.
void led_status_show_animation(void);

// What the voice chat is doing, shown on the LED ring in place of the
// connection status until it is back to idle. An error flashes red three
// times and then returns to idle by itself.
typedef enum {
    LED_VOICE_IDLE,
    LED_VOICE_LISTENING,     // blue level meter (led_status_set_level)
    LED_VOICE_TRANSCRIBING,  // amber comet
    LED_VOICE_THINKING,      // purple comet: waiting for the agent
    LED_VOICE_BUFFERING,     // green comet: the reply is being synthesised
    LED_VOICE_SPEAKING,      // green breathing
    LED_VOICE_ERROR,
} led_voice_t;

void led_status_set_voice(led_voice_t voice);
// Microphone level for LED_VOICE_LISTENING, 0 to 1.
void led_status_set_level(float level);
// Show the speaker volume (0 to 100) on the ring for a moment, over whatever
// it shows.
void led_status_show_volume(int percent);
