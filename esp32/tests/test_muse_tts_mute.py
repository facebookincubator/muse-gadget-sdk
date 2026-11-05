# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise speaker toggles through the production TTS pump and audio output."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SpeakerMute(unittest.TestCase):
    def test_mute_keeps_stream_and_playback_clock_then_unmutes_remaining_audio(self):
        session = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        start = session.index('static void poll_tts(void)')
        pump = session[start:session.index('\n#endif', start)]
        audio = (ROOT / 'components/muse/muse_audio.c').read_text()
        start = audio.index('esp_err_t muse_audio_write(')
        output = audio[start:audio.index('\nfloat muse_audio_dbfs(', start)]
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MP3_BUF 1024
#define M_MP3 0
#define MIC_RATE 16000
#define CAPTION_TEXT 1024
#define TEXT_MAX 1024
#define TEXT_CHARS_PER_S 16
#define CAPTION_LEN(p) strlen(p)
#define MUSE_AUDIO_RATE 16000
#define MUSE_AUDIO_CHUNK 4
#define CHANNELS 2
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_CODEC_DEV_OK 0
#define pdMS_TO_TICKS(n) (n)
using esp_err_t = int;
static bool speaker=true, eof;
static int cancelled, ended, reads;
static int64_t clock_us=1000000;
static bool muse_settings_speaker_on() { return speaker; }
static int64_t now_us() { return clock_us; }
static void muse_tts_cancel() { cancelled++; }
static void turn_done(bool) { ended++; }
static void mark(int) {}
enum class muse_tts_result { ok, failed, cancelled };
static size_t muse_tts_read(uint64_t id,uint8_t *out,size_t cap) {
 assert(id==7); if(cap<4) return 0;
 memset(out,++reads,4); return 4;
}
static bool muse_tts_end(uint64_t id,muse_tts_result *out) {
 assert(id==7); *out=muse_tts_result::ok; return eof;
}
struct msg_t { uint32_t pcm_start,pcm_frames; size_t len; };
static struct {
 int tts_msg;
 bool silent,mp3_ended,speech_failed;
 uint64_t speech_id=7;
 uint8_t mp3[MP3_BUF];
 size_t mp3_len,speech_received;
 uint32_t pcm_out,speech_pcm;
 int64_t speech_start_us=1000000,speech_progress_us=1000000,speech_total_us;
 char *texts;
 msg_t msgs[1];
} s_turn;
static struct { bool mic_init; } board;
static auto *muse_board=&board;
static int s_spk;
static int16_t s_out_stereo[MUSE_AUDIO_CHUNK*CHANNELS];
static std::vector<int16_t> played;
static int waits;
static void vTaskDelay(int) { waits++; }
static int esp_codec_dev_write(int,const void *data,int n) {
 auto *samples=static_cast<const int16_t *>(data);
 played.assign(samples,samples+n/sizeof(int16_t)); return ESP_CODEC_DEV_OK;
}
''' + pump + output + r'''
int main() {
 int16_t samples[]={101,202,303,404};
 poll_tts();
 assert(reads==1 && !ended && !cancelled && s_turn.speech_id==7);
 assert(muse_audio_write(samples,4)==ESP_OK && played[0]==101 && played[1]==101);
 speaker=false;
 for(int second=0;second<60;second++) {
  clock_us+=1000000;
  s_turn.pcm_out+=MIC_RATE;
  s_turn.mp3_len=0; // Decoder consumed previous bytes; playback clock continues.
  poll_tts();
  assert(!ended && !cancelled && !s_turn.speech_failed && !s_turn.silent);
  assert(s_turn.speech_id==7 && reads==second+2);
  assert(muse_audio_write(samples,4)==ESP_OK);
  for(auto sample:played) assert(sample==0);
 }
 assert(s_turn.pcm_out==60*MIC_RATE && s_turn.speech_progress_us==clock_us);
 speaker=true;
 s_turn.mp3_len=0;
 poll_tts();
 assert(!ended && !cancelled && !s_turn.speech_failed && reads==62);
 assert(muse_audio_write(samples,4)==ESP_OK && played[0]==101 && played[7]==404);
 // Completing while muted is a normal EOF, not an incomplete turn.
 speaker=false; eof=true; s_turn.mp3_len=0;
 poll_tts();
 assert(s_turn.mp3_ended && !s_turn.speech_failed && !ended && !cancelled);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', str(p / 'test.cpp'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True, timeout=10)
