"""Compile the real capture module against fault-injectable host I2S fakes."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


class E100xMicrophoneTest(unittest.TestCase):
    def test_capture_pins_and_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            files = {
                "sdkconfig.h": "#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n",
                "driver/gpio.h": """#pragma once
#include <stdint.h>
#define ESP_OK 0
#define GPIO_NUM_38 38
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_MODE_OUTPUT 1
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
int gpio_config(const gpio_config_t *);
int gpio_set_level(int,int);
""",
                "driver/i2s_pdm.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
typedef void *i2s_chan_handle_t;
typedef struct { int port,role,dma_desc_num,dma_frame_num; } i2s_chan_config_t;
typedef struct { struct { int rate; } clk_cfg; struct { int bits,mode; } slot_cfg; struct { int clk,din; } gpio_cfg; } i2s_pdm_rx_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(p,r) {.port=p,.role=r}
#define I2S_PDM_RX_CLK_DEFAULT_CONFIG(r) {.rate=r}
#define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(b,m) {.bits=b,.mode=m}
int i2s_new_channel(const i2s_chan_config_t *,void *,i2s_chan_handle_t *);
int i2s_channel_init_pdm_rx_mode(i2s_chan_handle_t,const i2s_pdm_rx_config_t *);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_del_channel(i2s_chan_handle_t);
int i2s_channel_read(i2s_chan_handle_t,void *,size_t,size_t *,unsigned);
""",
                "esp_heap_caps.h": """#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_malloc(size_t,int);
""",
                "esp_timer.h": "#pragma once\n",
                "freertos/FreeRTOS.h": "#pragma once\n#define pdMS_TO_TICKS(ms) (ms)\n",
                "freertos/task.h": "#pragma once\nvoid vTaskDelay(int);\n",
                "mbedtls/base64.h": "#pragma once\n#include <stddef.h>\nint mbedtls_base64_encode(unsigned char *,size_t,size_t *,const unsigned char *,size_t);\n",
            }
            for filename, contents in files.items():
                dest = out / filename
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(contents, encoding="utf-8")
            exe = out / "mic.exe"
            (out / "driver").mkdir(exist_ok=True)
            if not (out / "driver/i2c_master.h").exists():
                (out / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
            if not (out / "sdkconfig.h").exists():
                (out / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n")
            cc = shlex.split(os.environ.get("CC", "gcc"))
            result = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(out), "-I", str(JSON), "-I", str(ROOT / "main"),
                                     str(ROOT / "tests/reterminal_e100x_microphone_harness.c"),
                                     str(ROOT / "main/reterminal_e100x_microphone.c"), str(JSON / "cJSON.c"),
                                     "-lm", "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
