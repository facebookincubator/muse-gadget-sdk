# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Run the actual TriviaPOD register controls against fault-injecting fakes."""
from pathlib import Path
import tempfile
import unittest
from test_stopwatch_power import COMMON, StopWatchPower, defines, function

ROOT = Path(__file__).resolve().parents[1]


class TriviaPodPowerTest(unittest.TestCase):
    build = StopWatchPower.build
    run_cases = StopWatchPower.run_cases

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.board = (ROOT / "components/muse/boards/board_m5stack_triviapod.c").read_text()

    def test_register_readback_preservation_and_recovery(self):
        code = COMMON + defines(self.board, r"PMIC_\w+|IOE_\w+") + r'''
#define portMAX_DELAY 1000
#define ESP_ERR_NO_MEM 257
#define ESP_RETURN_ON_FALSE(expr, error, ...) do { if (!(expr)) return error; } while (0)
typedef void *SemaphoreHandle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags; } i2c_master_bus_config_t;
#define I2C_ADDR_BIT_LEN_7 0
#define I2C_NUM_0 0
#define I2C_SDA 47
#define I2C_SCL 48
#define I2C_CLK_SRC_DEFAULT 0
static void *s_i2c, *s_pmic, *s_ioe, *s_pmic_lock, *s_ioe_lock;
static int depth, allocations, devices, bus_creations, fail_reg = -1, ignore_reg = -1;
static uint8_t pmic[256], ioe[256];
static void *xSemaphoreCreateMutex(void) { ++allocations; return (void *)4; }
static void xSemaphoreTake(void *lock, int wait) { assert(lock && wait == portMAX_DELAY && !depth++); }
static void xSemaphoreGive(void *lock) { assert(lock && depth-- == 1); }
static int i2c_new_master_bus(const i2c_master_bus_config_t *cfg, void **out) {
    assert(cfg->sda_io_num == 47 && cfg->scl_io_num == 48); ++bus_creations; *out=(void *)1; return 0;
}
static int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *cfg, void **out) {
    assert(bus == s_i2c); ++devices; *out=(void *)(uintptr_t)cfg->device_address; return 0;
}
static int reg_read(void *dev, uint8_t reg, uint8_t *out, size_t n) {
    assert(depth == 1); if (reg == fail_reg) return -7;
    memcpy(out, (dev == s_pmic ? pmic : ioe) + reg, n); return 0;
}
static int reg_write(void *dev, uint8_t reg, const uint8_t *data, size_t n) {
    assert(depth == 1); if (reg == fail_reg) return -7;
    if (reg != ignore_reg) memcpy((dev == s_pmic ? pmic : ioe) + reg, data, n);
    return 0;
}
''' + "\n".join(function(self.board, n) for n in (
            "pmic_update", "ioe_update", "add_device", "power_i2c_init",
            "muse_triviapod_charge", "muse_triviapod_motor")) + r'''
int main(int argc, char **argv) {
    assert(argc == 2); int test=atoi(argv[1]);
    pmic[6]=0xb8;
    assert(!muse_triviapod_charge(true) && pmic[6] == 0xb9);
    assert(!muse_triviapod_charge(false) && pmic[6] == 0xb8);
    assert(!muse_triviapod_charge(true) && pmic[6] == 0xb9);
    assert(devices == 1 && bus_creations == 1 && allocations == 1);
    if (test == 0) {
        ignore_reg=6; assert(muse_triviapod_charge(false) == ESP_FAIL && pmic[6] == 0xb9);
        ignore_reg=-1; fail_reg=6; assert(muse_triviapod_charge(false) == -7);
        fail_reg=-1; assert(!muse_triviapod_charge(true));
    } else {
        ioe[0x03]=0xa5; ioe[0x04]=0x22;
        ioe[0x05]=0x5a; ioe[0x06]=0x77;
        ioe[0x13]=0xcc; ioe[0x14]=0x33;
        assert(!muse_triviapod_motor(false));
        assert(ioe[0x03] == 0xa5 && ioe[0x04] == 0x23);
        assert(ioe[0x05] == 0x5a && ioe[0x06] == 0x76);
        assert(ioe[0x13] == 0xcc && ioe[0x14] == 0x32);
        assert(devices == 2 && bus_creations == 1 && allocations == 2);
        if (test == 1) {
            assert(!muse_triviapod_motor(true));
            assert(ioe[0x25] == 0xd0 && ioe[0x26] == 0x07);
            assert(ioe[0x1b] == 0x99 && ioe[0x1c] == 0x89);
            assert(!muse_triviapod_motor(false) && !ioe[0x1b] && !ioe[0x1c]);
        } else if (test == 2) {
            ignore_reg=0x1b; assert(muse_triviapod_motor(true) == ESP_FAIL);
        } else if (test == 3) {
            ioe[0x25]=0; ignore_reg=0x25; assert(muse_triviapod_motor(true) == ESP_FAIL);
        } else if (test == 4) {
            ioe[0x04] &= ~1; ignore_reg=3; assert(muse_triviapod_motor(true) == ESP_FAIL);
        } else if (test == 5) {
            fail_reg=0x1b; assert(muse_triviapod_motor(false) == -7);
            fail_reg=-1; assert(!muse_triviapod_motor(false));
        } else {
            assert(!muse_triviapod_motor(true));
            fail_reg=0x25; assert(muse_triviapod_motor(false) == -7);
            assert(!ioe[0x1b] && !ioe[0x1c]);
        }
    }
    assert(!depth); return 0;
}
'''
        self.run_cases(self.build("registers", code), 7)

    def test_early_normal_boot_restores_charging(self):
        body = function(self.board, "init")
        self.assertLess(body.index("muse_triviapod_charge(true)"), body.index("add_device(IOE_ADDR"))
        self.assertLess(body.index("muse_triviapod_charge(true)"), body.index("panel reset"))


if __name__ == "__main__":
    unittest.main()
