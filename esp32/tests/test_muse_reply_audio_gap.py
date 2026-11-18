# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Run the production reply loop with audio gaps, resumption and interruption."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReplyAudioGap(unittest.TestCase):
    def test_reply_loop(self):
        source = (ROOT / 'components/muse/muse_voice.c').read_text()
        start = source.index('static bool hatch_reply(')
        reply = source[start:source.index('\nstatic void go_idle(', start)]
        ui = (ROOT / 'components/muse/muse_ui.c').read_text()
        pose = ui[ui.index('    muse_pose_t pose = {'):]
        pose_mode = pose.split('.mode = ', 1)[1].split(',\n', 1)[0]
        code = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "muse_chat.h"
#include "muse_state.h"
#undef strlcpy
#define strlcpy test_strlcpy
#define MUSE_AUDIO_CHUNK 320
#define MUSE_AUDIO_RATE 16000
#define MUSE_PTT_DOWN 1
#define REPLY_AUDIO_GAP_US 200000
#define REPLY_COMPLETE_HOLD_MS 300
#define REPLY_COMPLETE_POLL_MS 10
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static atomic_bool s_reply_waiting;
static bool muse_voice_reply_waiting(void) { return atomic_load(&s_reply_waiting); }
static int step, scenario, captions, silence_writes;
static int64_t now;
static float progress;
static muse_mode_t mode;
static void muse_wakeword_set_playback(bool active) { (void)active; }
void muse_state_poke(void) {}
void muse_state_set_progress(float p) { progress=p; }
void muse_state_set_mode(muse_mode_t m) { mode=m; }
void muse_state_set_level(float f) { (void)f; }
void muse_state_set_caption(const char *fmt, ...) { (void)fmt; captions++; }
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ms) { now += ms * 1000; }
static size_t strlcpy(char *dst,const char *src,size_t n) {
    snprintf(dst,n,"%s",src); return strlen(src);
}
static void go_idle(const char *text) { (void)text; mode=MUSE_MODE_IDLE; progress=0; }
static bool got_event(int event) {
    (void)event;
    return scenario==1 && step==3;
}
void muse_hatch_turn_cancel(void) {}
muse_hatch_ev_t muse_hatch_turn_event(char *text,size_t cap) {
    (void)cap; text[0]=0;
    if (step==3 && scenario==2) return MUSE_HATCH_EV_ERROR;
    if (step==5) { step++; return MUSE_HATCH_EV_DONE; }
    return MUSE_HATCH_EV_NONE;
}
size_t muse_hatch_turn_read(int16_t *buf,size_t frames,int wait_ms) {
    (void)wait_ms;
    assert(step<7);
    if (step==1) {
        assert(mode==MUSE_MODE_SPEAKING && !atomic_load(&s_reply_waiting));
        now+=100000; /* a brief underflow must not flicker the avatar */
    } else if (step==2) {
        assert(!atomic_load(&s_reply_waiting));
        now+=200000;
    } else if (step==3) {
        assert(atomic_load(&s_reply_waiting));
        assert(mode==MUSE_MODE_SPEAKING && progress==.99f);
    } else if (step==4) {
        assert(!atomic_load(&s_reply_waiting)); /* new audio resumes animation */
        assert(progress==.99f); /* still forward-only across messages */
    }
    int current=step++;
    if (current==0 || current==3) {
        memset(buf,0,frames*sizeof(*buf)); /* even quiet PCM is buffered audio */
        return frames;
    }
    return 0;
}
static float muse_audio_level(const int16_t *buf,size_t n) { (void)buf; (void)n; return 0; }
static void muse_audio_write(const int16_t *buf,size_t n) {
    (void)buf; (void)n;
    if (step!=1 && step!=4) silence_writes++;
    now+=20000;
}
float muse_turn_progress(size_t played,int *message) {
    *message=played>320 ? 1 : 0;
    return played>320 ? .2f : 1;
}
bool muse_hatch_turn_caption(size_t played,char *page,size_t cap) {
    (void)played; snprintf(page,cap,"reply"); return true;
}
''' + reply + '\nstatic muse_mode_t pose_mode(muse_mode_t mode) { return ' + pose_mode + '; }\n' + r'''
int main(void) {
    atomic_store(&s_reply_waiting,true);
    assert(pose_mode(MUSE_MODE_SPEAKING)==MUSE_MODE_IDLE);
    assert(pose_mode(MUSE_MODE_LISTENING)==MUSE_MODE_LISTENING);
    assert(pose_mode(MUSE_MODE_THINKING)==MUSE_MODE_THINKING);
    atomic_store(&s_reply_waiting,false);
    assert(pose_mode(MUSE_MODE_SPEAKING)==MUSE_MODE_SPEAKING);
    for (scenario=0;scenario<3;scenario++) {
        step=0; now=1000000; progress=0; captions=0; silence_writes=0;
        bool delivered=false;
        bool interrupted=hatch_reply(&delivered,false);
        assert(interrupted==(scenario==1));
        assert(!atomic_load(&s_reply_waiting));
        assert(silence_writes>=2 && captions>0);
        if (scenario==0) assert(delivered && progress==1);
        else assert(progress==0);
    }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'reply.c').write_text(code)
            subprocess.run([
                os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'components/muse'), str(path / 'reply.c'),
                '-o', str(path / 'reply'),
            ], check=True)
            subprocess.run([str(path / 'reply')], check=True)


if __name__ == '__main__':
    unittest.main()
