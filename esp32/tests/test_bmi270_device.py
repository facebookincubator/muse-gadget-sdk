# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Execute the BMI270 registry/lifecycle adapter with a fake vendor API."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / 'components/muse/boards'

FREERTOS = '''#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define pdTRUE 1
#define tskNO_AFFINITY (-1)
#define portMAX_DELAY UINT32_MAX
void vTaskDelay(unsigned ticks);
unsigned ulTaskNotifyTake(int clear, unsigned ticks);
void xTaskNotifyGive(TaskHandle_t task);
int xTaskCreatePinnedToCoreWithCaps(void (*f)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *out, int core, unsigned caps);
'''
FAKES = {
    'esp_attr.h': '#define EXT_RAM_BSS_ATTR\n',
    'esp_heap_caps.h': '#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n',
    'esp_log.h': '#pragma once\n#define ESP_LOG_NONE 0\nvoid esp_log_level_set(const char *tag, int level);\n#include <stdio.h>\n#define ESP_LOGE(tag,...) ((void)(tag), (void)fprintf(stderr,__VA_ARGS__))\n#define ESP_LOGW ESP_LOGE\n#define ESP_LOGI ESP_LOGE\n',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    'freertos/FreeRTOS.h': FREERTOS,
    'freertos/task.h': '#include "FreeRTOS.h"\n',
    'freertos/idf_additions.h': '#include "FreeRTOS.h"\n',
    'driver/i2c_master.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define I2C_ADDR_BIT_LEN_7 7
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, unsigned addr, int timeout);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int timeout);
const char *esp_err_to_name(esp_err_t err);
''',
    'bmi270_api.h': '''#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
#define BMI2_OK 0
#define BMI2_E_COM_FAIL (-2)
#define BMI270_CHIP_ID 0x24
#define BMI2_CHIP_ID_ADDR 0
#define BMI2_ACCEL 0
#define BMI2_GYRO 1
#define BMI2_AUX 2
#define BMI2_TEMP 32
#define BMI2_STEP_COUNTER 7
#define BMI2_ENABLE 1
#define BMI2_DISABLE 0
#define BMI2_ACC_ODR_100HZ 8
#define BMI2_GYR_ODR_100HZ 8
#define BMI2_ACC_RANGE_8G 2
#define BMI2_GYR_RANGE_2000 0
#define BMI2_ACC_NORMAL_AVG4 2
#define BMI2_GYR_NORMAL_MODE 2
#define BMI2_PERF_OPT_MODE 1
#define BMI2_POWER_OPT_MODE 0
#define BMI2_DRDY_ACC 0x80
#define BMI2_DRDY_GYR 0x40
#define BMI2_GYRO_CROSS_SENS_ENABLE 1
#define BMI2_CRT_RTOSK_ENABLE 2
struct bmi2_dev { int valid; };
typedef struct bmi2_dev *bmi270_handle_t;
struct bmi2_sens_axes_data { int16_t x, y, z; };
struct bmi2_sens_data { struct bmi2_sens_axes_data acc, gyr; uint8_t status; };
struct bmi2_feat_sensor_data { uint8_t type; union { uint32_t step_counter_output; } sens_data; };
struct bmi2_sens_config { uint8_t type; union {
    struct { uint8_t odr, bwp, filter_perf, range; } acc;
    struct { uint8_t odr, bwp, filter_perf, ois_range, range, noise_perf; } gyr;
} cfg; };
esp_err_t bmi270_sensor_create_from_master_bus_with_address(i2c_master_bus_handle_t bus,
    uint8_t addr, bmi270_handle_t *out, const uint8_t *config, uint64_t features);
esp_err_t bmi270_sensor_del(bmi270_handle_t *out);
int8_t bmi2_soft_reset(bmi270_handle_t dev);
int8_t bmi2_set_adv_power_save(uint8_t enable, bmi270_handle_t dev);
int8_t bmi270_sensor_enable(const uint8_t *sensors, uint8_t n, bmi270_handle_t dev);
int8_t bmi2_sensor_enable(const uint8_t *sensors, uint8_t n, bmi270_handle_t dev);
int8_t bmi2_sensor_disable(const uint8_t *sensors, uint8_t n, bmi270_handle_t dev);
int8_t bmi270_get_sensor_config(struct bmi2_sens_config *c, uint8_t n, bmi270_handle_t dev);
int8_t bmi270_set_sensor_config(struct bmi2_sens_config *c, uint8_t n, bmi270_handle_t dev);
int8_t bmi2_get_sensor_data(struct bmi2_sens_data *data, bmi270_handle_t dev);
int8_t bmi270_get_feature_data(struct bmi2_feat_sensor_data *data, uint8_t n, bmi270_handle_t dev);
''',
}

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "bmi270_device.c"
static struct bmi2_dev vendor;
static int creates, deletes, tasks, descriptors, pushes, resets, delete_failures;
static bool enabled[33], aps, fail_create, fail_config, fail_suspend;
static uint32_t step_count=100;
static int64_t clock_us=1000000;
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ticks) { clock_us+=ticks*1000; }
unsigned ulTaskNotifyTake(int clear, unsigned ticks) { (void)clear; (void)ticks; return 0; }
void xTaskNotifyGive(TaskHandle_t task) { assert(task); }
int xTaskCreatePinnedToCoreWithCaps(void (*f)(void *), const char *name, unsigned stack,
    void *arg, unsigned priority, TaskHandle_t *out, int core, unsigned caps) {
    (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    assert(f==imu_task && (caps & MALLOC_CAP_SPIRAM)); tasks++; *out=&tasks; return pdPASS;
}
void esp_log_level_set(const char *tag, int level) { assert(!strcmp(tag,"BMI2_ESP32") && level==ESP_LOG_NONE); }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "fake"; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, unsigned addr, int timeout) {
    assert(bus==(void *)1 && addr==0x68 && timeout==50); return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out) {
    assert(bus==(void *)1 && cfg->device_address==0x68); *out=(void *)2; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { assert(dev==(void *)2); return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tn, uint8_t *rx, size_t rn, int timeout) {
    assert(dev==(void *)2 && *tx==BMI2_CHIP_ID_ADDR && tn==1 && rn==1 && timeout==50); *rx=BMI270_CHIP_ID; return ESP_OK;
}
bool capabilities_registry_register(const capability_t *d) { assert(d); descriptors++; return true; }
void capabilities_registry_push(const capability_t *d, const float *v, int n) { assert(d && v && n>0); pushes++; }
void capabilities_registry_event(const capability_t *d, int event) { assert(d && event>=0); }
esp_err_t bmi270_sensor_create_from_master_bus_with_address(i2c_master_bus_handle_t bus,
    uint8_t addr, bmi270_handle_t *out, const uint8_t *config, uint64_t features) {
    assert(bus==(void *)1 && addr==0x68 && !config && features==3);
    creates++; vendor.valid=!fail_create; *out=&vendor; return fail_create ? ESP_FAIL : ESP_OK;
}
esp_err_t bmi270_sensor_del(bmi270_handle_t *out) { assert(*out==&vendor); deletes++; if(delete_failures>0) { delete_failures--; return ESP_FAIL; } *out=NULL; return ESP_OK; }
int8_t bmi2_soft_reset(bmi270_handle_t dev) { assert(dev==&vendor && dev->valid); resets++; memset(enabled,0,sizeof(enabled)); return BMI2_OK; }
int8_t bmi2_set_adv_power_save(uint8_t on, bmi270_handle_t dev) { assert(dev->valid); aps=on; return BMI2_OK; }
static int8_t sensors(const uint8_t *list, uint8_t n, bmi270_handle_t dev, bool on) {
    assert(dev==&vendor && dev->valid); if(!on && fail_suspend && n==5) return BMI2_E_COM_FAIL;
    for(int i=0;i<n;i++) { assert(list[i]<33); enabled[list[i]]=on; } return BMI2_OK;
}
int8_t bmi270_sensor_enable(const uint8_t *l,uint8_t n,bmi270_handle_t d) { return sensors(l,n,d,true); }
int8_t bmi270_sensor_disable(const uint8_t *l,uint8_t n,bmi270_handle_t d) { return sensors(l,n,d,false); }
int8_t bmi2_sensor_enable(const uint8_t *l,uint8_t n,bmi270_handle_t d) { return sensors(l,n,d,true); }
int8_t bmi2_sensor_disable(const uint8_t *l,uint8_t n,bmi270_handle_t d) { return sensors(l,n,d,false); }
int8_t bmi270_get_sensor_config(struct bmi2_sens_config *c,uint8_t n,bmi270_handle_t d) {
    assert(d->valid && n==2 && c[0].type==BMI2_ACCEL && c[1].type==BMI2_GYRO); return BMI2_OK;
}
int8_t bmi270_set_sensor_config(struct bmi2_sens_config *c,uint8_t n,bmi270_handle_t d) {
    assert(d->valid && n==2 && c[0].cfg.acc.range==BMI2_ACC_RANGE_8G && c[1].cfg.gyr.range==BMI2_GYR_RANGE_2000);
    assert(c[0].cfg.acc.odr==BMI2_ACC_ODR_100HZ && c[1].cfg.gyr.odr==BMI2_GYR_ODR_100HZ);
    assert(c[1].cfg.gyr.noise_perf==BMI2_POWER_OPT_MODE);
    return fail_config ? BMI2_E_COM_FAIL : BMI2_OK;
}
int8_t bmi2_get_sensor_data(struct bmi2_sens_data *out,bmi270_handle_t d) {
    assert(d->valid); *out=(struct bmi2_sens_data){.acc={4096,8192,12288},.gyr={0,0,0},.status=0xC0}; return BMI2_OK;
}
int8_t bmi270_get_feature_data(struct bmi2_feat_sensor_data *out,uint8_t n,bmi270_handle_t d) {
    assert(d->valid && n==1 && out->type==BMI2_STEP_COUNTER); out->sens_data.step_counter_output=step_count; return BMI2_OK;
}
int main(int argc,char **argv) {
    assert(argc==2); int mode=atoi(argv[1]);
    const bmi270_device_cfg_t cfg={.bus=(void *)1,.addr=0x68,.axis={1,0,2},.sign={1,-1,1}};
    assert(bmi270_device_register(&cfg)); assert(descriptors==3 && !creates && !tasks);
    assert(!imu_start(&s_devs[DEV_ACCEL],50)); assert(tasks==1 && !creates);
    fail_create=mode==1 || mode==4; fail_config=mode==2; fail_suspend=mode==3;
    delete_failures=mode==4 ? 2 : 0;
    apply(&s.want);
    if(mode==4) {
        assert(s.failed && !s.loaded && s.sensor==&vendor && !vendor.valid);
        assert(creates==1 && deletes==2); assert(cleanup_sensor()); assert(!s.sensor && deletes==3);
        assert(imu_start(&s_devs[DEV_ACCEL],50)==-1); assert(creates==1); return 0;
    }
    if(mode==1 || mode==2) { assert(s.failed && !s.loaded && !s.sensor && creates==1 && deletes==1); return 0; }
    assert(s.loaded && !aps && enabled[BMI2_ACCEL] && !enabled[BMI2_GYRO] && !enabled[BMI2_STEP_COUNTER]);
    sample(clock_us); float a[3]; assert(imu_read(&s_devs[DEV_ACCEL],a)==3);
    assert(a[0]==2 && a[1]==-1 && a[2]==3);
    assert(!imu_start(&s_devs[DEV_STEPS],1)); apply(&s.want); assert(enabled[BMI2_STEP_COUNTER]);
    read_steps(clock_us); float v; assert(imu_read(&s_devs[DEV_STEPS],&v)==1 && v==0);
    step_count+=3; read_steps(clock_us); assert(imu_read(&s_devs[DEV_STEPS],&v)==1 && v==3);
    imu_stop(&s_devs[DEV_STEPS]); apply(&s.want); assert(!enabled[BMI2_STEP_COUNTER] && enabled[BMI2_ACCEL]);
    imu_stop(&s_devs[DEV_ACCEL]); apply(&s.want);
    if(mode==3) { assert(s.failed && resets==1 && deletes==1 && !s.sensor && aps); return 0; }
    assert(aps && !enabled[BMI2_ACCEL] && !enabled[BMI2_GYRO] && !enabled[BMI2_STEP_COUNTER]);
    assert(!imu_start(&s_devs[DEV_GYRO],20)); apply(&s.want);
    assert(creates==1 && !aps && enabled[BMI2_GYRO] && !enabled[BMI2_ACCEL]);
    imu_stop(&s_devs[DEV_GYRO]); apply(&s.want); assert(aps && !enabled[BMI2_GYRO]);
    return 0;
}
'''


class Bmi270AdapterTest(unittest.TestCase):
    def test_lazy_lifecycle_samples_steps_suspend_and_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, text in FAKES.items():
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text)
            (root / 'harness.c').write_text(HARNESS)
            binary = root / 'harness'
            command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I', str(root), '-I', str(BOARD), '-I', str(ROOT / 'components/capabilities_registry/include'),
                       str(root / 'harness.c'), str(BOARD / 'imu_events.c'), '-lm', '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for case in range(5):
                with self.subTest(case=case):
                    ran = subprocess.run([str(binary), str(case)], capture_output=True, text=True)
                    self.assertEqual(ran.returncode, 0, ran.stderr)


if __name__ == '__main__':
    unittest.main()
