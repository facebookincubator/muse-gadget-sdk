"""Exercise real SD command/file logic with a temporary host FAT mount fake."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


class E100xSdTest(unittest.TestCase):
    def test_binary_roundtrip_and_paths(self):
        for model in (2,3):
            with self.subTest(model=model): self.check_model(model)


    def check_model(self, model):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "sdcard").mkdir()
            headers = {
                "sdkconfig.h": "#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n",
                "freertos/FreeRTOS.h": """#pragma once
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
""",
                "freertos/semphr.h": """#pragma once
typedef void *SemaphoreHandle_t;
void *xSemaphoreCreateMutex(void);
int xSemaphoreTake(void *,int);
void xSemaphoreGive(void *);
""",
                "freertos/task.h": """#pragma once
static inline int xTaskCreate(void (*f)(void *),const char *n,unsigned s,void *a,unsigned p,void *h) { (void)f;(void)n;(void)s;(void)a;(void)p;(void)h;return 1; }
void vTaskDelay(int);
""",
                "driver/gpio.h": """#pragma once
#include <stdint.h>
#define ESP_OK 0
typedef int esp_err_t;
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_39 39
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ENABLE 1
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en; } gpio_config_t;
int gpio_config(const gpio_config_t *);
int gpio_get_level(int);
int gpio_set_level(int,int);
const char *esp_err_to_name(int);
""",
                "driver/sdspi_host.h": """#pragma once
#define SPI2_HOST 2
typedef struct { int slot,max_freq_khz; } sdmmc_host_t;
typedef struct { int host_id,gpio_cs; } sdspi_device_config_t;
#define SDSPI_HOST_DEFAULT() {0}
#define SDSPI_DEVICE_CONFIG_DEFAULT() {0}
""",
                "sdmmc_cmd.h": """#pragma once
#include <stdint.h>
typedef struct { struct { uint32_t capacity,sector_size; } csd; } sdmmc_card_t;
""",
                "esp_vfs_fat.h": """#pragma once
#include <stdbool.h>
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
typedef struct { bool format_if_mount_failed; int max_files; } esp_vfs_fat_sdmmc_mount_config_t;
int esp_vfs_fat_sdspi_mount(const char *,const sdmmc_host_t *,const sdspi_device_config_t *,const esp_vfs_fat_sdmmc_mount_config_t *,sdmmc_card_t **);
int esp_vfs_fat_sdcard_unmount(const char *,sdmmc_card_t *);
""",
                "esp_log.h": "#pragma once\n#define ESP_LOGW(t,...) ((void)(t))\n#define ESP_LOGI(t,...) ((void)(t))\n",
                "mbedtls/base64.h": "#pragma once\n#include <stddef.h>\nint mbedtls_base64_encode(unsigned char *,size_t,size_t *,const unsigned char *,size_t);\nint mbedtls_base64_decode(unsigned char *,size_t,size_t *,const unsigned char *,size_t);\n",
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
            (out / "sdkconfig.h").write_text(f"#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 {int(model==2)}\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951 {int(model==3)}\n")
            cc = shlex.split(os.environ.get("CC", "gcc"))
            exe = out / "sd.exe"
            result = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     '-DSD_MOUNT="sdcard"', "-I", str(out), "-I", str(JSON),
                                     "-I", str(ROOT / "main"), str(ROOT / "tests/reterminal_e100x_sd_harness.c"),
                                     str(JSON / "cJSON.c"),
                                     "-lm", "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=out, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
