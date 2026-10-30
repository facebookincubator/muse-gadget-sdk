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

from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Compile the production driver against controllable ESP-IDF calls. Each harness
# invocation starts with fresh driver state, like a reboot.
FAKES = r"""
#pragma once
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define GPIO_NUM_21 21
#define GPIO_MODE_OUTPUT 1
#define ADC_UNIT_1 0
#define ADC_CHANNEL_0 0
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_12 12
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct { int unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten, bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { int unit_id, chan, atten, bitwidth; } adc_cali_curve_fitting_config_t;
int gpio_config(const gpio_config_t *);
int gpio_set_level(int, int);
int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
int adc_oneshot_config_channel(adc_oneshot_unit_handle_t, int, const adc_oneshot_chan_cfg_t *);
int adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *, adc_cali_handle_t *);
int adc_oneshot_read(adc_oneshot_unit_handle_t, int, int *);
int adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
"""

HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include "fake.h"
#include "reterminal_battery.h"
static int failure, enabled, deleted, allocations, samples, pin_mv=1900;
int gpio_config(const gpio_config_t *c) {
    assert(c->pin_bit_mask == (1ULL<<21) && c->mode == GPIO_MODE_OUTPUT);
    return failure==1 ? -1 : 0;
}
int gpio_set_level(int pin, int level) {
    assert(pin==21);
    if (failure==2 && level) return -1;
    enabled=level; return 0;
}
int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, void **h) {
    assert(c->unit_id==ADC_UNIT_1);
    if(failure==3) return -1;
    allocations++; *h=(void*)1; return 0;
}
int adc_oneshot_config_channel(void *h, int ch, const adc_oneshot_chan_cfg_t *c) {
    assert(h && ch==ADC_CHANNEL_0 && c->atten==ADC_ATTEN_DB_12 && c->bitwidth==12);
    return failure==4 ? -1 : 0;
}
int adc_oneshot_del_unit(void *h) {assert(h); deleted++; return 0;}
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, void **h) {
    assert(c->unit_id==ADC_UNIT_1 && c->chan==ADC_CHANNEL_0 && c->atten==ADC_ATTEN_DB_12 && c->bitwidth==12);
    if(failure==5) return -1;
    *h=(void*)2; return 0;
}
int adc_oneshot_read(void *h, int ch, int *raw) {
    assert(h && ch==ADC_CHANNEL_0);
    *raw=2000+(samples++ % 8);
    return failure==6 && samples==4 ? -1 : 0;
}
int adc_cali_raw_to_voltage(void *h, int raw, int *mv) {
    assert(h && raw==2003); // mean of eight distinct raw samples
    *mv=pin_mv; return failure==7 ? -1 : 0;
}
int main(int argc, char **argv) {
    assert(argc==2); failure=atoi(argv[1]);
    int mv=-42, pct=-42;
    assert(!reterminal_battery_read(&mv,&pct));
    reterminal_battery_init();
    if(failure) {
        assert(!reterminal_battery_read(&mv,&pct));
        assert(mv==-42 && pct==-42);
        if(failure<=5) assert(!enabled);
        if(failure==4 || failure==5) assert(deleted==1);
        return 0;
    }
    assert(enabled);
    reterminal_battery_init(); // repeated init must not allocate another unit
    assert(!deleted && allocations==1);
    assert(!reterminal_battery_read(NULL,&pct));
    assert(!reterminal_battery_read(&mv,NULL));
    assert(reterminal_battery_read(&mv,&pct));
    assert(mv==3800 && pct==60 && samples==8);
    const int curve[][2]={{4150,100},{3960,90},{3910,80},{3850,70},{3800,60},
        {3750,50},{3680,40},{3580,30},{3490,20},{3410,10},{3300,5},{3270,0}};
    for(size_t i=0;i<sizeof(curve)/sizeof(curve[0]);i++) {
        pin_mv=curve[i][0]/2;
        assert(reterminal_battery_read(&mv,&pct));
        assert(mv==curve[i][0] && pct==curve[i][1]);
    }
    int prev=-1;
    for(pin_mv=1250;pin_mv<=2200;pin_mv++) {
        assert(reterminal_battery_read(&mv,&pct));
        assert(mv==pin_mv*2 && pct>=prev && pct>=0 && pct<=100);
        prev=pct;
    }
    pin_mv=1888; assert(reterminal_battery_read(&mv,&pct)); assert(pct==55);
    pin_mv=1200; mv=pct=-42;
    assert(!reterminal_battery_read(&mv,&pct)); assert(mv==-42 && pct==-42);
    return 0;
}
"""


class ReterminalBatteryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.out = Path(cls.tmp.name)
        (cls.out / "fake.h").write_text(FAKES)
        for name in ("driver/gpio.h", "esp_adc/adc_cali.h",
                     "esp_adc/adc_cali_scheme.h", "esp_adc/adc_oneshot.h", "esp_log.h"):
            path = cls.out / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "fake.h"\n')
        (cls.out / "harness.c").write_text(HARNESS)
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.flags = ["-std=c11", "-Wall", "-Wextra", "-Werror",
                     "-I", str(cls.out), "-I", str(ROOT / "main")]
        result = subprocess.run([*cls.cc, *cls.flags, str(cls.out / "harness.c"),
                                 str(ROOT / "main/reterminal_battery.c"),
                                 "-o", str(cls.out / "battery")], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_calibrated_voltage_percentage_and_invalid_readings(self):
        self.run_case(0)

    def test_setup_and_read_failures_leave_status_unavailable(self):
        for failure in range(1, 8):
            with self.subTest(failure=failure):
                self.run_case(failure)

    def run_case(self, failure):
        result = subprocess.run([str(self.out / "battery"), str(failure)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_initialization_compiles_without_sht4x_for_both_panels(self):
        app = (ROOT / "main/app.c").read_text()
        include = app.index('#include "reterminal_battery.h"')
        include_block = app[app.rfind("#if", 0, include):app.index("#endif", include)+6]
        init = app.index("    reterminal_battery_init();")
        init_block = app[app.rfind("#if", 0, init):app.index("#endif", init)+6]
        source = include_block + "\nvoid app_run(void) {\n" + init_block + "\n}\n"
        for panel in ("UC8179", "SPECTRA6"):
            result = subprocess.run([*self.cc, *self.flags,
                "-DCONFIG_HOMEHUB_RETERMINAL_SHT4X=0",
                "-DCONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_" + panel + "=1",
                "-x", "c", "-fsyntax-only", "-"], input=source, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_health_fields_for_reterminal_ui_and_other_boards(self):
        json_dir = Path(os.environ.get(
            "CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))
        result = subprocess.run([*self.cc, "-I", str(json_dir), "-c",
            str(json_dir / "cJSON.c"), "-o", str(self.out / "cjson.o")],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index("    cJSON *battery_pct = nullptr")
        block = noise[start:noise.index('    cJSON_AddItemToObject(health, "metrics"', start)]
        source = r'''
#include <cassert>
#include "cJSON.h"
static int reads;
static bool available;
bool reterminal_battery_read(int *mv, int *pct) {
    reads++; *mv=3800; *pct=60; return available;
}
struct muse_power_t {int battery_pct, battery_mv; bool charging, usb;};
muse_power_t muse_state_power() {return {77,4000,true,true};}
cJSON *metrics_for_test() {
    cJSON *metrics = cJSON_CreateObject();
''' + block + r'''
    return metrics;
}
int main() {
    for(int ok=0;ok<2;ok++) {
        available=ok; reads=0;
        cJSON *m=metrics_for_test();
        auto pct=cJSON_GetObjectItem(m,"battery_pct");
        auto mv=cJSON_GetObjectItem(m,"battery_mv");
        auto charging=cJSON_GetObjectItem(m,"charging");
        auto usb=cJSON_GetObjectItem(m,"usb_power");
#if CONFIG_MUSE_ENABLED
        assert(reads==0 && pct->valueint==77 && mv->valueint==4000);
        assert(cJSON_IsTrue(charging) && cJSON_IsTrue(usb));
#elif CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 || CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6
        assert(reads==1);
        if(ok) {assert(pct->valueint==60 && mv->valueint==3800);}
        else {assert(cJSON_IsNull(pct) && cJSON_IsNull(mv));}
        assert(cJSON_IsNull(charging) && cJSON_IsNull(usb));
#else
        assert(reads==0 && cJSON_IsNull(pct) && cJSON_IsNull(mv));
        assert(cJSON_IsNull(charging) && cJSON_IsNull(usb));
#endif
        cJSON_Delete(m);
    }
}
'''
        path = self.out / "health.cpp"
        path.write_text(source)
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        for config in ("CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179",
                       "CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6",
                       "CONFIG_MUSE_ENABLED", "OTHER_BOARD"):
            with self.subTest(config=config):
                result = subprocess.run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-D"+config+"=1", "-I", str(json_dir), str(path),
                    str(self.out / "cjson.o"), "-o", str(self.out / "health")],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(self.out / "health")], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
