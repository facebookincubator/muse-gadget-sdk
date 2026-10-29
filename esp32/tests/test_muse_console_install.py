# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0

"""Compile console initialization and verify its VFS routing contract."""
import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ConsoleInstall(unittest.TestCase):
    def run_install(self, uart, failure):
        with tempfile.TemporaryDirectory() as folder:
            p = Path(folder)
            headers = {
                'esp_err.h': 'typedef int esp_err_t;\n#define ESP_OK 0\n',
                'sdkconfig.h': f'#define CONFIG_MUSE_CONSOLE_UART {int(uart)}\n#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 0\n#define CONFIG_ESP_CONSOLE_UART_NUM 0\n',
                'freertos/FreeRTOS.h': '#define portMAX_DELAY 0xffffffffu\n#define pdMS_TO_TICKS(ms) (ms)\n',
                'driver/usb_serial_jtag.h': '''typedef struct {int rx_buffer_size;} usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() ((usb_serial_jtag_driver_config_t){0})
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *);
int usb_serial_jtag_read_bytes(void *, unsigned, unsigned);
int usb_serial_jtag_write_bytes(const void *, unsigned, unsigned);
int usb_serial_jtag_is_connected(void);
''',
                'driver/usb_serial_jtag_vfs.h': 'void usb_serial_jtag_vfs_use_driver(void);\n',
                'driver/uart.h': '''int uart_driver_install(int, unsigned, int, int, void *, int);
int uart_read_bytes(int, void *, unsigned, unsigned);
int uart_write_bytes(int, const void *, unsigned);
''',
                'driver/uart_vfs.h': 'void uart_vfs_dev_use_driver(int);\n',
            }
            for name, text in headers.items():
                f = p / name
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(text)
            harness = p / 'main.c'
            harness.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include "muse_console.h"
#include "driver/usb_serial_jtag.h"
static int fail, installed, usb_vfs, uart_vfs, ordered = 1, rx;
int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *c) {rx=c->rx_buffer_size;installed=1;return fail;}
void usb_serial_jtag_vfs_use_driver(void) {usb_vfs++;ordered=installed;}
int usb_serial_jtag_read_bytes(void *b,unsigned n,unsigned t) {(void)b;(void)n;(void)t;return 0;}
int usb_serial_jtag_write_bytes(const void *b,unsigned n,unsigned t) {(void)b;(void)t;return n;}
int usb_serial_jtag_is_connected(void) {return 1;}
int uart_driver_install(int p,unsigned n,int a,int b,void *q,int f) {(void)p;(void)a;(void)b;(void)q;(void)f;rx=n;installed=1;return fail;}
void uart_vfs_dev_use_driver(int p) {(void)p;uart_vfs++;ordered=installed;}
int uart_read_bytes(int p,void *b,unsigned n,unsigned t) {(void)p;(void)b;(void)n;(void)t;return 0;}
int uart_write_bytes(int p,const void *b,unsigned n) {(void)p;(void)b;return n;}
int main(int argc,char **argv) {(void)argc;fail=atoi(argv[1]);int rc=muse_console_install(4096);printf("%d %d %d %d %d\n",rc,usb_vfs,uart_vfs,ordered,rx);}
''')
            exe = p / 'test'
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(p),
                '-I', str(ROOT / 'components/muse'),
                str(ROOT / 'components/muse/muse_console.c'), str(harness), '-o', str(exe)
            ], check=True, capture_output=True)
            return tuple(map(int, subprocess.check_output([str(exe), str(failure)], text=True).split()))

    def test_usb_vfs_uses_installed_driver(self):
        self.assertEqual(self.run_install(False, 0), (0, 1, 0, 1, 4096))

    def test_failed_usb_install_does_not_switch_vfs(self):
        self.assertEqual(self.run_install(False, 7), (7, 0, 0, 1, 4096))

    def test_uart_routing_is_preserved(self):
        self.assertEqual(self.run_install(True, 0), (0, 0, 1, 1, 4096))
        self.assertEqual(self.run_install(True, 7), (7, 0, 0, 1, 4096))


if __name__ == '__main__':
    unittest.main()
