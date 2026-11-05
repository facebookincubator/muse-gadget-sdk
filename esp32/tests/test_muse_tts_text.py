# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise the production retained-text handler independently of caption limits."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SpeechText(unittest.TestCase):
    def test_limits_replacement_utf8_and_allocation_failure(self):
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        start = source.index('static void retain_speech(')
        handler = source[start:source.index('\n#endif', start)]
        code = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static bool allocation_failure;
static void *heap_caps_realloc(void *p,size_t n,int) { return allocation_failure ? nullptr : realloc(p,n); }
struct msg_t {};
static struct {
 bool text;
 msg_t msgs[8];
 char *speech[8];
 bool speech_overflow[8];
 size_t speech_bytes;
} s_turn;
''' + handler + r'''
int main() {
 auto &m=s_turn.msgs[0];
 std::string long_text(16000,'a'); long_text += "世界";
 retain_speech(m,long_text.c_str(),false);
 assert(!strcmp(s_turn.speech[0],long_text.c_str()));
 retain_speech(m,"終",false);
 assert(std::string(s_turn.speech[0])==long_text+"終");
 retain_speech(m,"authoritative final",true);
 assert(!strcmp(s_turn.speech[0],"authoritative final"));
 std::string max_text(32767,'x');
 retain_speech(m,max_text.c_str(),true);
 assert(!s_turn.speech_overflow[0]);
 retain_speech(m,"overflow",false);
 assert(s_turn.speech_overflow[0] && strlen(s_turn.speech[0])==32767);
 retain_speech(m,"ignored",false);
 assert(strlen(s_turn.speech[0])==32767);
 retain_speech(m,"recovered",true);
 assert(!s_turn.speech_overflow[0] && !strcmp(s_turn.speech[0],"recovered"));
 allocation_failure=true;
 retain_speech(m,"failure",false);
 assert(s_turn.speech_overflow[0] && !strcmp(s_turn.speech[0],"recovered"));
 allocation_failure=false;
 retain_speech(m,max_text.c_str(),true);
 for(int i=1;i<4;i++) retain_speech(s_turn.msgs[i],max_text.c_str(),false);
 assert(s_turn.speech_bytes==128*1024);
 retain_speech(s_turn.msgs[4],"cannot fit",false);
 assert(s_turn.speech_overflow[4] && !s_turn.speech[4]);
 s_turn.text=true;
 retain_speech(s_turn.msgs[5],"typed stays unspoken",false);
 assert(!s_turn.speech[5]);
 for(auto *p:s_turn.speech) free(p);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', str(p / 'test.cpp'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)
