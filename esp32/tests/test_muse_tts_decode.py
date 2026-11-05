# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Verify production MP3 staging preserves the ending across network chunks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DecodeChunks(unittest.TestCase):
    def test_chunked_decode_matches_complete_file(self):
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        start = source.index('static void decode(void)')
        decode = source[start:source.index('\n/* Ends the turn', start)]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#define MP3_HOLD (1441+4)
#define MIC_RATE 16000
#define ESP_LOGI(...) ((void)0)
#define M_AUDIO 0
#define TTS_FINISHED 2
static void mark(int) {}
static void pace_silently() {}
static std::atomic<int> s_gen{1};
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME], pcm16[MINIMP3_MAX_SAMPLES_PER_FRAME+8];
static int16_t *s_pcm=pcm,*s_pcm16=pcm16;
static int s_out;
static size_t written;
static size_t xStreamBufferSpacesAvailable(int) { return 100000; }
static size_t xStreamBufferSend(int,const void *,size_t n,int) { written+=n/2; return n; }
static void resampler_init(int *,int,int) {}
static size_t resample(int *,const int16_t *,int n,int16_t *) { return n; }
struct msg_t { uint32_t pcm_start,pcm_frames; int tts; };
static struct {
 int tts_msg,gen=1;
 bool silent,mp3_ended;
 uint8_t mp3[65536];
 size_t mp3_len;
 mp3dec_t dec;
 int down_rate,down,kbps;
 uint32_t pcm_out;
 msg_t msgs[1];
} s_turn;
''' + decode + r'''
static size_t run(const std::vector<uint8_t> &mp3,size_t chunk) {
 s_turn={}; s_turn.gen=1; written=0; mp3dec_init(&s_turn.dec);
 for(size_t off=0;off<mp3.size();) {
  size_t n=std::min(chunk,mp3.size()-off);
  assert(s_turn.mp3_len+n<=sizeof(s_turn.mp3));
  memcpy(s_turn.mp3+s_turn.mp3_len,mp3.data()+off,n);
  s_turn.mp3_len+=n; off+=n; decode();
 }
 s_turn.mp3_ended=true; decode();
 assert(s_turn.tts_msg==-1 && !s_turn.mp3_len);
 assert(s_turn.msgs[0].pcm_frames==written);
 return written;
}
int main(int argc,char **argv) {
 std::ifstream f(argv[1],std::ios::binary);
 std::vector<uint8_t> mp3{std::istreambuf_iterator<char>(f),{}};
 assert(!mp3.empty());
 size_t frames=run(mp3,32768); assert(frames>0);
 for(size_t chunk:{1,17,1973,4096}) assert(run(mp3,chunk)==frames);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                            '-I' + str(ROOT / 'components/minimp3/include'),
                            str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test'), str(ROOT / 'components/muse/test_reply.mp3')],
                           check=True, timeout=20)
