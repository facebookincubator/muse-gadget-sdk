# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Muse console deadlines and capture lifecycle, using the production C code."""
import os
import json
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MirrorAdapter(unittest.TestCase):
    def test_held_keys_taps_and_pointer_edges(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        pointer = source[source.index('static void pointer_put'):source.index('uint16_t *muse_mirror_frame_begin')]
        keys = source[source.index('static void key_set'):source.index('#define SERIAL_CHUNK')]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
typedef struct {uint8_t edit;char text[64];} muse_mirror_text_t;
static bool s_text_viewer;static atomic_bool s_text_field;static int s_text_sent,s_text_in;
static bool muse_mirror_core_text(const uint8_t *b,size_t n,uint8_t *e,char *t) {(void)b;(void)n;(void)e;(void)t;return false;}
static int xQueueSend(int q,const void *m,int ticks) {(void)q;(void)m;(void)ticks;return 1;}
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define PTR_Q 32
#define TAP_MS 150
#define SEND_MS 3000
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef struct {uint16_t x,y;bool down,edge;} ptr_t;
static ptr_t s_ptr_q[PTR_Q],s_ptr;
static unsigned s_ptr_head,s_ptr_n,s_held;
static int s_ptr_mux,s_ptr_x,s_ptr_y,s_tap_key,s_w=10,s_h=10;
static int64_t s_tap_until;
static char s_board[1];
static size_t s_board_len;
static int events[32],n_events;
static void on_key(int role,bool down) {events[n_events++]=down?role:-role;}
static struct {void (*on_key)(int,bool);bool touch;} s_cfg={.on_key=on_key,.touch=true};
static int64_t esp_timer_get_time(void) {return 1000;}
static void dirty_add(int x1,int y1,int x2,int y2) {(void)x1;(void)y1;(void)x2;(void)y2;}
static bool viewer_send(const void *p,size_t n,int ms) {(void)p;(void)n;(void)ms;return true;}
static void serial_end(void) {}
static int sounds;
static bool option_input(const uint8_t *b,size_t n) {if (n!=2||b[0]<7||b[0]>10) return false;sounds++;return true;}
'''
        harness += pointer + keys + r'''
int main(void) {
    input((uint8_t[]){6,1,1},3);
    input((uint8_t[]){6,1,1},3);
    input((uint8_t[]){3,1},2);
    input((uint8_t[]){6,1,0},3);
    assert(n_events==1 && events[0]==1);
    tap_end();assert(n_events==2 && events[1]==-1);
    input((uint8_t[]){3,2},2);
    input((uint8_t[]){6,2,1},3);
    tap_end();assert(n_events==3 && events[2]==2);
    input((uint8_t[]){6,4,1},3);
    keys_release();assert(n_events==6 && events[4]==-2 && events[5]==-4 && !s_held);
    input((uint8_t[]){6,1,2},3);
    input((uint8_t[]){6,11,1},3);
    input((uint8_t[]){3,0},2);
    assert(n_events==6);
    input((uint8_t[]){9,1},2);
    input((uint8_t[]){7,2},2);
    assert(n_events==6 && sounds==2);
    input((uint8_t[]){1,2,0,3,0},5);
    input((uint8_t[]){1,4,0,5,0},5);
    input((uint8_t[]){1,6,0,7,0},5);
    input((uint8_t[]){0,6,0,7,0},5);
    int x,y;bool down;
    assert(muse_mirror_pointer_read(&x,&y,&down) && down && x==2 && y==3);
    assert(muse_mirror_pointer_read(&x,&y,&down) && down && x==6 && y==7);
    assert(!muse_mirror_pointer_read(&x,&y,&down) && !down && x==6 && y==7);
    input((uint8_t[]){1,10,0,0,0},5);
    assert(!muse_mirror_pointer_read(&x,&y,&down) && !down);
    s_cfg.touch=false;input((uint8_t[]){1,2,0,3,0},5);
    assert(!muse_mirror_pointer_read(&x,&y,&down) && !down);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / 'test.c').write_text(harness)
            result = subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'run')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([p / 'run'], check=True, timeout=5)

    def test_console_partial_reads_and_writes(self):
        headers = {
            'esp_err.h': 'typedef int esp_err_t;\n#define ESP_OK 0\n',
            'freertos/FreeRTOS.h': '#include <stdint.h>\ntypedef uint32_t TickType_t;\n#define portMAX_DELAY UINT32_MAX\n#define pdMS_TO_TICKS(x) (x)\n',
            'freertos/task.h': '#include "freertos/FreeRTOS.h"\nTickType_t xTaskGetTickCount(void);\n',
            'driver/usb_serial_jtag.h': '''typedef struct {int rx_buffer_size;} usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() ((usb_serial_jtag_driver_config_t){0})
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *);
int usb_serial_jtag_read_bytes(void *, unsigned, unsigned);
int usb_serial_jtag_write_bytes(const void *, unsigned, unsigned);
int usb_serial_jtag_is_connected(void);
''',
            'driver/uart.h': '''int uart_driver_install(int, unsigned, int, int, void *, int);
int uart_read_bytes(int, void *, unsigned, unsigned);
int uart_write_bytes(int, const void *, unsigned);
int uart_wait_tx_done(int, unsigned);
int uart_set_baudrate(int, unsigned);
''',
            'driver/usb_serial_jtag_vfs.h': 'void usb_serial_jtag_vfs_use_driver(void);\n',
            'driver/uart_vfs.h': 'void uart_vfs_dev_use_driver(int);\n',
        }
        harness = r'''
#include <assert.h>
#include <string.h>
#include "sdkconfig.h"
#include "muse_console.h"
#include "driver/usb_serial_jtag.h"
static unsigned tick, calls, offset, stall, error, timeout;
unsigned xTaskGetTickCount(void) {return tick;}
static int read_bytes(void *p, unsigned n, unsigned t) {
    calls++; timeout=t;
    if (stall) {tick+=t;return 0;}
    if (error) return -1;
    if (n>3)n=3;
    for(unsigned i=0;i<n;i++)((unsigned char *)p)[i]=offset++;
    tick+=2;return n;
}
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *c) {(void)c;return 0;}
void usb_serial_jtag_vfs_use_driver(void) {}
int usb_serial_jtag_read_bytes(void *p,unsigned n,unsigned t) {return read_bytes(p,n,t);}
int usb_serial_jtag_write_bytes(const void *p,unsigned n,unsigned t) {(void)p;(void)n;timeout=t;return 2;}
int usb_serial_jtag_is_connected(void) {return 1;}
int uart_driver_install(int p,unsigned n,int a,int b,void *q,int f) {(void)p;(void)n;(void)a;(void)b;(void)q;(void)f;return 0;}
int uart_read_bytes(int p,void *b,unsigned n,unsigned t) {(void)p;return read_bytes(b,n,t);}
int uart_write_bytes(int p,const void *b,unsigned n) {(void)p;(void)b;(void)n;return 2;}
void uart_vfs_dev_use_driver(int p) {(void)p;}
static unsigned baud,drained;
int uart_wait_tx_done(int p,unsigned t) {(void)p;(void)t;drained++;return 0;}
int uart_set_baudrate(int p,unsigned b) {(void)p;assert(drained);drained=0;baud=b;return 0;}
int main(void) {
    unsigned char buf[12];
    assert(muse_console_read(buf,9,10));
    assert(calls==3 && timeout==6 && tick==6);
    for(unsigned i=0;i<9;i++)assert(buf[i]==i);
    calls=0;tick=0;
    assert(!muse_console_read(buf,12,5));
    assert(calls==3 && tick==6);  /* partial reads share a deadline */
    calls=0;tick=0;stall=1;
    assert(!muse_console_read(buf,1,5) && calls==1 && tick==5);
    stall=0;error=1;
    assert(!muse_console_read(buf,1,5));
    error=0;calls=0;tick=0xfffffffcU;offset=0;
    assert(muse_console_read(buf,9,10) && calls==3 && tick==2);
    calls=0;assert(muse_console_read(buf,0,0) && calls==0);
    assert(!muse_console_read(buf,1,0) && calls==0);
    assert(muse_console_write_bytes(buf,9,37)==2);
#if !CONFIG_MUSE_CONSOLE_UART
    assert(timeout==37);
    assert(!muse_console_set_baud(921600) && !baud);
#else
    /* What was written leaves at the old speed first; 0 is the configured speed. */
    assert(muse_console_set_baud(921600) && baud==921600);
    assert(muse_console_set_baud(0) && baud==115200);
#endif
    return 0;
}
'''
        for uart in (0, 1):
            with self.subTest(uart=uart), tempfile.TemporaryDirectory() as folder:
                p = Path(folder)
                for name, body in headers.items():
                    f = p / name
                    f.parent.mkdir(parents=True, exist_ok=True)
                    f.write_text(body)
                (p / 'sdkconfig.h').write_text(f'#define CONFIG_MUSE_MIRROR 1\n#define CONFIG_MUSE_CONSOLE_UART {uart}\n#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 0\n#define CONFIG_ESP_CONSOLE_UART_NUM 0\n#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200\n')
                (p / 'test.c').write_text(harness)
                command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                           '-I', str(p), '-I', str(ROOT / 'components/muse'), '-I', str(ROOT / 'components/muse_mirror'), str(p / 'test.c'),
                           str(ROOT / 'components/muse/muse_console.c'), '-o', str(p / 'run')]
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                subprocess.run([p / 'run'], check=True, timeout=5)

    def test_reconnect_requires_a_fresh_complete_redraw(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        lifecycle = source[source.index('enum { CAPTURE_OFF'):source.index('/* Buffers and queues live in PSRAM')]
        frame = source[source.index('uint16_t *muse_mirror_frame_begin'):source.index('void muse_mirror_set_screen_state')]
        build = source[source.index('static size_t build_msg'):source.index('/* A key the viewer holds')]
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "muse_mirror_core.h"
#define portMAX_DELAY 0
#define MSG_MAX 128
static int s_lock, locked;
static void xSemaphoreTake(int lock, int wait) {(void)lock;(void)wait;assert(!locked);locked=1;}
static void xSemaphoreGive(int lock) {(void)lock;assert(locked);locked=0;}
static uint16_t pixels[4], *s_fb=pixels;
static uint16_t snapped[4], *s_snap=snapped;
static size_t msg_cap(void) {return MSG_MAX;}
static bool s_drawing;
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ticks) {(void)ticks;}
static uint8_t s_msg[MSG_MAX], s_state;
static int s_w=2, s_h=2;
static muse_mirror_core_t s_core;
'''
        harness += lifecycle + frame + build + r'''
int main(void) {
    assert(muse_mirror_core_init(&s_core,2,2));
    assert(!muse_mirror_frame_begin() && !locked);
    unsigned first, second;
    capture_set(true);
    assert(muse_mirror_capture_begin(&first));
    assert(!muse_mirror_capture_begin(&second));
    assert(build_msg()==0);
    uint16_t *fb=muse_mirror_frame_begin();assert(fb && locked);
    for(int i=0;i<4;i++)fb[i]=0xf800;
    muse_mirror_frame_end(-1,-1,4,4,true);assert(!locked);
    assert(build_msg()==0); /* no partially refreshed initial frame */
    capture_set(false);
    muse_mirror_capture_ready(first);
    assert(!muse_mirror_frame_begin() && build_msg()==0);
    capture_set(true);
    assert(muse_mirror_capture_begin(&second) && second!=first);
    muse_mirror_capture_ready(first);
    assert(build_msg()==0); /* a previous viewer's redraw cannot publish */
    fb=muse_mirror_frame_begin();assert(fb);
    for(int i=0;i<4;i++)fb[i]=0x07e0;
    muse_mirror_frame_end(0,0,2,2,true);
    muse_mirror_capture_ready(second);
    assert(build_msg()>8 && s_msg[0]=='F' && s_msg[2]==1);
    assert(snapped[3]==0x07e0); /* sent from the snapshot */
    capture_set(false);assert(!muse_mirror_frame_begin() && !locked);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            (p / 'test.c').write_text(harness)
            result = subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-I', str(ROOT / 'components/muse'), '-I', str(ROOT / 'components/muse_mirror'), str(p / 'test.c'),
                                     str(ROOT / 'components/muse_mirror/muse_mirror_core.c'), '-o', str(p / 'run')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([p / 'run'], check=True, timeout=5)

    def test_failed_start_leaves_no_screen(self):
        # The screen calls check s_w before taking the lock: it is set only once nothing can fail.
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        start = source[source.index('esp_err_t muse_mirror_session_start('):]
        published = start.index('    s_w = w;')
        self.assertEqual(start.count('s_w ='), 1)
        self.assertEqual(start.count('s_h ='), 1)
        self.assertEqual(re.findall(r'\breturn\b[^;]*;', start[published:]), ['return ESP_OK;'])
        self.assertLess(published, start.index('__atomic_store_n(&s_fb, fb'))

    @unittest.skipUnless(os.name == 'posix', 'C harness uses POSIX compiler and sanitizers')
    def test_board_name_stays_valid_json(self):
        source = (ROOT / 'components/muse_mirror/muse_mirror_session.c').read_text()
        escape = source[source.index('static void json_text('):source.index('esp_err_t muse_mirror_session_start(')]
        harness = r'''#include <stdio.h>
#include <string.h>
static char s_name[48];
''' + escape + r'''
static void name(const char *raw) {
    json_text(s_name, sizeof(s_name), raw);
    printf("{\"name\":\"%s\"}\n", s_name);
}
int main(void) {
    name("ab\xC3\",\"x\":1");
    name("ab\xE2\x82\\");
    name("\x80\xBF stray \xF8 high");
    name("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \"q\" \\b");
    name("\xC3");
    name("0123456789012345678901234567890123456789012345\xE2\x82\xAC");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, exe = Path(tmp) / 'name.c', Path(tmp) / 'name'
            path.write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(path), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([exe], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
        names = [json.loads(line)['name'] for line in result.stdout.decode('utf-8').splitlines()]
        self.assertEqual(names, ['ab","x":1', 'ab\\', ' stray  high',
                                 'café € 😀 "q" \\b', '', '0123456789012345678901234567890123456789012345'])
