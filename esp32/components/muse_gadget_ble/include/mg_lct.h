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
 * A live call's start, timed on the gadget (LCT: live call timeline): each
 * step's first time in a call prints once on mg.lct (the Nordic UART mirror,
 * and the console) as
 *
 *   LCT c=<call> <label> <what> t=<esp_timer us> +<ms since T0>
 *
 * (c: the calls timed since boot, the same on every line of one call)
 * with labels the phone apps' timelines share (T0 press, T7 live, T9 first
 * frame shown; the r suffix marks the gadget's receipt of the phone's step).
 * A call the phone started has no T0: its first step is the base. The call's
 * end prints a summary line and starts over:
 *
 *   LCT c=<call> summary (ms since T0) by=<who started it> end=<what ended it>: T0=0 T0r=12 ...
 *
 * by: menu, console, touch, key or phone (a call the phone started); end:
 * End call control, menu, console, Call button, Live call row, phone
 * (ended), phone failed, timeout (no answer), disconnect (the client gone).
 * ESP-Iris's log ring can drop a call's start lines; the summary is what
 * survives.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MG_LCT_T0_PRESS,          /* the user asked for a call (Live call in the UI, >call) */
    MG_LCT_T0R_SEND,          /* the live_call start written to the phone */
    MG_LCT_T1C_CONNECTING,    /* the phone says connecting */
    MG_LCT_T7R_LIVE,          /* the phone says live */
    MG_LCT_TV_CAPS,           /* the phone asked for the video capabilities (answered at once) */
    MG_LCT_TS_START,          /* the phone started the video stream (start_streaming) */
    MG_LCT_T8R_BYTES,         /* the stream's first Data bytes */
    MG_LCT_T8K_KEYFRAME,      /* the decoder has its first keyframe */
    MG_LCT_T8D_DECODED,       /* the first picture decoded */
    MG_LCT_T9_SHOWN,          /* the first picture on the panel */
    MG_LCT_T6S_AUDIO_START,   /* the phone started the reply audio (stream_audio) */
    MG_LCT_T6R_AUDIO_BYTES,   /* its first Data bytes */
    MG_LCT_T6P_AUDIO_PLAYED,  /* its first samples to the speaker */
    MG_LCT_TVOL,              /* the call-start volume raise ran */
    MG_LCT_COUNT
} mg_lct_point_t;

/* T0: a new call's timeline starts now (a call already being timed keeps its
 * own). by: who started it, a static string ("menu", "console", "touch",
 * "key"); mg_lct_begin() says nothing. */
void mg_lct_begin_by(const char *by);
void mg_lct_begin(void);
/* A step: its first time in this call prints. Any task. */
void mg_lct_mark(mg_lct_point_t point);
/* The call ended (either side): the summary prints, with what ended it (a
 * static string: "End call control", "phone", "timeout", "disconnect", ...;
 * mg_lct_end() says nothing), and the next step starts over. */
void mg_lct_end_why(const char *why);
void mg_lct_end(void);
/* The summary line after "LCT c=<call> " (mg_lct_fmt.c, host-tested): "summary
 * (ms since T0) by=<by> end=<end>: T0=0 ..." with each step's ms since base,
 * "-" for one that didn't happen. Returns its length. */
int mg_lct_format_summary(char *out, size_t cap, const int64_t at[MG_LCT_COUNT], int64_t base, const char *by,
                          const char *end);

#ifdef __cplusplus
}
#endif
