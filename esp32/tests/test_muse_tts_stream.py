# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Compile the production streaming worker against a bounded, threaded RTOS fake."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StreamingTTS(unittest.TestCase):
    def test_backpressure_cancellation_and_request_isolation(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / 'freertos').mkdir()
            (p / 'fake.h').write_text(r'''
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
inline void heap_caps_free(void *p) { free(p); }
struct Buffer { std::mutex mutex; std::deque<unsigned char> bytes; size_t cap; };
using StreamBufferHandle_t = Buffer *;
inline Buffer *xStreamBufferCreateWithCaps(size_t n, int, int) { auto *b=new Buffer; b->cap=n-1; return b; }
inline void xStreamBufferReset(Buffer *b) { std::lock_guard<std::mutex> l(b->mutex); b->bytes.clear(); }
inline size_t xStreamBufferBytesAvailable(Buffer *b) { std::lock_guard<std::mutex> l(b->mutex); return b->bytes.size(); }
inline size_t xStreamBufferSend(Buffer *b, const void *p, size_t n, int) {
 std::this_thread::sleep_for(std::chrono::milliseconds(1));
 std::lock_guard<std::mutex> l(b->mutex); n=std::min(n,b->cap-b->bytes.size());
 auto *s=(const unsigned char *)p; for(size_t i=0;i<n;i++) b->bytes.push_back(s[i]); return n;
}
inline size_t xStreamBufferReceive(Buffer *b, void *p, size_t n, int) {
 std::lock_guard<std::mutex> l(b->mutex); n=std::min(n,b->bytes.size());
 auto *s=(unsigned char *)p; for(size_t i=0;i<n;i++) { s[i]=b->bytes.front(); b->bytes.pop_front(); } return n;
}
inline int xTaskCreateWithCaps(void (*fn)(void *), const char *, int, void *arg, int, void *, int) {
 std::thread([=]{fn(arg);}).detach(); return pdPASS;
}
inline void vTaskDeleteWithCaps(void *) {}
''')
            for name in ['esp_heap_caps.h', 'freertos/FreeRTOS.h', 'freertos/task.h',
                         'freertos/stream_buffer.h', 'freertos/idf_additions.h']:
                (p / name).write_text('#include "fake.h"\n')
            (p / 'test.cpp').write_text(r'''
#include "muse_tts_stream.h"
#include "fake.h"
#include <cassert>
#include <cstring>
constexpr size_t total=700123;
static muse_tts_result provider(const char *text,const muse_tts_sink &s) {
 assert(!strcmp(text,"owned text"));
 assert(s.format(s.context,muse_tts_format::mp3));
 unsigned char bytes[1973];
 for(size_t off=0;off<total;) {
  size_t n=std::min(sizeof(bytes),total-off);
  for(size_t i=0;i<n;i++) bytes[i]=(off+i)%251;
  if(!s.write(s.context,bytes,n)) return muse_tts_result::cancelled;
  off+=n;
 }
 return muse_tts_result::ok;
}
static muse_tts_result unsupported(const char *,const muse_tts_sink &s) {
 assert(!s.format(s.context,muse_tts_format::unsupported)); return muse_tts_result::failed;
}
static void idle() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
int main() {
 assert(muse_tts_get_provider()==nullptr);
 assert(!muse_tts_start(0,"owned text",provider));
 assert(!muse_tts_start(1,nullptr,provider));
 assert(!muse_tts_start(1,"",provider));
 assert(!muse_tts_start(1,"owned text",nullptr));
 char text[]="owned text";
 assert(muse_tts_start(1,text,provider)); text[0]='X';
 std::this_thread::sleep_for(std::chrono::milliseconds(50));
 assert(muse_tts_busy()); // Producer must pause on its 32 KiB queue.
 assert(!muse_tts_start(2,"owned text",provider));
 unsigned char bytes[1031]; size_t received=0; muse_tts_result r;
 for(int loops=0;loops<5000;loops++) {
  assert(!muse_tts_read(0,bytes,sizeof(bytes)));
  size_t n=muse_tts_read(1,bytes,sizeof(bytes));
  for(size_t i=0;i<n;i++) assert(bytes[i]==(received+i)%251);
  received+=n;
  if(muse_tts_end(1,&r)) break;
  idle();
 }
 assert(received==total && r==muse_tts_result::ok);
 assert(muse_tts_start(2,"owned text",provider));
 std::this_thread::sleep_for(std::chrono::milliseconds(50));
 muse_tts_cancel();
 for(int i=0;i<5000 && muse_tts_busy();i++) idle();
 assert(!muse_tts_busy());
 assert(muse_tts_end(2,&r) && r==muse_tts_result::cancelled);
 assert(muse_tts_start(3,"owned text",unsupported));
 for(int i=0;i<5000 && muse_tts_busy();i++) idle();
 assert(!muse_tts_end(2,&r));
 assert(muse_tts_end(3,&r) && r==muse_tts_result::failed);
}
''')
            binary = p / 'test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
                            '-I' + str(p), '-I' + str(ROOT / 'components/muse'),
                            str(ROOT / 'components/muse/muse_tts_stream.cpp'),
                            str(p / 'test.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
            # An application supplies a strong definition without editing the engine.
            code = (p / 'test.cpp').read_text()
            code = code.replace('int main() {',
                'muse_tts_provider muse_tts_get_provider() { return provider; }\nint main() {')
            code = code.replace('muse_tts_get_provider()==nullptr',
                                'muse_tts_get_provider()==provider')
            code = code.replace('muse_tts_start(1,text,provider)',
                                'muse_tts_start(1,text,muse_tts_get_provider())')
            (p / 'test.cpp').write_text(code)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
                            '-I' + str(p), '-I' + str(ROOT / 'components/muse'),
                            str(ROOT / 'components/muse/muse_tts_stream.cpp'),
                            str(p / 'test.cpp'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
