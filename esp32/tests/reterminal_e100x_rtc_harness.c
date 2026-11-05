#include <assert.h>
#include <string.h>
#include "reterminal_e100x_rtc.h"
#include "reterminal_e100x_board.h"

static uint8_t s_regs[16];
static bool s_write_failure;
static bool s_control_read_failure, s_start_failure, s_stop_stuck;
i2c_master_bus_handle_t reterminal_e100x_i2c_bus_get(void) { return (void *)1; }
int i2c_master_probe(void *bus, int address, int timeout) {
    assert(bus && address == 0x51 && timeout > 0); return 0;
}
int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *c, void **device) {
    assert(bus && c->device_address == 0x51); *device = (void *)2; return 0;
}
int i2c_master_bus_rm_device(void *device) { assert(device); return 0; }
int i2c_master_transmit_receive(void *device, const uint8_t *reg, size_t rn,
                               uint8_t *out, size_t n, int timeout) {
    assert(device && rn == 1 && *reg + n <= sizeof(s_regs) && timeout > 0);
    if (*reg == 0 && s_control_read_failure) return -1;
    memcpy(out, s_regs + *reg, n); return 0;
}
int i2c_master_transmit(void *device, const uint8_t *data, size_t n, int timeout) {
    assert(device && n > 1 && data[0] + n - 1 <= sizeof(s_regs) && timeout > 0);
    if (s_write_failure && data[0] == 2) return -1;
    if (data[0] == 0 && !(data[1] & 0x20)) {
        if (s_start_failure) return -1;
        if (s_stop_stuck) return 0;
    }
    memcpy(s_regs + data[0], data + 1, n - 1); return 0;
}
int main(void) {
    reterminal_e100x_rtc_time_t t = {2026,10,18,12,34,56,true}, read;
    s_regs[0] = 0x80;
    assert(reterminal_e100x_rtc_set(&t));
    assert(s_regs[0] == 0x80); // Unrelated control bits survive STOP/start.
    assert(s_regs[2] == 0x56 && s_regs[3] == 0x34 && s_regs[4] == 0x12);
    assert(s_regs[6] == 0 && s_regs[7] == 0x10 && s_regs[8] == 0x26);
    assert(reterminal_e100x_rtc_read(&read) && read.valid && read.year == 2026 && read.day == 18);
    s_regs[0] |= 0x20;
    assert(reterminal_e100x_rtc_read(&read) && read.valid && s_regs[0] == 0x80);
    s_regs[2] |= 0x80;
    s_regs[0] |= 0x20;
    assert(reterminal_e100x_rtc_read(&read) && !read.valid); // Voltage-low flag.
    assert(!(s_regs[0] & 0x20) && (s_regs[2] & 0x80));
    s_regs[0] |= 0x20;
    s_start_failure = true;
    assert(!reterminal_e100x_rtc_read(&read));
    s_start_failure = false;
    s_stop_stuck = true;
    assert(!reterminal_e100x_rtc_read(&read));
    s_stop_stuck = false;
    s_control_read_failure = true;
    assert(!reterminal_e100x_rtc_read(&read));
    s_control_read_failure = false;
    t = (reterminal_e100x_rtc_time_t){2024,2,29,0,0,0,true};
    assert(reterminal_e100x_rtc_set(&t));
    t.year = 2023; assert(!reterminal_e100x_rtc_set(&t));
    t = (reterminal_e100x_rtc_time_t){2000,1,1,0,0,0,true};
    assert(reterminal_e100x_rtc_set(&t) && s_regs[6] == 6);
    s_write_failure = true;
    assert(!reterminal_e100x_rtc_set(&t));
    assert(!(s_regs[0] & 0x20)); // Failed time write still restarts the clock.
    return 0;
}
