"""Real battery driver against ADC fakes: averaging, curve and failure cleanup."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class E100xBatteryTest(unittest.TestCase):
    def test_sampling_and_curve(self):
        for model in (1,2,3):
            with self.subTest(model=model): self.check_model(model)

    def check_model(self, model):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            headers = {
                "sdkconfig.h": "#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n",
                "driver/gpio.h": """#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef int esp_err_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
int gpio_config(const gpio_config_t *);
int gpio_set_level(int,int);
""",
                "esp_adc/adc_oneshot.h": """#pragma once
#define ADC_CHANNEL_0 0
#define ADC_UNIT_1 1
#define ADC_ULP_MODE_DISABLE 0
#define ADC_BITWIDTH_DEFAULT 12
#define ADC_ATTEN_DB_12 12
typedef void *adc_oneshot_unit_handle_t;
typedef int adc_unit_t;
typedef int adc_channel_t;
typedef struct { int unit_id,ulp_mode; } adc_oneshot_unit_init_cfg_t;
typedef struct { int bitwidth,atten; } adc_oneshot_chan_cfg_t;
int adc_oneshot_io_to_channel(int,int *,int *);
int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *,void **);
int adc_oneshot_del_unit(void *);
int adc_oneshot_config_channel(void *,int,const adc_oneshot_chan_cfg_t *);
int adc_oneshot_read(void *,int,int *);
""",
                "esp_adc/adc_cali.h": "#pragma once\ntypedef void *adc_cali_handle_t;\nint adc_cali_raw_to_voltage(void *,int,int *);\n",
                "esp_adc/adc_cali_scheme.h": """#pragma once
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
typedef struct { int unit_id,chan,atten,bitwidth; } adc_cali_curve_fitting_config_t;
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *,void **);
""",
                "esp_log.h": "#pragma once\n#define ESP_LOGW(t,...) ((void)(t))\n#define ESP_LOGI(t,...) ((void)(t))\n",
                "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n",
                "freertos/task.h": "#pragma once\nvoid vTaskDelay(int);\n",
            }
            for filename, contents in headers.items():
                dest = out / filename
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(contents, encoding="utf-8")
            (out / "driver").mkdir(exist_ok=True)
            if not (out / "driver/i2c_master.h").exists():
                (out / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
            if not (out / "sdkconfig.h").exists():
                (out / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n")
            (out / "sdkconfig.h").write_text(f"#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 {int(model==1)}\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 {int(model==2)}\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951 {int(model==3)}\n#define CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE 0\n")
            board = (ROOT / "main/reterminal_e100x.c").read_text()
            data = board.split("// ---- Board data (host-tested) ----",1)[1].split("// ---- Board resources ----",1)[0]
            (out / "board_data.c").write_text('#include "reterminal_e100x_board.h"\n'+data)
            cc = shlex.split(os.environ.get("CC", "gcc"))
            exe = out / "battery.exe"
            result = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(out), "-I", str(ROOT / "main"),
                                     str(ROOT / "tests/reterminal_e100x_battery_harness.c"),
                                     str(ROOT / "main/battery_adc.c"), str(out / "board_data.c"), "-o", str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
