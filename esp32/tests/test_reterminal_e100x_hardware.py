"""Host checks only: command ownership, validation and full registration size."""
import os
import json
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get("CJSON_SOURCE_DIR", ROOT / "managed_components/espressif__cjson/cJSON"))


class E100xHardwareTest(unittest.TestCase):
    def test_commands_and_registration(self):
        for board, inputs, hardware in [(1,1,1),(2,1,1),(3,1,1),(3,1,0),(3,0,1)]:
            with self.subTest(board=board, inputs=inputs, hardware=hardware):
                self.check_profile(board, inputs, hardware)

    def check_profile(self, board, inputs, hardware):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "freertos").mkdir()
            (out / "freertos/FreeRTOS.h").write_text(
                "#pragma once\n#define pdPASS 1\n#define pdMS_TO_TICKS(x) (x)\n"
                "typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n"
                "#define portENTER_CRITICAL(x) ((void)(x))\n"
                "#define portEXIT_CRITICAL(x) ((void)(x))\n")
            (out / "freertos/task.h").write_text(
                "#pragma once\n#include <stddef.h>\n"
                "typedef void (*TaskFunction_t)(void *);\n"
                "int xTaskCreate(TaskFunction_t,const char *,unsigned,void *,unsigned,void *);\n"
                "void vTaskDelay(int);\nvoid vTaskDelete(void *);\n")
            (out / "esp_timer.h").write_text(
                "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
            (out / "esp_wifi.h").write_text(
                "#pragma once\n"
                "typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;\n"
                "#define ESP_OK 0\nint esp_wifi_get_ps(wifi_ps_type_t *ps);\n")
            config = ("#pragma once\n#define CONFIG_SPIRAM 1\n#define CONFIG_HOMEHUB_RETERMINAL_E100X_HARDWARE 1\n" if board != 3 else
                      f"#pragma once\n#define CONFIG_SPIRAM 1\n#define CONFIG_HOMEHUB_RETERMINAL_E1003_INPUT {inputs}\n#define CONFIG_HOMEHUB_RETERMINAL_E1003_HARDWARE {hardware}\n")
            config += f"#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 {int(board==2)}\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179 {int(board==1)}\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_IT8951 {int(board==3)}\n"
            (out / "sdkconfig.h").write_text(config)
            (out / "esp_log.h").write_text("#pragma once\n#define ESP_LOGW(...) ((void)0)\n")
            source = (ROOT / "main/reterminal_e100x.c").read_text()
            start = source.index("void reterminal_e100x_battery_metrics(")
            end = source.index("\n}\n", start)+3
            (out / "battery_cache.inc").write_text(source[start:end])
            noise = (ROOT / "main/noise_control.cpp").read_text(encoding="utf-8")
            start = noise.index("static cJSON *string_param(")
            end = noise.index("// ---- OTA callback", start)
            (out / "noise_register.inc").write_text(noise[start:end], encoding="utf-8")
            start = noise.index("static char *wrap_result_json(")
            end = noise.index("// ---- Reconnect policy", start)
            (out / "noise_result.inc").write_text(noise[start:end], encoding="utf-8")
            flags = ["-Wall", "-Wextra", "-Werror", "-I", str(out),
                     "-I", str(JSON), "-I", str(ROOT / "main")]
            (out / "driver").mkdir(exist_ok=True)
            if not (out / "driver/i2c_master.h").exists():
                (out / "driver/i2c_master.h").write_text("#pragma once\ntypedef void *i2c_master_bus_handle_t;\n")
            if not (out / "sdkconfig.h").exists():
                (out / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6 1\n")
            cc = shlex.split(os.environ.get("CC", "gcc"))
            cxx = shlex.split(os.environ.get("CXX", "g++"))
            objects = []
            for source in [ROOT / "tests/reterminal_e100x_hardware_harness.c",
                           ROOT / "main/reterminal_e100x_commands.c", JSON / "cJSON.c"]:
                obj = out / (source.stem + ".o")
                result = subprocess.run([*cc, "-std=c11", *flags, "-c", str(source),
                                         "-o", str(obj)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                objects.append(str(obj))
            exe = out / "hardware.exe"
            result = subprocess.run([*cxx, "-std=c++17", *flags,
                                     str(ROOT / "tests/reterminal_e100x_register_harness.cpp"),
                                     *objects, "-lm", "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            measured = re.search(r"register_bytes=(\d+)", result.stderr)
            self.assertIsNotNone(measured, result.stderr)
            self.assertLess(int(measured.group(1)), 16384)
            if os.environ.get("E100X_REG_SIZE_REPORT") == "1":
                print(f"E100{board} inputs={inputs} hardware={hardware}: {measured.group(1)} bytes")
            actual = json.loads(result.stdout)
            def shape(value):
                if isinstance(value, dict):
                    return {k: shape(v) for k,v in value.items() if k != "description"}
                return value
            expected = json.loads((ROOT / "tests/e100x_command_contract.json").read_text())["e1003" if board == 3 else "e10012"]
            if not inputs:
                expected = {k:v for k,v in expected.items() if k not in ("buttons.read","touch.read","events.read","events.wait")}
            if not hardware:
                expected = {k:v for k,v in expected.items() if k in ("buttons.read","touch.read","events.read","events.wait")}
            actual = {k:v for k,v in actual.items() if k.split(".")[0] in ("buttons","touch","events","battery","buzzer","rtc","sd","microphone","pmic","led")}
            self.assertEqual(shape(actual), expected)


if __name__ == "__main__":
    unittest.main()
