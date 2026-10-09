/* Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../components/muse/muse_watcher_power_test.h"
#define ESP_CODEC_DEV_OK 0
#define RTC_NOINIT_ATTR
#define ESP_RETURN_ON_ERROR(call, tag, msg) do { esp_err_t err_ = (call); if (err_) return err_; } while (0)
#define ESP_RETURN_ON_FALSE(ok, err, tag, msg) do { if (!(ok)) return (err); } while (0)
typedef void *esp_codec_dev_handle_t;
typedef void *i2s_chan_handle_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
typedef struct audio_codec_if_t {
    int (*enable)(const struct audio_codec_if_t *, bool);
} audio_codec_if_t;
static bool s_ptest_spk_open, s_ptest_mic_open, s_ptest_audio_attempted, s_ptest_audio_failed;
static bool s_ptest_audio_poisoned, s_ptest_i2s_parked, hw_active, s_ptest_i2s_stopped;
static muse_ptest_codec_t s_ptest_codec_state = MUSE_PTEST_CODEC_COLD;
static esp_codec_dev_handle_t s_ptest_spk, s_ptest_mic;
static i2s_chan_handle_t s_ptest_tx, s_ptest_rx;
static const audio_codec_if_t *s_ptest_dac_if, *s_ptest_adc_if;
static int s_ptest_volume;
static int init_error, open_error, lowlevel_error, force_error;
static unsigned init_calls, close_calls, force_calls, disable_calls;
static int codec_enable(const audio_codec_if_t *codec, bool on)
{
    (void)codec;
    if (lowlevel_error) { return lowlevel_error; }
    hw_active = on;
    return 0;
}
static const audio_codec_if_t codec = { .enable = codec_enable };
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    init_calls++;
    hw_active = true; /* ADC constructor already writes before a wrapper exists */
    s_ptest_dac_if = s_ptest_adc_if = &codec;
    s_ptest_tx = s_ptest_rx = (void *)1;
    s_ptest_i2s_stopped = false;
    if (init_error) { return init_error; }
    *spk = (void *)1;
    *mic = (void *)2;
    return 0;
}
static int esp_codec_dev_open(esp_codec_dev_handle_t handle, esp_codec_dev_sample_info_t *fs)
{
    (void)handle; (void)fs;
    hw_active = true; /* emulate wrapper's partially-open side effect */
    return open_error;
}
static int esp_codec_dev_close(esp_codec_dev_handle_t handle)
{
    (void)handle;
    close_calls++;
    return 0; /* wrapper ignores low-level disable failure */
}
static int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t h, int v) { (void)h; (void)v; return 0; }
static int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t h, float v) { (void)h; (void)v; return 0; }
static int i2s_channel_enable(i2s_chan_handle_t h) { (void)h; return 0; }
static int i2s_channel_disable(i2s_chan_handle_t h) { (void)h; disable_calls++; return 0; }
#define EXP_PWR_CODEC_PA 4096
static int ptest_set(unsigned mask, bool on) { (void)mask; (void)on; return 0; }
static int ptest_force_suspend(void)
{
    force_calls++;
    if (force_error) { return force_error; }
    hw_active = false;
    return 0;
}
static int ptest_verify_suspended(void);
static muse_ptest_codec_regs_t s_ptest_codec_regs;
static int s_ptest_adc_addr;
static void *s_i2c = (void *)1;
static bool wrong_register_values;
static int reg_transport_error = -1;
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define ES7243_ADDR 0x13
#define ES7243E_ADDR 0x14
#define ESP_LOGE(tag, ...) do {} while (0)
typedef void *i2c_master_dev_handle_t;
typedef struct { unsigned device_address, scl_speed_hz; } i2c_device_config_t;
static int i2c_master_probe(void *bus, unsigned addr, int timeout)
{
    (void)bus; (void)timeout;
    return addr == ES7243E_ADDR ? ESP_OK : ESP_FAIL;
}
static int i2c_master_bus_add_device(void *bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out)
{
    (void)bus;
    *out = (void *)(uintptr_t)cfg->device_address;
    return 0;
}
static int i2c_master_bus_rm_device(i2c_master_dev_handle_t device) { (void)device; return 0; }
static int i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *reg, size_t tx, uint8_t *out, size_t rx, int timeout)
{
    (void)timeout;
    assert(tx == 1 && rx == 1);
    if ((int)*reg == reg_transport_error) { return 263; }
    *out = 0;
    if ((uintptr_t)device == 0x18) {
        if (*reg == 0x0d) { *out = wrong_register_values ? 0x99 : hw_active ? 0xfa : 0xfc; }
        if (*reg == 0x01 && hw_active) { *out = 0x3f; }
        if (*reg == 0x0e && !hw_active) { *out = 0xff; }
        if (*reg == 0x12 && !hw_active) { *out = 2; }
    } else {
        if (*reg == 0x00) { *out = hw_active ? 0x80 : 0x1e; }
        if (*reg == 0x01 && hw_active) { *out = 0x3a; }
        if (*reg == 0x04) { *out = 1; }
        if (*reg == 0xf9 && !hw_active) { *out = 1; }
    }
    return 0;
}

typedef int gpio_num_t;
typedef int gpio_mode_t;
#define I2S_MCLK 10
#define I2S_BCLK 11
#define I2S_WS 12
#define I2S_DOUT 16
#define GPIO_NUM_40 40
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_DISABLE 0
#define BIT64(n) (UINT64_C(1) << (n))
static uint16_t s_ptest_knobs;
static uint64_t s_ptest_held_mask;
static int hold_error_nth, release_error_nth;
static unsigned hold_calls, release_calls;
static bool deep_hold;
bool muse_watcher_ptest_deep_holds_owned(void);
static int gpio_hold_en(gpio_num_t pin)
{
    (void)pin;
    assert(muse_watcher_ptest_deep_holds_owned()); /* committed BEFORE acquisition */
    hold_calls++;
    return hold_error_nth == (int)hold_calls ? ESP_FAIL : 0;
}
static int gpio_hold_dis(gpio_num_t pin)
{
    (void)pin;
    release_calls++;
    return release_error_nth == (int)release_calls ? ESP_FAIL : 0;
}
static int ptest_pin(gpio_num_t pin, gpio_mode_t mode, bool down) { (void)pin; (void)mode; (void)down; return 0; }
static void gpio_deep_sleep_hold_en(void) { deep_hold = true; }
static void gpio_deep_sleep_hold_dis(void) { deep_hold = false; }

/* Generated from the actual BSP function bodies, not a second implementation. */
#include "watcher_power_bsp_functions.inc"

int main(int argc, char **argv)
{
    if (argc != 2) { return 2; }
    int mode = atoi(argv[1]);
    int operation = 0, cleanup = 0, retry = 0;
    if (mode == 0) {
        init_error = ESP_FAIL;
        operation = ptest_audio_open();
        cleanup = ptest_audio_close();
        retry = ptest_audio_open();
        assert(operation && !cleanup && retry && !hw_active && s_ptest_audio_poisoned && force_calls);
    } else if (mode == 1) {
        open_error = ESP_FAIL;
        operation = ptest_audio_open();
        cleanup = ptest_audio_close();
        retry = ptest_audio_open();
        assert(operation && !cleanup && retry && close_calls == 2 && force_calls && !hw_active);
    } else if (mode == 2) {
        operation = ptest_audio_initialize();
        cleanup = ptest_audio_close();
        assert(!operation && !cleanup && hw_active && !force_calls && s_ptest_codec_state == MUSE_PTEST_CODEC_INITIALIZED);
    } else if (mode == 3) {
        s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
        hw_active = true; /* warm external hardware, no recreated wrappers */
        cleanup = ptest_audio_close();
        retry = ptest_audio_open();
        assert(!cleanup && !retry && force_calls == 1 && init_calls == 1);
    } else if (mode == 4) {
        operation = ptest_audio_open();
        lowlevel_error = ESP_FAIL;
        cleanup = ptest_audio_close();
        retry = ptest_audio_open();
        assert(!operation && cleanup && retry && s_ptest_audio_poisoned && force_calls && !hw_active);
    } else if (mode == 5) {
        s_ptest_i2s_parked = true;
        operation = ptest_audio_open();
        assert(operation == ESP_ERR_INVALID_STATE && !init_calls && !s_ptest_audio_attempted);
    } else if (mode == 12) {
        assert(!ptest_audio_initialize());
        assert(!ptest_audio_close());
        assert(s_ptest_i2s_stopped && disable_calls == 2);
        open_error = 99;
        assert(ptest_audio_open() != ESP_OK); /* data enables before wrapper failure */
        assert(!s_ptest_i2s_stopped);
        assert(!ptest_audio_close());
        assert(disable_calls == 4 && s_ptest_i2s_stopped);
    } else if (mode == 11) {
        assert(!ptest_audio_initialize());
        assert(!ptest_audio_close());
        assert(disable_calls == 2);
        assert(!ptest_audio_close());
        assert(disable_calls == 2); /* never disable stopped channels again */
        assert(s_ptest_codec_state == MUSE_PTEST_CODEC_INITIALIZED);
    } else if (mode == 9 || mode == 10) {
        s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
        hw_active = false;
        if (mode == 9) { wrong_register_values = true; }
        else { reg_transport_error = 0x0d; }
        operation = ptest_verify_suspended();
        assert(!s_ptest_codec_regs.expected);
        if (mode == 9) { assert(!operation && s_ptest_codec_regs.dac[3] == 0x99); }
        else { assert(operation == 263 && !(s_ptest_codec_regs.dac_mask & 8)); }
    } else if (mode == 8) {
        operation = ptest_audio_initialize();
        assert(!operation && muse_watcher_ptest_warm_seen());
        for (int reset = 0; reset < 2; reset++) {
            /* reset inside constructor or after held-pad marker is cleared */
            s_ptest_spk = s_ptest_mic = s_ptest_tx = s_ptest_rx = NULL;
            s_ptest_dac_if = s_ptest_adc_if = NULL;
            s_ptest_spk_open = s_ptest_mic_open = s_ptest_audio_attempted = s_ptest_audio_failed = s_ptest_audio_poisoned = false;
            assert(muse_watcher_ptest_warm_seen());
            s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
            cleanup = ptest_audio_close();
            assert(!cleanup && !hw_active && muse_watcher_ptest_warm_seen());
        }
        assert(force_calls == 2);
    } else if (mode >= 6) {
        s_ptest_knobs = MUSE_PTEST_I2S_LOW | MUSE_PTEST_RGB_LOW;
        if (mode == 6) { hold_error_nth = 1; }
        operation = muse_watcher_ptest_prepare_deep(true);
        assert(muse_watcher_ptest_deep_holds_owned());
        s_ptest_held_mask = 0; /* reset/torn large journal: only tiny ownership survives */
        if (mode == 7) { release_error_nth = 3; }
        cleanup = muse_watcher_ptest_release_deep_holds();
        if (mode == 7) {
            assert(cleanup && muse_watcher_ptest_deep_holds_owned());
            release_error_nth = 0;
            cleanup = muse_watcher_ptest_release_deep_holds();
        }
        assert(!cleanup && !muse_watcher_ptest_deep_holds_owned() && muse_watcher_ptest_warm_seen() && !deep_hold && release_calls >= 5);
    }
    printf("{\"operation\":%d,\"cleanup\":%d,\"retry\":%d,\"close_calls\":%u,\"force_calls\":%u}\n", operation, cleanup, retry, close_calls, force_calls);
    return 0;
}
