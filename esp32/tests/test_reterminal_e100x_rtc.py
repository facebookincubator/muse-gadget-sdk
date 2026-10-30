"""PCF8563 register, date-validation and restart-on-error checks on the host."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class E100xRtcTest(unittest.TestCase):
    def test_calendar_and_clock_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            headers = {
                "driver/i2c_master.h": """#pragma once
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define I2C_ADDR_BIT_LEN_7 7
typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
int i2c_master_probe(void *,int,int);
int i2c_master_bus_add_device(void *,const i2c_device_config_t *,void **);
int i2c_master_bus_rm_device(void *);
int i2c_master_transmit_receive(void *,const uint8_t *,size_t,uint8_t *,size_t,int);
int i2c_master_transmit(void *,const uint8_t *,size_t,int);
""",
                "freertos/FreeRTOS.h": """#pragma once
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
""",
                "freertos/semphr.h": """#pragma once
typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(void *m,int t) { (void)m;(void)t;return 1; }
static inline void xSemaphoreGive(void *m) { (void)m; }
""",
                "freertos/task.h": "#pragma once\nstatic inline void vTaskDelay(int ms) { (void)ms; }\n",
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
            cc = shlex.split(os.environ.get("CC", "gcc"))
            exe = out / "rtc.exe"
            result = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-I", str(out), "-I", str(ROOT / "main"),
                                     str(ROOT / "tests/reterminal_e100x_rtc_harness.c"),
                                     str(ROOT / "main/reterminal_e100x_rtc.c"), "-o", str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
