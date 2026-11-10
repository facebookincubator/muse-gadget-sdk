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

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


def function(source, name):
    start = re.search(r'static (?:void|esp_err_t) ' + name + r'\([^)]*\)\n\{', source).start()
    return source[start:source.index('\n}\n', start) + 3]


class TriviaPODHaptics(unittest.TestCase):
    def test_motor_contract(self):
        source = (Path(__file__).resolve().parents[1] / 'components/muse/boards/board_m5stack_triviapod.c').read_text()
        defines = '\n'.join(re.findall(r'^#define (?:MOTOR_\w+|IOE_PWM1_DUTY|PMIC_SYS_CMD|PMIC_SHUTDOWN).*', source, re.M))
        code = defines + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
enum { ESP_OK=0, ESP_FAIL=-1, pdTRUE=1, eSetValueWithOverwrite=1 };
typedef int esp_err_t;
static void *s_motor_task=(void *)1;
static int s_ioe=1, s_pmic=2, s_ioe_lock=3, elapsed, writes, duty, delays;
static bool s_motor_enabled=true, s_motor_on, setting=true, shutdown, stop_fails;
static uint32_t pending;
static jmp_buf done;
static esp_err_t power_off(void);
static bool muse_settings_vibration_on(void) { return setting; }
static void xSemaphoreTake(int lock,uint32_t wait) { assert(lock==3 && wait==portMAX_DELAY); }
static void xSemaphoreGive(int lock) { assert(lock==3); }
static void xTaskNotify(void *task,uint32_t ms,int mode) { assert(task==s_motor_task && mode==1); pending=ms; }
static int xTaskNotifyWait(uint32_t a,uint32_t b,uint32_t *ms,uint32_t wait) {
    assert(!a && b==UINT32_MAX && wait==portMAX_DELAY);
    if(!pending) longjmp(done,1);
    *ms=pending; pending=0; return pdTRUE;
}
static esp_err_t reg_write(int dev,uint8_t reg,const uint8_t *data,size_t n) {
    if(dev==s_pmic) { assert((stop_fails || !duty) && !s_motor_enabled); longjmp(done,2); }
    assert(dev==s_ioe && reg==IOE_PWM1_DUTY && n==2);
    if(stop_fails && !data[0] && !data[1]) return ESP_FAIL;   /* the expander refuses the stop */
    duty=data[0] | data[1]<<8; assert(duty==(writes++ ? 0 : MOTOR_DUTY)); return ESP_OK;
}
static void vTaskDelay(int ms) { assert(++delays<10); if(shutdown) power_off(); elapsed+=ms; }
''' + ''.join(function(source, name) for name in ('motor_off_locked', 'motor_task', 'vibrate', 'power_off')) + r'''
static void run(int scenario) {
    elapsed=writes=delays=0; setting=scenario!=2; shutdown=scenario>=3; stop_fails=scenario==4;
    s_motor_enabled=true;   /* a power off that reached the PMIC never came back */
    vibrate(scenario==1 ? 999 : 30);
    assert(!writes && !elapsed); /* requests do not wait or touch I2C */
    int result=setjmp(done); if(!result) motor_task(NULL);
    if(stop_fails) {
        /* The stop failed, and power off still reached the PMIC. */
        assert(result==2 && s_motor_on && duty==MOTOR_DUTY && writes==1 && !elapsed);
        return;
    }
    assert(result==(shutdown ? 2 : 1) && !s_motor_on && !duty);
    assert(writes==(setting ? 2 : 0) && elapsed==(setting && !shutdown ? (scenario==1 ? 120 : 30) : 0));
}
int main(void) { for(int scenario=0;scenario<5;scenario++) run(scenario); }
'''
        with tempfile.TemporaryDirectory() as tmp:
            c = Path(tmp) / 'motor.c'
            c.write_text(code)
            subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(c), '-o', str(c.with_suffix(''))], check=True)
            subprocess.run([str(c.with_suffix(''))], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1'})
