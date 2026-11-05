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
static struct { int nmsgs; msg_t msgs[3]; char *texts; bool speech_failed; } s_turn;
static size_t caption_at;
static bool muse_hatch_caption_at(const char *,size_t at,char *,size_t) {caption_at=at;return true;}
''' + functions + r'''
static bool close(float a,float b) {return a>b-.0001f && a<b+.0001f;}
int main() {
 int message=99; char page[20];
 assert(muse_hatch_turn_progress(0,&message)==0 && message==-1);
 char text[3*TEXT_MAX]={}; memset(text,'x',1000); s_turn.texts=text;
 s_turn.nmsgs=1; auto &m=s_turn.msgs[0];
 m={6000,TTS_QUEUED,0,0};
 assert(muse_hatch_turn_progress(0,&message)==0 && message==-1);
 m.tts=TTS_ACTIVE; m.pcm_frames=90*MIC_RATE;
 assert(close(muse_hatch_turn_progress(30*MIC_RATE,&message),1.f/3) && message==0);
 assert(muse_hatch_turn_caption(30*MIC_RATE,page,sizeof(page)) && caption_at==333);
 assert(close(muse_hatch_turn_progress(60*MIC_RATE,&message),2.f/3));
 assert(close(muse_hatch_turn_progress(90*MIC_RATE,&message),.99f));
 // Stream is complete in the decoder, but buffered PCM has not drained yet.
 m.tts=TTS_FINISHED; m.pcm_end=90*MIC_RATE;
 assert(close(muse_hatch_turn_progress(89*MIC_RATE,&message),89.f/90.f));
 assert(muse_hatch_turn_progress(90*MIC_RATE,&message)==1);
 m.tts=TTS_ACTIVE; m.pcm_frames=120*MIC_RATE;
 assert(close(muse_hatch_turn_progress(60*MIC_RATE,&message),.5f));
 // Buffering: the same consumed cursor must produce identical progress.
 assert(close(muse_hatch_turn_progress(60*MIC_RATE,&message),.5f));
 // Legacy fallback duration uses full message length, not the 1 KiB caption.
 m.pcm_frames=0;
 uint32_t estimated=(uint64_t)m.len*MIC_RATE/SPEECH_CHARS_PER_S;
 assert(close(muse_hatch_turn_progress(estimated/2,&message),.5f));
 // Decoder can already have prepared the next message; don't select it early.
 m.pcm_frames=90*MIC_RATE; m.tts=TTS_FINISHED;
 s_turn.nmsgs=2; s_turn.msgs[1]={40,TTS_ACTIVE,90*MIC_RATE,10*MIC_RATE};
 assert(close(muse_hatch_turn_progress(45*MIC_RATE,&message),.5f) && message==0);
 assert(muse_hatch_turn_progress(90*MIC_RATE,&message)==0 && message==1);
 assert(close(muse_hatch_turn_progress(95*MIC_RATE,&message),.5f));
 s_turn.msgs[1].tts=TTS_FINISHED; s_turn.msgs[1].pcm_end=100*MIC_RATE;
 assert(muse_hatch_turn_progress(100*MIC_RATE,nullptr)==1);
#if CONFIG_MUSE_TTS_STREAMING
 s_turn.speech_failed=true;
 assert(close(muse_hatch_turn_progress(100*MIC_RATE,&message),.99f));
#endif
 // Caption-only message holds its final page while queued silence drains.
 s_turn.nmsgs=1; m={1000,TTS_ACTIVE,0,10*MIC_RATE};
 assert(close(muse_hatch_turn_progress(5*MIC_RATE,&message),.5f));
 assert(close(muse_hatch_turn_progress(11*MIC_RATE,&message),.99f));
 m.tts=TTS_FINISHED; m.pcm_end=12*MIC_RATE; s_turn.speech_failed=false;
 assert(close(muse_hatch_turn_progress(11*MIC_RATE,&message),.99f));
 assert(muse_hatch_turn_progress(12*MIC_RATE,&message)==1);
 s_turn.nmsgs=0;
 assert(muse_hatch_turn_progress(0,&message)==0 && message==-1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.cpp').write_text(code)
            for streaming in (0,1):
                subprocess.run([os.environ.get('CXX','c++'),'-std=c++17',f'-DCONFIG_MUSE_TTS_STREAMING={streaming}',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
                subprocess.run([str(p/'test')],check=True)

    def test_initial_tts_estimate_uses_full_synthesis_text_not_caption_prefix(self):
        source=(ROOT/'components/muse/muse_chat_session.cpp').read_text()
        start=source.index('static void start_tts(void)')
        function=source[start:source.index('static void tts_data(',start)]
        code=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#define CONFIG_MUSE_TTS_STREAMING 1
#define MIC_RATE 16000
#define SPEECH_CHARS_PER_S 14
#define TEXT_CHARS_PER_S 16
#define TEXT_MAX 1024
#define CAPTION_LEN(t) strnlen(t,1023)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define M_TTS 0
static const char *TAG="test";
enum {TTS_NONE,TTS_QUEUED,TTS_ACTIVE,TTS_FINISHED};
struct msg_t { int tts; size_t len; uint32_t pcm_start,pcm_frames; char id[8]; };
static struct {
 int tts_msg=-1,nmsgs=1; msg_t msgs[1];
 bool silent,speech_failed,speech_overflow[1],mp3_ended;
 char *speech[1],*texts;
 uint64_t speech_id; size_t speech_received,mp3_len;
 int64_t speech_start_us,speech_progress_us;
 uint32_t speech_pcm,pcm_out;
 int kbps,down_rate,dec;
} s_turn;
using muse_tts_provider=int;
static bool speaker=true;
static int requests;
static bool muse_settings_speaker_on(){return speaker;}
static int muse_tts_get_provider(){return 1;}
static bool muse_tts_busy(){return false;}
static bool muse_tts_start(uint64_t,const char *text,int){assert(strlen(text)==5000);requests++;return true;}
static int64_t now_us(){return 1000000;}
static void mp3dec_init(int *){}
static void mark(int){}
static void show_reply_start(const msg_t &){}
''' + function + r'''
int main(){
 std::string full(5000,'a'),prefix(1000,'a');
 s_turn.speech[0]=full.data();s_turn.texts=prefix.data();
 s_turn.msgs[0].len=5000;s_turn.msgs[0].tts=TTS_QUEUED;
 start_tts();
 assert(requests==1 && !s_turn.silent && s_turn.tts_msg==0);
 assert(s_turn.msgs[0].pcm_frames==(uint64_t)5000*MIC_RATE/SPEECH_CHARS_PER_S);
 speaker=false;s_turn.tts_msg=-1;s_turn.msgs[0].tts=TTS_QUEUED;
 start_tts();
 assert(requests==1 && s_turn.silent);
 assert(s_turn.msgs[0].pcm_frames==1000*MIC_RATE/TEXT_CHARS_PER_S);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX','c++'),'-std=c++17',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
