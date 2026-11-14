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

/*
 * Muse's chat, drawn as an app: the conversation in bubbles in a scrolling
 * column, Muse's reply growing as it's written, and what Muse is doing at the
 * top (a pulsing dot while it listens, a spinner while it works). It follows
 * the chat through the gadget (chat_open, then EV_CHAT), so replies to the
 * Muse app on the phone show up too.
 *
 * The talk button still talks to Muse (muse_talk_key), with what Muse hears
 * shown at the bottom; the second button, or the button on screen, asks Muse
 * for a joke (chat_send), and the app shows that message like the others. A
 * drag scrolls back; new text scrolls into view again after a few seconds.
 * The talk button doesn't close it, so the close mark at the top (touch) or a
 * long press of the second button does.
 * Build from the SDK folder: python3 build.py examples/chat.c
 */
#include <string.h>

#include "../gadget.h"

GADGET_MANIFEST("{\"api\":\"1.0\",\"name\":\"chat\",\"title\":\"Chat\",\"icon\":\"chat\",\"color\":\"blue\",\"uses\":[\"chat.read\",\"chat.send\"]}");

#define BG 0x121214
#define PANEL 0x17171C
#define RING 0x23232B
#define SAGE 0x5FA87D
#define CORAL 0xC05A50
#define SLATE 0x5E7FA6
#define LAV 0x8E8CA8
#define PALE 0xD8D5CC
#define VIOLET 0x7E639F

#define SHOWN_MAX 8          /* the gadget keeps the last 8 messages for an app */
#define HOLD_MS 8000         /* after a drag, new text waits this long to scroll into view */
#define CAPTION_TIMER 1
#define JOKE "Tell me a short joke, in two or three sentences."

typedef struct {
    int32_t id, row, bubble, label;
} shown_t;

static shown_t shown[SHOWN_MAX];
static int32_t n_shown;
static int32_t column, tail, dot, spinner, mode_label, footer, joke_button, close_button;
static int32_t st_plain, st_bubble;
static int32_t font, text_max;
static int32_t hold_until, holding;
static int32_t mode, busy, online = 1, send_op;
static const char *hint;
static char text[4096];      /* one message: the gadget keeps up to 4 KB of each */
static char caption[160];

/* ---- Messages ---- */

static void fill(shown_t *s)
{
    int32_t role = chat_info(s->id, CHAT_INFO_ROLE);
    int32_t n = chat_text(s->id, 0, text, sizeof text - 4);
    if (n < 0) {
        return;
    }
    if (chat_info(s->id, CHAT_INFO_TRUNCATED) == 1) {
        memcpy(text + n, "...", 4);
    } else if (n == 0) {
        memcpy(text, "...", 4);   /* a reply that hasn't started */
    }
    int32_t w = ui_text_width(font, text);
    ui_set(s->label, P_WIDTH, w < text_max ? w + 1 : text_max);
    ui_set_text(s->label, P_TEXT, text);
    ui_set(s->row, P_FLEX_MAIN_PLACE, role == ROLE_MUSE ? PLACE_START : PLACE_END);
    ui_set(s->bubble, P_BG_COLOR, role == ROLE_USER ? SLATE : role == ROLE_APP ? VIOLET : PANEL);
    ui_set(s->bubble, P_BG_OPA, role == ROLE_MUSE ? OPA_COVER : 110);
    ui_set(s->bubble, P_BORDER_WIDTH, role == ROLE_MUSE ? 1 : 0);
}

static int32_t index_of(int32_t id)
{
    for (int32_t i = 0; i < n_shown; i++) {
        if (shown[i].id == id) {
            return i;
        }
    }
    return -1;
}

static int32_t plain(int32_t type, int32_t parent)
{
    int32_t o = ui_create(type, parent);
    ui_style_add(o, st_plain, PART_MAIN);
    ui_set(o, P_SCROLLABLE, 0);
    ui_set(o, P_CLICKABLE, 0);   /* presses go through to the column, which scrolls */
    return o;
}

/* A bubble for message id, at place `at` among those shown. */
static void insert(int32_t id, int32_t at)
{
    if (n_shown == SHOWN_MAX) {
        return;
    }
    shown_t s = {.id = id};
    s.row = plain(W_OBJ, column);
    ui_set(s.row, P_WIDTH, PCT(100));
    ui_set(s.row, P_HEIGHT, SIZE_CONTENT);
    ui_set(s.row, P_LAYOUT, LAYOUT_FLEX);
    ui_set(s.row, P_FLEX_FLOW, FLEX_ROW);
    s.bubble = ui_create(W_OBJ, s.row);
    ui_style_add(s.bubble, st_bubble, PART_MAIN);
    ui_set(s.bubble, P_WIDTH, SIZE_CONTENT);
    ui_set(s.bubble, P_HEIGHT, SIZE_CONTENT);
    ui_set(s.bubble, P_SCROLLABLE, 0);
    ui_set(s.bubble, P_CLICKABLE, 0);
    s.label = ui_create(W_LABEL, s.bubble);
    ui_set(s.label, P_LONG_MODE, LONG_WRAP);
    ui_set(s.label, P_TEXT_FONT, font);
    ui_set(s.label, P_TEXT_COLOR, PALE);
    ui_set(s.row, P_INDEX, at);   /* rows come in the chat's order, the tail after them */
    memmove(&shown[at + 1], &shown[at], (size_t)(n_shown - at) * sizeof shown[0]);
    shown[at] = s;
    n_shown++;
    fill(&shown[at]);
}

/* Brings the bubbles in line with the messages the gadget keeps: drops those
 * it no longer has, adds new ones in their place. */
static void sync(void)
{
    int32_t ids[SHOWN_MAX], n = 0;
    for (int32_t k = SHOWN_MAX - 1; k >= 0; k--) {   /* oldest first */
        int32_t id = chat_latest(k);
        if (id > 0) {
            ids[n++] = id;
        }
    }
    for (int32_t i = 0; i < n_shown;) {
        int32_t keep = 0;
        for (int32_t k = 0; k < n && !keep; k++) {
            keep = ids[k] == shown[i].id;
        }
        if (keep) {
            i++;
            continue;
        }
        ui_delete(shown[i].row);
        memmove(&shown[i], &shown[i + 1], (size_t)(n_shown - i - 1) * sizeof shown[0]);
        n_shown--;
    }
    for (int32_t k = 0; k < n; k++) {   /* those before it are shown by now: it goes k-th */
        if (index_of(ids[k]) < 0) {
            insert(ids[k], k);
        }
    }
}

static void scroll_down(void)
{
    if (holding && sys_millis() - hold_until < 0) {
        return;   /* the person is reading further up */
    }
    holding = 0;
    ui_scroll_to_view(tail, 1);
}

/* ---- What Muse is doing ---- */

static void show_footer(void)
{
    int32_t heard = (mode == MUSE_LISTENING || mode == MUSE_ERROR) && muse_caption(caption, sizeof caption) > 0;
    ui_set_text(footer, P_TEXT, heard ? caption : hint);
    ui_set(footer, P_TEXT_COLOR, mode == MUSE_ERROR ? CORAL : heard ? PALE : LAV);
}

static void show_mode(void)
{
    static const char *const names[] = {"Ready", "Listening", "Thinking", "Speaking", "Something went wrong"};
    int32_t working = online && (mode == MUSE_THINKING || (mode == MUSE_IDLE && busy));
    if (mode < MUSE_IDLE || mode > MUSE_ERROR) {
        mode = MUSE_IDLE;
    }
    const char *name = !online ? "Offline" : working && mode == MUSE_IDLE ? "Working" : names[mode];
    int32_t color = !online ? RING : mode == MUSE_LISTENING || mode == MUSE_ERROR ? CORAL
                  : mode == MUSE_SPEAKING ? SAGE : LAV;
    ui_set_text(mode_label, P_TEXT, name);
    ui_set(mode_label, P_TEXT_COLOR, color == RING ? LAV : color);
    ui_set(dot, P_BG_COLOR, color);
    ui_set(dot, P_HIDDEN, working);
    ui_set(spinner, P_HIDDEN, !working);
    if (mode == MUSE_LISTENING) {
        ui_animate(dot, P_OPA, 60, 500, EASE_IN_OUT | ANIM_REPEAT | ANIM_REVERSE);
        sys_timer(CAPTION_TIMER, 250, 1);   /* what Muse hears, as it hears it */
    } else {
        ui_animate_stop(dot, P_OPA);
        ui_set(dot, P_OPA, OPA_COVER);
        sys_timer_stop(CAPTION_TIMER);
    }
    show_footer();
}

static void ask_for_a_joke(void)
{
    send_op = chat_send(JOKE);
    if (send_op < 0) {
        ui_set_text(footer, P_TEXT, send_op == ERR_OFFLINE ? "Offline" : "Couldn't send");
    }
}

/* ---- The app ---- */

GADGET_EXPORT(on_start) void on_start(void)
{
    int32_t w = sys_info(INFO_SCREEN_WIDTH);
    int32_t inset = sys_info(INFO_SAFE_INSET);
    int32_t pad = inset > 8 ? inset : 8;
    int32_t buttons = sys_info(INFO_BUTTONS);
    font = w >= 400 ? FONT_20 : FONT_16;
    text_max = (w - 2 * pad - 2 * 12) * 85 / 100;   /* a bubble's text: most of the column */
    ui_set(UI_ROOT, P_BG_COLOR, BG);

    st_plain = ui_style_create();
    ui_style_set(st_plain, P_BG_OPA, OPA_TRANSP);
    ui_style_set(st_plain, P_BORDER_WIDTH, 0);
    ui_style_set(st_plain, P_PAD_ALL, 0);
    st_bubble = ui_style_create();
    ui_style_set(st_bubble, P_RADIUS, 16);
    ui_style_set(st_bubble, P_PAD_ALL, 12);
    ui_style_set(st_bubble, P_BORDER_COLOR, RING);

    /* A column filling the screen, inside what a round screen shows: the
     * header, the messages, then the footer. */
    int32_t screen = plain(W_OBJ, UI_ROOT);
    ui_set(screen, P_WIDTH, PCT(100));
    ui_set(screen, P_HEIGHT, PCT(100));
    ui_set(screen, P_PAD_ALL, pad);
    ui_set(screen, P_PAD_ROW, 8);
    ui_set(screen, P_LAYOUT, LAYOUT_FLEX);
    ui_set(screen, P_FLEX_FLOW, FLEX_COLUMN);
    ui_set(screen, P_FLEX_CROSS_PLACE, PLACE_CENTER);

    int32_t header = plain(W_OBJ, screen);
    ui_set(header, P_WIDTH, PCT(100));
    ui_set(header, P_HEIGHT, SIZE_CONTENT);
    ui_set(header, P_LAYOUT, LAYOUT_FLEX);
    ui_set(header, P_FLEX_FLOW, FLEX_ROW);
    ui_set(header, P_FLEX_MAIN_PLACE, PLACE_CENTER);
    ui_set(header, P_FLEX_CROSS_PLACE, PLACE_CENTER);
    ui_set(header, P_PAD_COLUMN, 8);
    dot = plain(W_OBJ, header);
    ui_set(dot, P_WIDTH, 10);
    ui_set(dot, P_HEIGHT, 10);
    ui_set(dot, P_RADIUS, RADIUS_CIRCLE);
    ui_set(dot, P_BG_OPA, OPA_COVER);
    spinner = ui_create(W_SPINNER, header);
    ui_set(spinner, P_WIDTH, 16);
    ui_set(spinner, P_HEIGHT, 16);
    ui_set(spinner, P_ARC_WIDTH, 3);
    ui_set(spinner, P_ARC_COLOR, RING);
    ui_style(spinner, PART_INDICATOR, P_ARC_WIDTH, 3);
    ui_style(spinner, PART_INDICATOR, P_ARC_COLOR, SLATE);
    mode_label = ui_create(W_LABEL, header);
    /* The talk button is Muse's here, so it doesn't close the app: this does
     * on a touch screen, and a long press of the second button. */
    if (sys_info(INFO_TOUCH) == 1) {
        close_button = ui_create(W_LABEL, header);
        ui_set_text(close_button, P_TEXT, SYM_CLOSE);
        ui_set(close_button, P_TEXT_COLOR, PALE);
        ui_set(close_button, P_CLICKABLE, 1);
        ui_on(close_button, UI_CLICKED);
    }

    column = ui_create(W_OBJ, screen);
    ui_style_add(column, st_plain, PART_MAIN);
    ui_set(column, P_WIDTH, PCT(100));
    ui_set(column, P_FLEX_GROW, 1);
    ui_set(column, P_LAYOUT, LAYOUT_FLEX);
    ui_set(column, P_FLEX_FLOW, FLEX_COLUMN);
    ui_set(column, P_PAD_ROW, 8);
    ui_set(column, P_SCROLL_DIR, DIR_VER);
    ui_set(column, P_SCROLLBAR_MODE, SCROLLBAR_ACTIVE);
    ui_on(column, UI_PRESSED);
    tail = plain(W_OBJ, column);   /* stays last: scrolling to it scrolls to the end */
    ui_set(tail, P_WIDTH, 1);
    ui_set(tail, P_HEIGHT, 1);

    int32_t second = buttons & (1 << BUTTON_SECONDARY);
    if (sys_info(INFO_TOUCH) == 1 && !second) {
        joke_button = ui_create(W_BUTTON, screen);
        ui_set(joke_button, P_BG_COLOR, PANEL);
        ui_set(joke_button, P_BORDER_COLOR, RING);
        ui_set(joke_button, P_BORDER_WIDTH, 1);
        ui_set(joke_button, P_RADIUS, RADIUS_CIRCLE);
        ui_on(joke_button, UI_CLICKED);
        int32_t l = ui_create(W_LABEL, joke_button);
        ui_set_text(l, P_TEXT, SYM_ENVELOPE "  A joke");
        ui_set(l, P_TEXT_COLOR, PALE);
    }
    hint = second ? "Talk button: talk to Muse. Second button: a joke, hold to close."
                  : "Hold the talk button to talk to Muse.";
    footer = ui_create(W_LABEL, screen);
    ui_set(footer, P_WIDTH, PCT(100));
    ui_set(footer, P_LONG_MODE, LONG_WRAP);
    ui_set(footer, P_TEXT_ALIGN, TEXT_ALIGN_CENTER);
    ui_set(footer, P_TEXT_FONT, FONT_14);

    muse_talk_key(1);   /* the talk button stays Muse's push-to-talk */
    mode = muse_mode();
    show_mode();
    if (chat_open() < 0) {
        ui_set_text(footer, P_TEXT, "Can't follow the chat");
    }
}

GADGET_EXPORT(on_event) void on_event(int32_t type, int32_t a, int32_t b)
{
    switch (type) {
    case EV_CHAT:
        if (a == CHAT_BUSY) {
            busy = b;
            show_mode();
        } else if (a == CHAT_CONNECTED) {
            online = b;
            show_mode();
            if (b) {
                sync();
                scroll_down();
            }
        } else {   /* CHAT_MESSAGE, CHAT_TEXT, CHAT_DONE: b is the message */
            int32_t i = index_of(b);
            if (i < 0) {
                sync();
            } else {
                fill(&shown[i]);
            }
            if (a == CHAT_MESSAGE) {
                holding = 0;   /* a new message: follow the chat again */
            }
            scroll_down();
        }
        break;
    case EV_MUSE:
        mode = a;
        show_mode();
        break;
    case EV_CONNECTION:
        online = a;
        show_mode();
        break;
    case EV_TIMER:
        show_footer();
        break;
    case EV_UI:
        if (a == column && b == UI_PRESSED) {
            holding = 1;
            hold_until = sys_millis() + HOLD_MS;
        } else if (a == joke_button && b == UI_CLICKED) {
            ask_for_a_joke();
        } else if (a == close_button && b == UI_CLICKED) {
            sys_exit();
        }
        break;
    case EV_BUTTON:
        if (a == BUTTON_SECONDARY && b == BTN_UP) {
            ask_for_a_joke();
        } else if (a == BUTTON_SECONDARY && b == BTN_LONG) {
            sys_exit();   /* the app stops: no UP follows */
        }
        break;
    case EV_DONE:
        if (a == send_op && b < 0) {
            ui_set_text(footer, P_TEXT, "Muse didn't take the message");
            sys_log("chat: chat_send failed");
        }
        break;
    }
}

GADGET_EXPORT(on_stop) void on_stop(int32_t reason)
{
    (void)reason;
    chat_close();
}
