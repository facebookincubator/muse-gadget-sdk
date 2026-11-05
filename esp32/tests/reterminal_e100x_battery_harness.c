#include <assert.h>
#include "battery_adc.h"
#include "reterminal_e100x_board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"

static int s_samples[10], s_reads, s_power, s_wait_ms, s_read_failure, s_cali_failure;
int gpio_config(const gpio_config_t *c) { assert(c->pin_bit_mask == (1ULL << E100X_BATTERY_ENABLE_GPIO)); return 0; }
int gpio_set_level(int gpio, int level) { assert(gpio == E100X_BATTERY_ENABLE_GPIO); s_power = level; return 0; }
void vTaskDelay(int ms) { assert(ms == 5 || ms == 25); s_wait_ms += ms; }
int adc_oneshot_io_to_channel(int gpio, int *unit, int *channel) {
    assert(gpio == 1); *unit = 1; *channel = 0; return 0;
}
int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, void **out) {
    assert(c->unit_id == 1); *out = (void *)1; return 0;
}
int adc_oneshot_del_unit(void *unit) { assert(unit); return 0; }
int adc_oneshot_config_channel(void *u, int channel, const adc_oneshot_chan_cfg_t *c) {
    assert(u && channel == 0 && c->atten == 12); return 0;
}
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, void **out) {
    assert(c->unit_id == 1 && c->chan == 0); *out = (void *)2; return 0;
}
int adc_cali_raw_to_voltage(void *cali, int raw, int *mv) {
    assert(cali);
    if (s_cali_failure) return -1;
    *mv = raw; return 0;
}
int adc_oneshot_read(void *u, int channel, int *raw) {
    assert(u && channel == 0 && s_power && s_reads < 10);
    if (s_read_failure) return -1;
    *raw = s_samples[s_reads++]; return 0;
}
static void samples(int mv) {
    s_reads = 0; s_wait_ms = 0;
    for (int i = 0; i < 10; i++) s_samples[i] = mv;
}
int main(void) {
    const reterminal_e100x_board_config_t *cfg = reterminal_e100x_board();
    assert(cfg->model == RETERMINAL_E100X_BOARD_ID);
    assert(cfg->keys[0] == 3 && cfg->keys[1] == 4 && cfg->keys[2] == 5);
    assert(cfg->width == (cfg->model == 3 ? 1872 : 800));
    assert(cfg->height == (cfg->model == 3 ? 1404 : 480));
    assert(cfg->sd_enable_gpio == (cfg->model == 3 ? 39 : 16));
    assert(cfg->battery_enable_gpio == (cfg->model == 3 ? 40 : 21));
    assert(cfg->battery_curve_mv[50] == (cfg->model == 3 ? 3849 : 3642));
    int mv = -1, pct = -1;
    samples(1900);
    assert(battery_adc_read(&mv, &pct));
    assert(mv == 3800 && pct == (cfg->model == 3 ? 43 : 76) && s_reads == 10 && s_power == E100X_BATTERY_KEEP_ENABLED);
    assert(s_wait_ms == 5 + 9 * 25);
    #if RETERMINAL_E100X_BOARD_ID != 3
    assert(reterminal_e100x_battery_percent(2795) == 0 && reterminal_e100x_battery_percent(4111) == 100);
    assert(reterminal_e100x_battery_percent(3642) == 50);
    #endif
    assert(reterminal_e100x_battery_percent(0) == 0 && reterminal_e100x_battery_percent(5000) == 100);
    samples(1821); s_samples[0] = 1812; s_samples[1] = 1830;
    assert(battery_adc_read(&mv, &pct) && mv == 3642 && pct == reterminal_e100x_battery_percent(3642));
    samples(1900); s_samples[0] = 2101; s_samples[1] = 1700;
    mv = -1; pct = -1;
    assert(!battery_adc_read(&mv, &pct) && s_power == E100X_BATTERY_KEEP_ENABLED && mv == -1 && pct == -1);
    samples(499);
    assert(!battery_adc_read(&mv, &pct) && s_power == E100X_BATTERY_KEEP_ENABLED);
    samples(1900); s_read_failure = 1;
    assert(!battery_adc_read(&mv, &pct) && s_power == E100X_BATTERY_KEEP_ENABLED);
    s_read_failure = 0; s_cali_failure = 1;
    assert(!battery_adc_read(&mv, &pct) && s_power == E100X_BATTERY_KEEP_ENABLED);
    return 0;
}
