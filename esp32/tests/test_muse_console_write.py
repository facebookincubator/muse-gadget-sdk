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

"""Compile the real console writer against controlled USB/UART transports."""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class ConsoleWrite(unittest.TestCase):
    def run_writer(self, uart, scenario):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            headers = {
                'esp_err.h': 'typedef int esp_err_t;\n#define ESP_OK 0\n',
                'sdkconfig.h': f'#define CONFIG_MUSE_CONSOLE_UART {int(uart)}\n#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 0\n#define CONFIG_ESP_CONSOLE_UART_NUM 0\n',
                'freertos/FreeRTOS.h': '#define portMAX_DELAY 0xffffffffu\n#define pdMS_TO_TICKS(x) (x)\n',
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
''',
                'driver/uart_vfs.h': 'void uart_vfs_dev_use_driver(int);\n',
            }
            for name, text in headers.items():
                f = p / name; f.parent.mkdir(parents=True, exist_ok=True); f.write_text(text)
            harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_console.h"
#include "driver/usb_serial_jtag.h"
static unsigned calls, bytes, maxwait, maxchunk;
static int scenario;
static unsigned char captured[400];
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *c) {(void)c;return 0;}
int usb_serial_jtag_read_bytes(void *p,unsigned n,unsigned t) {(void)p;(void)n;(void)t;return 0;}
int usb_serial_jtag_is_connected(void) {return 1;}
static int write_fake(const void *p,unsigned n,unsigned t) {
    calls++; if(t>maxwait)maxwait=t; if(n>maxchunk)maxchunk=n;
    if(scenario==1 && calls==2)return 0;
    if(scenario==2 && n>7)n=7;
    if(scenario==3)return -1;
    if(scenario==4)return (int)n+1;
    memcpy(captured+bytes,p,n);bytes+=n;return (int)n;
}
int usb_serial_jtag_write_bytes(const void *p,unsigned n,unsigned t) {return write_fake(p,n,t);}
int uart_driver_install(int p,unsigned n,int a,int b,void *q,int f) {(void)p;(void)n;(void)a;(void)b;(void)q;(void)f;return 0;}
int uart_read_bytes(int p,void *b,unsigned n,unsigned t) {(void)p;(void)b;(void)n;(void)t;return 0;}
int uart_write_bytes(int p,const void *b,unsigned n) {(void)p;return write_fake(b,n,0);}
void uart_vfs_dev_use_driver(int p) {(void)p;}
int main(int argc,char **argv) {
    (void)argc;scenario=atoi(argv[1]);unsigned char input[300];
    for(unsigned i=0;i<sizeof(input);i++)input[i]=(unsigned char)i;
    bool ok=muse_console_write(input,scenario==5?0:sizeof(input));
    printf("%d %u %u %u %u %d\n",ok,calls,bytes,maxwait,maxchunk,memcmp(input,captured,bytes)==0);
}
'''
            (p / 'harness.c').write_text(harness)
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(p), '-I', str(ROOT / 'components/muse'), str(ROOT / 'components/muse/muse_console.c'), str(p/'harness.c'), '-o', str(p/'run')]
            subprocess.run(command, check=True, capture_output=True)
            r=subprocess.run([str(p/'run'),str(scenario)],check=True,capture_output=True,text=True,timeout=2)
            return list(map(int,r.stdout.split()))

    def test_usb_complete_and_bounded(self):
        self.assertEqual(self.run_writer(False,0),[1,3,300,200,128,1])
    def test_usb_stall_aborts(self):
        self.assertEqual(self.run_writer(False,1),[0,2,128,200,128,1])
    def test_usb_partial_write_preserves_bytes(self):
        r=self.run_writer(False,2);self.assertEqual(r[0],1);self.assertEqual(r[2],300);self.assertEqual(r[5],1)
    def test_usb_driver_error(self):
        self.assertEqual(self.run_writer(False,3)[0],0)
    def test_usb_invalid_count(self):
        self.assertEqual(self.run_writer(False,4)[0],0)
    def test_usb_empty(self):
        self.assertEqual(self.run_writer(False,5),[1,0,0,0,0,1])
    def test_uart_complete(self):
        self.assertEqual(self.run_writer(True,0),[1,1,300,0,300,1])
    def test_uart_short_write_is_failure(self):
        self.assertEqual(self.run_writer(True,2)[0],0)
    def test_uart_driver_error(self):
        self.assertEqual(self.run_writer(True,3)[0],0)

class SnapshotAbort(unittest.TestCase):
    def run_snapshot(self, failure):
        source = (ROOT / 'components/muse/muse_ui.c').read_text()
        body = source.split('static void send_snapshot(void)', 1)[1].split('void muse_ui_request_snapshot', 1)[0]
        harness = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LV_USE_SNAPSHOT 1
#define LV_COLOR_FORMAT_RGB565 0
static int failure, writes, destroyed, ended;
typedef struct {struct {unsigned w,h,stride;} header; unsigned char *data;} lv_draw_buf_t;
static unsigned char pixels[600];
static lv_draw_buf_t buffer={{150,2,300},pixels};
static void *lv_screen_active(void) {return NULL;}
static lv_draw_buf_t *lv_snapshot_take(void *s,int f) {(void)s;(void)f;return &buffer;}
static void lv_draw_buf_destroy(lv_draw_buf_t *b) {(void)b;destroyed++;}
static int mbedtls_base64_encode(unsigned char *b,size_t cap,size_t *out,const unsigned char *p,size_t n) {
    (void)p;(void)n;(void)cap;*out=4;memcpy(b,"AAAA",4);return 0;
}
static bool muse_console_write(const void *p,size_t n) {
    (void)n;writes++;if(writes==failure)return false;
    if(!memcmp(p,"SNAP END",8))ended++;return true;
}
"""
        # Execute the actual UI function, isolating unrelated UI/LVGL machinery.
        harness += 'static void send_snapshot(void)' + body
        harness += r"""
int main(int argc,char **argv) {(void)argc;failure=atoi(argv[1]);send_snapshot();printf("%d %d %d\n",writes,destroyed,ended);}
"""
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.c').write_text(harness)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'run')],check=True,capture_output=True)
            r=subprocess.run([str(p/'run'),str(failure)],check=True,text=True,capture_output=True,timeout=2)
            return list(map(int,r.stdout.split()))

    def test_complete_snapshot_frees_buffer_and_ends(self):
        self.assertEqual(self.run_snapshot(0),[8,1,1])
    def test_header_failure_frees_buffer_and_aborts(self):
        self.assertEqual(self.run_snapshot(1),[1,1,0])
    def test_mid_frame_failure_stops_writes_and_frees_buffer(self):
        self.assertEqual(self.run_snapshot(3),[3,1,0])
    def test_end_failure_still_frees_buffer(self):
        self.assertEqual(self.run_snapshot(8),[8,1,0])

if __name__=='__main__':unittest.main()
