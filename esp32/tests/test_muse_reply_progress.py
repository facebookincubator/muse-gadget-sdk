# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Production message selection, caption timing, and progress at a playback cursor."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class ReplyProgress(unittest.TestCase):
    def test_playback_duration_corrections_completion_and_caption_clock(self):
        source=(ROOT/'components/muse/muse_chat_session.cpp').read_text()
        start=source.index('static int reply_message_at(')
        functions=source[start:source.index('extern "C" size_t muse_hatch_turn_read(',start)]
        code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#define MIC_RATE 16000
#define SPEECH_CHARS_PER_S 14
#define TEXT_MAX 1024
#define CAPTION_LEN(t) strlen(t)
enum {TTS_NONE,TTS_QUEUED,TTS_ACTIVE,TTS_FINISHED};
struct msg_t { size_t len; int tts; uint32_t pcm_start,pcm_frames,pcm_end; };
static struct { int nmsgs; msg_t msgs[3]; char *texts; } s_turn;
static size_t caption_at;
static bool muse_hatch_caption_at(const char *,size_t at,char *,size_t) {caption_at=at;return true;}
''' + functions + r'''
static bool close(float a,float b) {return a>b-.0001f && a<b+.0001f;}
int main() {
 int message=99; char page[20];
 assert(muse_turn_progress(0,&message)==0 && message==-1);
 char text[3*TEXT_MAX]={}; memset(text,'x',1000); s_turn.texts=text;
 s_turn.nmsgs=1; auto &m=s_turn.msgs[0];
 m={6000,TTS_QUEUED,0,0};
 assert(muse_turn_progress(0,&message)==0 && message==-1);
 m.tts=TTS_ACTIVE; m.pcm_frames=90*MIC_RATE;
 assert(close(muse_turn_progress(30*MIC_RATE,&message),1.f/3) && message==0);
 assert(muse_hatch_turn_caption(30*MIC_RATE,page,sizeof(page)) && caption_at==333);
 assert(close(muse_turn_progress(60*MIC_RATE,&message),2.f/3));
 assert(close(muse_turn_progress(90*MIC_RATE,&message),.99f));
 /* Stream is complete in the decoder, but buffered PCM has not drained yet. */
 m.tts=TTS_FINISHED; m.pcm_end=90*MIC_RATE;
 assert(close(muse_turn_progress(89*MIC_RATE,&message),89.f/90.f));
 assert(muse_turn_progress(90*MIC_RATE,&message)==1);
 m.tts=TTS_ACTIVE; m.pcm_frames=120*MIC_RATE;
 assert(close(muse_turn_progress(60*MIC_RATE,&message),.5f));
 /* Buffering: the same consumed cursor must produce identical progress. */
 assert(close(muse_turn_progress(60*MIC_RATE,&message),.5f));
 /* Preserve main's caption-length fallback even for a longer message. */
 m.pcm_frames=0;
 uint32_t estimated=(uint64_t)strlen(text)*MIC_RATE/SPEECH_CHARS_PER_S;
 assert(close(muse_turn_progress(estimated/2,&message),.5f));
 assert(muse_hatch_turn_caption(estimated/2,page,sizeof(page)) && caption_at==(uint64_t)(estimated/2)*1000/estimated);
 /* Decoder can already have prepared the next message; don't select it early. */
 m.pcm_frames=90*MIC_RATE; m.tts=TTS_FINISHED;
 s_turn.nmsgs=2; s_turn.msgs[1]={40,TTS_ACTIVE,90*MIC_RATE,10*MIC_RATE};
 assert(close(muse_turn_progress(45*MIC_RATE,&message),.5f) && message==0);
 assert(muse_turn_progress(90*MIC_RATE,&message)==0 && message==1);
 assert(close(muse_turn_progress(95*MIC_RATE,&message),.5f));
 s_turn.msgs[1].tts=TTS_FINISHED; s_turn.msgs[1].pcm_end=100*MIC_RATE;
 assert(muse_turn_progress(100*MIC_RATE,nullptr)==1);
 /* Caption-only message holds its final page while queued silence drains. */
 s_turn.nmsgs=1; m={1000,TTS_ACTIVE,0,10*MIC_RATE};
 assert(close(muse_turn_progress(5*MIC_RATE,&message),.5f));
 assert(close(muse_turn_progress(11*MIC_RATE,&message),.99f));
 m.tts=TTS_FINISHED; m.pcm_end=12*MIC_RATE;
 assert(close(muse_turn_progress(11*MIC_RATE,&message),.99f));
 assert(muse_turn_progress(12*MIC_RATE,&message)==1);
 s_turn.nmsgs=0;
 assert(muse_turn_progress(0,&message)==0 && message==-1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX','c++'),'-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
