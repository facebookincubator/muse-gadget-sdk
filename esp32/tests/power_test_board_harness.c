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
/* Host harness for the SenseCAP Watcher power-test board hooks: diagnostic
 * pin knobs (park, read back, restore), expander ownership, state apply
 * ordering and guards, readback checks, per-tick service and the vendor codec
 * shutdown sequence. The function bodies come verbatim from
 * board_sensecap_watcher.c (power_test_board_functions.inc); this file only
 * models the ESP-IDF drivers they call. Panel, LCD, ADC-driver and audio
 * bring-up are stubbed: they are hardware sequences the fakes cannot model. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../components/muse/muse_power_test.h"
#include "../components/muse/boards/muse_power_test_board.h"

#define TAG "board"
#define RTC_NOINIT_ATTR
#define ESP_RETURN_ON_ERROR(call, tag, msg) do { esp_err_t err_ = (call); if (err_) return err_; } while (0)
#define ESP_RETURN_ON_FALSE(ok, err, tag, msg) do { if (!(ok)) return (err); } while (0)
#define ESP_LOGE(tag, ...) do {} while (0)
#define BIT(n) (1u << (n))
#define BIT64(n) (UINT64_C(1) << (n))
#define pdMS_TO_TICKS(ms) (ms)
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define UART_PIN_NO_CHANGE (-1)
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define SIG_GPIO_OUT_IDX 256
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0

/* ---- GPIO model ---------------------------------------------------------- */
typedef int gpio_num_t;
typedef int gpio_mode_t;
#define NPINS 49
enum { GPIO_NUM_1 = 1, GPIO_NUM_2 = 2, GPIO_NUM_4 = 4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_8, GPIO_NUM_9,
       GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15, GPIO_NUM_16, GPIO_NUM_17,
       GPIO_NUM_18, GPIO_NUM_21 = 21, GPIO_NUM_38 = 38, GPIO_NUM_39, GPIO_NUM_40, GPIO_NUM_41, GPIO_NUM_42,
       GPIO_NUM_43, GPIO_NUM_44, GPIO_NUM_45, GPIO_NUM_46, GPIO_NUM_47, GPIO_NUM_48 };
#define GPIO_MODE_DISABLE 0
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_MODE_DEF_OD 4
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define GPIO_PULLDOWN_ONLY 7
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; int pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { bool pu, pd, ie, oe, od, slp_sel; int drv; uint32_t fun_sel, sig_out; } gpio_io_config_t;
static struct { uint32_t out; struct { uint32_t val; } out1; struct { uint32_t val; } func_out_sel_cfg[NPINS]; } GPIO;
static gpio_io_config_t pins[NPINS];
static bool hold[NPINS];
static int sleep_dir[NPINS], sleep_pull[NPINS];
static const int GPIO_PIN_MUX_REG[NPINS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48 };
#define PIN_FUNC_SELECT(reg, fun) (pins[(reg)].fun_sel = (fun))
static unsigned gpio_writes, isolate_calls;
static bool sleep_switch, deep_hold_all;

/* One injectable failure: the Nth call of a named fake returns `fail_error`. */
static const char *fail_name;
static unsigned fail_nth, fail_seen;
static esp_err_t fail_error = ESP_FAIL;
static esp_err_t fault(const char *name)
{
    if (fail_name && !strcmp(fail_name, name) && ++fail_seen == fail_nth) { return fail_error; }
    return ESP_OK;
}
static void inject(const char *name, unsigned nth, esp_err_t error)
{
    fail_name = name; fail_nth = nth; fail_seen = 0; fail_error = error;
}

static int latch(gpio_num_t pin) { return pin < 32 ? (GPIO.out >> pin) & 1 : (GPIO.out1.val >> (pin - 32)) & 1; }
static esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    ESP_RETURN_ON_ERROR(fault("gpio_set_level"), TAG, "");
    gpio_writes++;
    if (pin < 32) { GPIO.out = level ? GPIO.out | BIT(pin) : GPIO.out & ~BIT(pin); }
    else { GPIO.out1.val = level ? GPIO.out1.val | BIT(pin - 32) : GPIO.out1.val & ~BIT(pin - 32); }
    return ESP_OK;
}
static esp_err_t gpio_config(const gpio_config_t *cfg)
{
    ESP_RETURN_ON_ERROR(fault("gpio_config"), TAG, "");
    gpio_writes++;
    for (int pin = 0; pin < NPINS; pin++) {
        if (!(cfg->pin_bit_mask & BIT64(pin))) { continue; }
        pins[pin].ie = cfg->mode & GPIO_MODE_INPUT;
        pins[pin].oe = cfg->mode & GPIO_MODE_OUTPUT;
        pins[pin].od = cfg->mode & GPIO_MODE_DEF_OD;
        pins[pin].pu = cfg->pull_up_en;
        pins[pin].pd = cfg->pull_down_en;
        pins[pin].fun_sel = 1; /* PIN_FUNC_GPIO */
        GPIO.func_out_sel_cfg[pin].val = SIG_GPIO_OUT_IDX; /* gpio_config disconnects the matrix */
    }
    return ESP_OK;
}
static esp_err_t gpio_get_io_config(gpio_num_t pin, gpio_io_config_t *out)
{
    ESP_RETURN_ON_ERROR(fault("gpio_get_io_config"), TAG, "");
    *out = pins[pin];
    out->sig_out = GPIO.func_out_sel_cfg[pin].val & 0x1ff;
    return ESP_OK;
}
static esp_err_t gpio_set_drive_capability(gpio_num_t pin, int drv) { gpio_writes++; pins[pin].drv = drv; return ESP_OK; }
static esp_err_t gpio_sleep_sel_en(gpio_num_t pin) { pins[pin].slp_sel = true; return ESP_OK; }
static esp_err_t gpio_sleep_sel_dis(gpio_num_t pin) { pins[pin].slp_sel = false; return ESP_OK; }
static esp_err_t gpio_pullup_dis(gpio_num_t pin) { gpio_writes++; pins[pin].pu = false; return ESP_OK; }
static esp_err_t gpio_pulldown_dis(gpio_num_t pin) { gpio_writes++; pins[pin].pd = false; return ESP_OK; }
static bool rtc_gpio_is_valid_gpio(gpio_num_t pin) { return pin <= 21; }
static esp_err_t rtc_gpio_pullup_en(gpio_num_t pin) { pins[pin].pu = true; return ESP_OK; }
static esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin) { pins[pin].pu = false; return ESP_OK; }
static esp_err_t rtc_gpio_pulldown_en(gpio_num_t pin) { pins[pin].pd = true; return ESP_OK; }
static esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) { pins[pin].pd = false; return ESP_OK; }
static esp_err_t gpio_sleep_set_direction(gpio_num_t pin, int mode) { sleep_dir[pin] = mode; return ESP_OK; }
static esp_err_t gpio_sleep_set_pull_mode(gpio_num_t pin, int pull) { sleep_pull[pin] = pull; return ESP_OK; }
static void esp_sleep_config_gpio_isolate(void) { isolate_calls++; }
static void esp_sleep_enable_gpio_switch(bool on)
{
    sleep_switch = on; /* IDF sets or clears SLP_SEL on every pad */
    for (int pin = 0; pin < NPINS; pin++) { pins[pin].slp_sel = on; }
}
static esp_err_t gpio_hold_en(gpio_num_t pin) { ESP_RETURN_ON_ERROR(fault("gpio_hold_en"), TAG, ""); hold[pin] = true; return ESP_OK; }
static esp_err_t gpio_hold_dis(gpio_num_t pin) { ESP_RETURN_ON_ERROR(fault("gpio_hold_dis"), TAG, ""); hold[pin] = false; return ESP_OK; }
static void gpio_deep_sleep_hold_en(void) { deep_hold_all = true; }
static void gpio_deep_sleep_hold_dis(void) { deep_hold_all = false; }

/* ---- UART -------------------------------------------------------------- */
static unsigned uart_set_pin_calls;
static esp_err_t uart_wait_tx_done(int port, int ticks) { (void)port; (void)ticks; return fault("uart_wait_tx_done"); }
static esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{
    (void)port; (void)rts; (void)cts;
    ESP_RETURN_ON_ERROR(fault("uart_set_pin"), TAG, "");
    uart_set_pin_calls++;
    pins[tx].oe = true; pins[tx].ie = false; pins[rx].ie = true; pins[rx].oe = false; pins[rx].pu = true;
    GPIO.func_out_sel_cfg[tx].val = 12; /* U0TXD */
    return ESP_OK;
}

/* ---- I2C: PCA9535 expander at 0x21 and the codecs ----------------------- */
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { unsigned device_address, scl_speed_hz; } i2c_device_config_t;
static uint16_t exp_regs[8], exp_external; /* external input levels */
static uint8_t dac_regs[256], adc_regs[256];
static unsigned adc_addr = 0x14;
static unsigned codec_writes[2], codec_bad_reg = 0x100;
static bool dac_late_error; /* a second, different write error later in the sequence */
static char log_buf[2048];
static void log_event(const char *text)
{
    size_t n = strlen(log_buf);
    snprintf(log_buf + n, sizeof(log_buf) - n, "%s%s", n ? "," : "", text);
}
static esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t n, int timeout)
{
    (void)timeout;
    unsigned addr = (unsigned)(uintptr_t)dev;
    if (addr == 0x21) {
        ESP_RETURN_ON_ERROR(fault("exp_write"), TAG, "");
        assert(n == 3);
        exp_regs[buf[0]] = buf[1] | buf[2] << 8;
        char text[32];
        snprintf(text, sizeof(text), "exp%02x=%04x", buf[0], exp_regs[buf[0]]);
        log_event(text);
        return ESP_OK;
    }
    assert(n == 2);
    bool dac = addr == (ES8311_CODEC_DEFAULT_ADDR >> 1);
    codec_writes[dac ? 0 : 1]++;
    esp_err_t e = fault(dac ? "dac_write" : "adc_write");
    if (!e && dac && dac_late_error && buf[0] == 0x45) { e = ESP_ERR_INVALID_STATE; }
    if (!e) { (dac ? dac_regs : adc_regs)[buf[0]] = buf[1]; }
    return e;
}
static esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *reg, size_t tx,
                                             uint8_t *out, size_t rx, int timeout)
{
    (void)timeout;
    assert(tx == 1);
    unsigned addr = (unsigned)(uintptr_t)dev;
    if (addr == 0x21) {
        ESP_RETURN_ON_ERROR(fault("exp_read"), TAG, "");
        assert(rx == 2);
        uint16_t v = exp_regs[*reg];
        /* The input port reads the pin: outputs reflect their own latch. */
        if (*reg == 0x00) { v = (exp_external & 0x20ff) | (exp_regs[2] & ~0x20ff); }
        out[0] = v & 0xff; out[1] = v >> 8;
        return ESP_OK;
    }
    assert(rx == 1);
    if (*reg == codec_bad_reg) { return 263; }
    *out = (addr == (ES8311_CODEC_DEFAULT_ADDR >> 1) ? dac_regs : adc_regs)[*reg];
    return ESP_OK;
}
static esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out)
{
    (void)bus;
    *out = (void *)(uintptr_t)cfg->device_address;
    return ESP_OK;
}
static esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { (void)dev; return ESP_OK; }
static esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, unsigned addr, int timeout)
{
    (void)bus; (void)timeout;
    return addr == adc_addr ? ESP_OK : ESP_FAIL;
}

/* ---- Other drivers ------------------------------------------------------ */
typedef void *esp_codec_dev_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *i2s_chan_handle_t;
typedef struct { int battery_mv; } muse_power_t;
static int ledc_duty;
static unsigned ledc_get_duty(int mode, int channel) { (void)mode; (void)channel; return (unsigned)ledc_duty; }
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static int battery_mv = 3900;
static unsigned power_reads;
static esp_err_t read_power(muse_power_t *p) { ESP_RETURN_ON_ERROR(fault("read_power"), TAG, ""); power_reads++; p->battery_mv = battery_mv; return ESP_OK; }
static int16_t i2s_buf[320];
static size_t i2s_short;
static esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *buf, size_t n, size_t *done, int timeout)
{
    (void)h; (void)timeout;
    ESP_RETURN_ON_ERROR(fault("i2s_read"), TAG, "");
    memset(buf, 0, n);
    *done = n - i2s_short;
    return ESP_OK;
}
static esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *buf, size_t n, size_t *done, int timeout)
{
    (void)h; (void)timeout;
    ESP_RETURN_ON_ERROR(fault("i2s_write"), TAG, "");
    assert(n == sizeof(i2s_buf));
    memcpy(i2s_buf, buf, n);
    *done = n - i2s_short;
    return ESP_OK;
}
static void vTaskDelay(int ticks) { char text[24]; snprintf(text, sizeof(text), "delay%d", ticks); log_event(text); }

/* State the BSP defines before its power-test section. */
static i2c_master_bus_handle_t s_i2c = (void *)1;
static i2c_master_dev_handle_t s_exp;
static uint16_t s_exp_out;
static i2s_chan_handle_t s_ptest_tx = (void *)1, s_ptest_rx = (void *)2;
static int s_ptest_adc_addr;
static esp_err_t init(void) { s_exp = (void *)(uintptr_t)0x21; return ESP_OK; }

/* Stubbed hardware bring-up: logged so apply ordering can be checked. */
static esp_err_t ptest_lcd(int percent);
static esp_err_t ptest_lcd_off(void);
static esp_err_t ptest_adc_init(void);
static esp_err_t ptest_audio_initialize(void);
static esp_err_t ptest_audio_open(void);
static esp_err_t ptest_audio_close(void);

/* The extract carries the board's power-test statics whole; the panel, ADC
 * and audio bring-up that use some of them are left out. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "power_test_board_functions.inc"
#pragma GCC diagnostic pop

static esp_err_t ptest_lcd(int percent) { char t[16]; snprintf(t, sizeof(t), "lcd%d", percent); log_event(t); return fault("lcd"); }
static esp_err_t ptest_lcd_off(void) { log_event("lcd_off"); return fault("lcd"); }
static esp_err_t ptest_adc_init(void) { log_event("adc_init"); return fault("adc_init"); }
static esp_err_t ptest_audio_initialize(void)
{
    log_event("audio_init");
    s_ptest_audio_attempted = true;
    if (s_ptest_codec_state == MUSE_PTEST_CODEC_COLD) { s_ptest_codec_state = MUSE_PTEST_CODEC_INITIALIZED; }
    return fault("audio");
}
static esp_err_t ptest_audio_open(void)
{
    log_event("audio_open");
    s_ptest_audio_attempted = true;
    s_ptest_spk_open = s_ptest_mic_open = true;
    if (s_ptest_codec_state == MUSE_PTEST_CODEC_COLD) { s_ptest_codec_state = MUSE_PTEST_CODEC_INITIALIZED; }
    return fault("audio");
}
static esp_err_t ptest_audio_close(void)
{
    log_event("audio_close");
    if (s_ptest_spk_open) { s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED; }
    s_ptest_spk_open = s_ptest_mic_open = false;
    return fault("audio");
}

/* ---- Scenarios ---------------------------------------------------------- */
static gpio_io_config_t boot_pins[NPINS];
static uint32_t boot_matrix[NPINS], boot_out, boot_out1;

static void boot(void)
{
    /* Boot-like pad state: I2S routed to the I2S peripheral, pulled-up I2C
     * and knob inputs, the UART on its matrix signals, unused pads idle. */
    for (int pin = 0; pin < NPINS; pin++) {
        pins[pin] = (gpio_io_config_t){ .ie = true, .drv = 2, .fun_sel = 1 };
        GPIO.func_out_sel_cfg[pin].val = SIG_GPIO_OUT_IDX;
    }
    const int i2s[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT };
    for (int i = 0; i < 4; i++) {
        pins[i2s[i]] = (gpio_io_config_t){ .oe = true, .drv = 3, .fun_sel = 1 };
        GPIO.func_out_sel_cfg[i2s[i]].val = 23 + i; /* I2S0 output signals */
    }
    pins[I2S_DIN] = (gpio_io_config_t){ .ie = true, .drv = 2, .fun_sel = 1 };
    const int pulled[] = { I2C_SDA, I2C_SCL, EXP_INT, KNOB_A, KNOB_B };
    for (int i = 0; i < 5; i++) { pins[pulled[i]].pu = true; pins[pulled[i]].od = i < 2; }
    pins[I2C_SDA].oe = pins[I2C_SCL].oe = true;
    pins[43] = (gpio_io_config_t){ .oe = true, .drv = 2 };
    pins[44] = (gpio_io_config_t){ .ie = true, .pu = true, .drv = 2 };
    GPIO.func_out_sel_cfg[43].val = 12;
    GPIO.out = BIT(I2S_WS);
    GPIO.out1.val = BIT(40 - 32) | BIT(I2C_SDA - 32);
    memcpy(boot_pins, pins, sizeof(pins));
    for (int pin = 0; pin < NPINS; pin++) { boot_matrix[pin] = GPIO.func_out_sel_cfg[pin].val; }
    boot_out = GPIO.out; boot_out1 = GPIO.out1.val;
    exp_external = EXP_VBUS | EXP_STDBY; /* VBUS high: USB absent */
    assert(!muse_ptest_board_init());
    exp_regs[6] = EXP_INPUTS;
    assert(!ptest_set(EXP_PWR_SYSTEM, true));
    adc_addr = ES7243E_ADDR;
    log_buf[0] = 0;
}
static bool restored(const gpio_num_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        gpio_num_t p = list[i];
        gpio_io_config_t now = pins[p], was = boot_pins[p];
        if (now.pu != was.pu || now.pd != was.pd || now.ie != was.ie || now.oe != was.oe || now.od != was.od ||
            now.drv != was.drv || now.fun_sel != was.fun_sel || now.slp_sel != was.slp_sel ||
            GPIO.func_out_sel_cfg[p].val != boot_matrix[p] ||
            latch(p) != (int)(p < 32 ? (boot_out >> p) & 1 : (boot_out1 >> (p - 32)) & 1)) {
            fprintf(stderr, "pin %d not restored\n", p);
            return false;
        }
    }
    return true;
}
static const gpio_num_t I2S5[] = { I2S_MCLK, I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN };
static const gpio_num_t PULL5[] = { I2C_SDA, I2C_SCL, EXP_INT, KNOB_A, KNOB_B };
static const gpio_num_t UNUSED7[] = { CAM_SCLK, CAM_MOSI, CAM_MISO, CAM_CS, GPIO_NUM_46, GPIO_NUM_17, GPIO_NUM_18 };
static bool hiz(gpio_num_t p) { return !pins[p].ie && !pins[p].oe && !pins[p].pu && !pins[p].pd; }
static muse_ptest_state_t state(muse_ptest_load_t load, int backlight, bool amp, uint16_t knobs, muse_ptest_codec_t codec)
{
    return (muse_ptest_state_t){ "s", "", load, 0, backlight, amp, knobs, 1000, codec, "other", "" };
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void scenario_all_knobs_round_trip(void)
{
    const uint16_t knobs = MUSE_PTEST_I2S_LOW | MUSE_PTEST_UART_HIZ | MUSE_PTEST_RGB_LOW | MUSE_PTEST_PULLS_OFF |
                           MUSE_PTEST_UNUSED_HIZ | MUSE_PTEST_GPIO_ISOLATE;
    CHECK(ptest_apply_knobs(knobs) == ESP_OK);
    CHECK(s_ptest_knobs == knobs && s_ptest_uart_parked && s_ptest_i2s_parked);
    for (int i = 0; i < 4; i++) {
        gpio_num_t p = I2S5[i];
        CHECK(pins[p].oe && !pins[p].ie && !pins[p].pu && !pins[p].pd && !latch(p));
        CHECK(GPIO.func_out_sel_cfg[p].val == SIG_GPIO_OUT_IDX && !pins[p].slp_sel); /* low through sleep */
    }
    CHECK(pins[I2S_DIN].ie && !pins[I2S_DIN].oe && pins[I2S_DIN].pd);
    CHECK(sleep_dir[I2S_DIN] == GPIO_MODE_INPUT && sleep_pull[I2S_DIN] == GPIO_PULLDOWN_ONLY);
    CHECK(hiz(43) && hiz(44));
    CHECK(pins[40].oe && !latch(40) && !pins[40].slp_sel);
    for (int i = 0; i < 3; i++) { CHECK(!pins[PULL5[i]].pu && !pins[PULL5[i]].pd); }
    CHECK(pins[I2C_SDA].ie); /* I2C keeps working; only its pulls are gone */
    CHECK(hiz(KNOB_A) && hiz(KNOB_B));
    for (int i = 0; i < 7; i++) { CHECK(hiz(UNUSED7[i])); }
    CHECK(sleep_switch && isolate_calls == 1);
    const gpio_num_t lcd[] = { TP_SDA, TP_SCL, LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CS, LCD_BL };
    for (int i = 0; i < 9; i++) { CHECK(!pins[lcd[i]].slp_sel); }
    CHECK(pins[EXP_INT].slp_sel && pins[3].slp_sel); /* the rest, battery ADC GPIO3 included, isolated */
    CHECK(ptest_check_knobs() == ESP_OK);

    CHECK(ptest_revert_knobs() == ESP_OK);
    CHECK(s_ptest_knobs == 0 && !s_ptest_uart_parked && !s_ptest_i2s_parked && !sleep_switch);
    CHECK(uart_set_pin_calls == 1 && pins[43].oe && GPIO.func_out_sel_cfg[43].val == 12);
    CHECK(restored(I2S5, 5) && restored(PULL5, 5) && restored(UNUSED7, 7));
    CHECK(hiz(40)); /* RGB DIN released to high-Z, not left driven */
    CHECK(ptest_check_knobs() == ESP_OK);
}

static void scenario_i2s_hiz_round_trip(void)
{
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_HIZ) == ESP_OK);
    for (int i = 0; i < 5; i++) { CHECK(hiz(I2S5[i])); }
    CHECK(ptest_check_knobs() == ESP_OK);
    CHECK(ptest_revert_knobs() == ESP_OK && restored(I2S5, 5));
}

static void scenario_rejected_knobs_touch_nothing(void)
{
    unsigned writes = gpio_writes;
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW | MUSE_PTEST_I2S_HIZ) == ESP_ERR_INVALID_ARG);
    s_ptest_spk_open = true;
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW) == ESP_ERR_INVALID_STATE); /* never park open codecs */
    s_ptest_spk_open = false;
    exp_external &= ~EXP_VBUS; /* USB present */
    CHECK(ptest_apply_knobs(MUSE_PTEST_UART_HIZ) == ESP_ERR_INVALID_STATE);
    exp_external |= EXP_VBUS;
    s_exp_out |= BIT(8); /* SD rail on */
    CHECK(ptest_apply_knobs(MUSE_PTEST_UNUSED_HIZ) == ESP_ERR_INVALID_STATE);
    s_exp_out = (s_exp_out & ~BIT(8)) | EXP_PWR_AI;
    CHECK(ptest_apply_knobs(MUSE_PTEST_UNUSED_HIZ) == ESP_ERR_INVALID_STATE);
    s_exp_out &= ~EXP_PWR_AI;
    CHECK(gpio_writes == writes && s_ptest_knobs == 0 && !s_ptest_uart_parked && !s_ptest_i2s_parked);
    CHECK(restored(I2S5, 5) && restored(UNUSED7, 7));
}

static void scenario_uart_parking(void)
{
    /* A TX FIFO that never drains leaves both pads connected. */
    inject("uart_wait_tx_done", 1, ESP_ERR_TIMEOUT);
    CHECK(ptest_apply_knobs(MUSE_PTEST_UART_HIZ) == ESP_ERR_TIMEOUT);
    CHECK(!s_ptest_uart_parked && pins[43].oe && pins[44].ie && muse_ptest_board_uart_restore() == ESP_OK);
    CHECK(uart_set_pin_calls == 0);
    /* The USB dry run may park with USB present; restore is idempotent. */
    exp_external &= ~EXP_VBUS;
    muse_ptest_board_usb_dryrun(true);
    CHECK(ptest_apply_knobs(MUSE_PTEST_UART_HIZ) == ESP_OK && s_ptest_uart_parked && hiz(43) && hiz(44));
    CHECK(muse_ptest_board_uart_restore() == ESP_OK && !s_ptest_uart_parked && !(s_ptest_knobs & MUSE_PTEST_UART_HIZ));
    CHECK(muse_ptest_board_uart_restore() == ESP_OK && uart_set_pin_calls == 1);
    muse_ptest_board_usb_dryrun(false);
    /* A failed re-route keeps the parked flag so the next sample retries. */
    exp_external |= EXP_VBUS;
    CHECK(ptest_apply_knobs(MUSE_PTEST_UART_HIZ) == ESP_OK);
    inject("uart_set_pin", 1, ESP_FAIL);
    CHECK(muse_ptest_board_uart_restore() == ESP_FAIL && s_ptest_uart_parked);
    CHECK(muse_ptest_board_uart_restore() == ESP_OK && !s_ptest_uart_parked);
}

static void scenario_readback_detects_drift(void)
{
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW) == ESP_OK && ptest_check_knobs() == ESP_OK);
    pins[I2S_BCLK].pu = true;
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[I2S_BCLK].pu = false;
    GPIO.func_out_sel_cfg[I2S_WS].val = 25; /* re-routed to the peripheral */
    CHECK(ptest_check_knobs() == ESP_FAIL);
    GPIO.func_out_sel_cfg[I2S_WS].val = SIG_GPIO_OUT_IDX;
    gpio_set_level(I2S_MCLK, 1);
    CHECK(ptest_check_knobs() == ESP_FAIL);
    gpio_set_level(I2S_MCLK, 0);
    pins[I2S_DIN].pd = false;
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[I2S_DIN].pd = true;
    CHECK(ptest_check_knobs() == ESP_OK);
    CHECK(ptest_revert_knobs() == ESP_OK);

    CHECK(ptest_apply_knobs(MUSE_PTEST_RGB_LOW | MUSE_PTEST_PULLS_OFF | MUSE_PTEST_UNUSED_HIZ) == ESP_OK);
    gpio_set_level(40, 1);
    CHECK(ptest_check_knobs() == ESP_FAIL);
    gpio_set_level(40, 0);
    pins[KNOB_A].ie = true; /* knob wake input re-enabled */
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[KNOB_A].ie = false;
    pins[EXP_INT].pu = true;
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[EXP_INT].pu = false;
    s_exp_out |= BIT(8);
    CHECK(ptest_check_knobs() == ESP_FAIL); /* SD rail came on under parked pads */
    s_exp_out &= ~BIT(8);
    pins[CAM_CS].oe = true;
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[CAM_CS].oe = false;
    pins[EXP_INT].slp_sel = true; /* isolation without the knob */
    CHECK(ptest_check_knobs() == ESP_FAIL);
    pins[EXP_INT].slp_sel = false;
    CHECK(ptest_check_knobs() == ESP_OK);
    CHECK(ptest_revert_knobs() == ESP_OK);

    CHECK(ptest_apply_knobs(MUSE_PTEST_UART_HIZ) == ESP_OK);
    pins[44].pu = true;
    CHECK(ptest_check_knobs() == ESP_FAIL);
    CHECK(ptest_revert_knobs() == ESP_OK && restored(PULL5, 5) && restored(UNUSED7, 7));
}

static void scenario_failed_snapshot_and_restore(void)
{
    /* A failed pin snapshot reports the error before the pin is changed. */
    inject("gpio_get_io_config", 3, ESP_FAIL);
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW) == ESP_FAIL);
    CHECK(s_ptest_i2s_saved == 2 && !(s_ptest_knobs & MUSE_PTEST_I2S_LOW) && restored(I2S5, 5));
    CHECK(ptest_revert_knobs() == ESP_OK && restored(I2S5, 5));
    /* A failed restore keeps the remaining saved pins for the next attempt. */
    CHECK(ptest_apply_knobs(MUSE_PTEST_PULLS_OFF) == ESP_OK);
    inject("gpio_config", 1, ESP_FAIL);
    CHECK(ptest_revert_knobs() == ESP_FAIL && s_ptest_pull_saved == 5);
    CHECK(ptest_revert_knobs() == ESP_OK && s_ptest_pull_saved == 0 && restored(PULL5, 5));
}

static void scenario_revert_releases_owned_holds(void)
{
    CHECK(muse_ptest_board_prepare_deep(true) == ESP_ERR_INVALID_STATE && !muse_ptest_board_deep_holds_owned());
    CHECK(muse_ptest_board_prepare_deep(false) == ESP_OK && !deep_hold_all);
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW | MUSE_PTEST_RGB_LOW) == ESP_OK);
    CHECK(muse_ptest_board_prepare_deep(true) == ESP_OK);
    CHECK(deep_hold_all && hold[I2S_MCLK] && hold[I2S_DOUT] && hold[40] && !hold[I2S_DIN]);
    CHECK(muse_ptest_board_deep_holds_owned() && muse_ptest_board_warm_seen());
    CHECK(s_ptest_knobs & MUSE_PTEST_GPIO_HOLD);
    CHECK(ptest_revert_knobs() == ESP_OK);
    CHECK(!deep_hold_all && !hold[I2S_MCLK] && !hold[40] && !muse_ptest_board_deep_holds_owned() && s_ptest_knobs == 0);
    CHECK(muse_ptest_board_warm_seen() && hiz(40) && restored(I2S5, 5));
    /* After a reset only the RTC ownership marker survives: still released. */
    CHECK(ptest_apply_knobs(MUSE_PTEST_I2S_LOW | MUSE_PTEST_RGB_LOW) == ESP_OK);
    CHECK(muse_ptest_board_prepare_deep(true) == ESP_OK);
    s_ptest_held_mask = 0;
    CHECK(ptest_revert_knobs() == ESP_OK && !hold[I2S_WS] && !muse_ptest_board_deep_holds_owned());
}

static void scenario_expander_ownership_and_power(void)
{
    uint16_t before = s_exp_out;
    CHECK(ptest_set(EXP_PWR_AI, true) == ESP_ERR_INVALID_ARG);
    CHECK(ptest_set(BIT(8), true) == ESP_ERR_INVALID_ARG); /* SD rail is never switched */
    CHECK(s_exp_out == before && exp_regs[2] == before);
    inject("exp_write", 1, ESP_FAIL);
    CHECK(ptest_set(EXP_PWR_LCD, true) == ESP_FAIL && s_exp_out == before); /* shadow follows success only */
    CHECK(ptest_set(EXP_PWR_LCD, true) == ESP_OK && s_exp_out == (before | EXP_PWR_LCD) && exp_regs[2] == s_exp_out);

    muse_ptest_readback_t p = {0};
    exp_external = EXP_VBUS | EXP_STDBY;
    CHECK(muse_ptest_board_power(&p) == ESP_OK && !p.usb && !p.charging);
    exp_external = EXP_STDBY; /* VBUS low = USB present, standby high = charging */
    CHECK(muse_ptest_board_power(&p) == ESP_OK && p.usb && p.charging);
    exp_external = 0;
    CHECK(muse_ptest_board_power(&p) == ESP_OK && p.usb && !p.charging);
    inject("exp_read", 1, ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_power(&p) == ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_power(NULL) == ESP_ERR_INVALID_STATE);
    s_exp = NULL;
    CHECK(muse_ptest_board_power(&p) == ESP_ERR_INVALID_STATE);
    muse_ptest_state_t idle = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&idle) == ESP_ERR_INVALID_STATE);
}

static void scenario_apply_guards_and_order(void)
{
    muse_ptest_state_t s = state(MUSE_PTEST_UNSUPPORTED, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_ERR_NOT_SUPPORTED && !log_buf[0]);
    CHECK(muse_ptest_board_apply(NULL) == ESP_ERR_INVALID_STATE);
    /* An audio load with parked I2S is refused before anything is reverted. */
    CHECK(ptest_apply_knobs(MUSE_PTEST_RGB_LOW) == ESP_OK);
    s = state(MUSE_PTEST_MIC, -1, false, MUSE_PTEST_I2S_LOW, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_ERR_INVALID_STATE && s_ptest_knobs == MUSE_PTEST_RGB_LOW);

    /* Sine with the amplifier: amp off before codec work, on only after. */
    log_buf[0] = 0;
    s = state(MUSE_PTEST_SINE, 50, true, 0, MUSE_PTEST_CODEC_KEEP);
    s_ptest_frames = 7; s_ptest_adc_samples = 3; s_ptest_battery_mv = 1;
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    char want[256];
    snprintf(want, sizeof(want), "exp02=%04x,exp02=%04x,audio_open,lcd50,exp02=%04x,exp02=%04x,delay20",
             EXP_PWR_SYSTEM, EXP_PWR_SYSTEM, EXP_PWR_SYSTEM, EXP_PWR_SYSTEM | EXP_PWR_CODEC_PA);
    if (strcmp(log_buf, want)) { fprintf(stderr, "order: %s\n want: %s\n", log_buf, want); exit(1); }
    CHECK(s_ptest_knobs == 0 && hiz(40)); /* previous knobs reverted */
    CHECK(s_ptest_frames == 0 && s_ptest_adc_samples == 0 && s_ptest_battery_mv == 0);
    CHECK(s_ptest_expected_duty == 511 && s_ptest_load == MUSE_PTEST_SINE);

    /* Leaving audio closes it; ADC sampling powers the divider and inits ADC. */
    log_buf[0] = 0;
    s = state(MUSE_PTEST_ADC_SAMPLE, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    snprintf(want, sizeof(want), "exp02=%04x,exp02=%04x,audio_close,lcd_off,exp02=%04x,adc_init",
             EXP_PWR_SYSTEM | EXP_PWR_CODEC_PA, EXP_PWR_SYSTEM, EXP_PWR_SYSTEM | EXP_PWR_BAT_ADC);
    if (strcmp(log_buf, want)) { fprintf(stderr, "order: %s\n want: %s\n", log_buf, want); exit(1); }
    CHECK(s_ptest_expected_duty == 0 && (s_exp_out & EXP_PWR_BAT_ADC));

    /* A cold label is impossible once codecs were touched. */
    s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_COLD);
    CHECK(muse_ptest_board_apply(&s) == ESP_ERR_INVALID_STATE);
    /* A never-opened label is impossible after a suspend. */
    s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_INITIALIZED);
    CHECK(s_ptest_codec_state == MUSE_PTEST_CODEC_SUSPENDED && muse_ptest_board_apply(&s) == ESP_ERR_INVALID_STATE);
    /* A failed bring-up step stops before the knobs are applied. */
    s = state(MUSE_PTEST_IDLE, 25, false, MUSE_PTEST_RGB_LOW, MUSE_PTEST_CODEC_KEEP);
    inject("lcd", 1, ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_apply(&s) == ESP_ERR_TIMEOUT && s_ptest_knobs == 0);
}

static void scenario_codec_policies(void)
{
    muse_ptest_state_t s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_COLD);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK && s_ptest_codec_state == MUSE_PTEST_CODEC_COLD);
    log_buf[0] = 0;
    s = state(MUSE_PTEST_IDLE, -1, false, MUSE_PTEST_I2S_HIZ, MUSE_PTEST_CODEC_INITIALIZED);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    CHECK(strstr(log_buf, "audio_init,audio_close,lcd_off") && !strstr(log_buf, "audio_open"));
    CHECK(s_ptest_codec_state == MUSE_PTEST_CODEC_INITIALIZED && s_ptest_knobs == MUSE_PTEST_I2S_HIZ);
    log_buf[0] = 0;
    s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_SUSPENDED);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK && strstr(log_buf, "audio_open,audio_close,lcd_off"));
    CHECK(s_ptest_codec_state == MUSE_PTEST_CODEC_SUSPENDED && s_ptest_knobs == 0 && restored(I2S5, 5));
}

static void set_codec_bytes(unsigned history, bool es7243)
{
    memset(dac_regs, 0, sizeof(dac_regs));
    memset(adc_regs, 0, sizeof(adc_regs));
    adc_addr = es7243 ? ES7243_ADDR : ES7243E_ADDR;
    s_ptest_adc_addr = 0;
    if (history == MUSE_PTEST_CODEC_INITIALIZED) {
        dac_regs[0x0d] = 0xfa;
        if (es7243) { adc_regs[0x00] = 1; adc_regs[0x05] = 0x13; }
        else { adc_regs[0x00] = 0x80; adc_regs[0x01] = 0x3a; adc_regs[0x04] = 1; }
    } else {
        dac_regs[0x0d] = 0xfc; dac_regs[0x0e] = 0xff; dac_regs[0x12] = 2;
        if (es7243) { adc_regs[0x06] = 0x5c; }
        else { adc_regs[0x00] = 0x1e; adc_regs[0x04] = 1; adc_regs[0xf9] = 1; }
    }
}

static void scenario_readback(void)
{
    muse_ptest_state_t s = state(MUSE_PTEST_IDLE, 50, false, MUSE_PTEST_RGB_LOW, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    s_ptest_pwm = true;
    ledc_duty = 511;
    s_ptest_codec_state = MUSE_PTEST_CODEC_INITIALIZED;
    set_codec_bytes(MUSE_PTEST_CODEC_INITIALIZED, true);
    muse_ptest_readback_t r = {0};
    CHECK(muse_ptest_board_readback(&r) == ESP_OK);
    CHECK(!r.usb && r.output == s_exp_out && r.direction == EXP_INPUTS && r.backlight_duty == 511);
    CHECK(r.knobs_applied == MUSE_PTEST_RGB_LOW && r.codec_state == MUSE_PTEST_CODEC_INITIALIZED);
    muse_ptest_codec_regs_t regs;
    muse_ptest_board_codec_regs(&regs);
    CHECK(regs.adc_variant == 1 && regs.dac_mask == 0x7f && regs.adc_mask == 0x07 && regs.expected);
    CHECK(regs.dac[3] == 0xfa && regs.adc[1] == 0x13);

    /* Suspended ES7243: source-final bytes are expected; a stray byte is
     * reported as unexpected evidence without failing readback. */
    s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
    set_codec_bytes(MUSE_PTEST_CODEC_SUSPENDED, true);
    CHECK(muse_ptest_board_readback(&r) == ESP_OK);
    muse_ptest_board_codec_regs(&regs);
    CHECK(regs.expected);
    adc_regs[0x06] = 0x00;
    CHECK(muse_ptest_board_readback(&r) == ESP_OK);
    muse_ptest_board_codec_regs(&regs);
    CHECK(!regs.expected);
    /* Open codecs carry no fixed expectation. */
    set_codec_bytes(MUSE_PTEST_CODEC_SUSPENDED, false);
    s_ptest_spk_open = s_ptest_mic_open = true;
    CHECK(muse_ptest_board_readback(&r) == ESP_OK && r.codecs_open);
    muse_ptest_board_codec_regs(&regs);
    CHECK(!regs.expected && regs.adc_variant == 2 && regs.adc_mask == 0x0f);
    s_ptest_spk_open = s_ptest_mic_open = false;

    /* Hardware disagreement fails readback. */
    exp_regs[6] = EXP_INPUTS & ~1u;
    CHECK(muse_ptest_board_readback(&r) == ESP_FAIL);
    exp_regs[6] = EXP_INPUTS;
    exp_regs[2] ^= EXP_PWR_LCD; /* output register differs from the shadow */
    CHECK(muse_ptest_board_readback(&r) == ESP_FAIL);
    exp_regs[2] ^= EXP_PWR_LCD;
    ledc_duty = 500;
    CHECK(muse_ptest_board_readback(&r) == ESP_FAIL);
    ledc_duty = 511;
    gpio_set_level(40, 1);
    CHECK(muse_ptest_board_readback(&r) == ESP_FAIL);
    gpio_set_level(40, 0);
    codec_bad_reg = 0x0e; /* I2C transport error on one codec byte */
    CHECK(muse_ptest_board_readback(&r) == 263);
    muse_ptest_board_codec_regs(&regs);
    CHECK(!(regs.dac_mask & (1u << 4)) && !regs.expected);
    codec_bad_reg = 0x100;
    adc_addr = 0; /* no ADC answers the probe */
    s_ptest_adc_addr = 0;
    CHECK(muse_ptest_board_readback(&r) == ESP_FAIL);
    muse_ptest_board_codec_regs(&regs);
    CHECK(regs.adc_variant == 0 && regs.dac_mask == 0x7f);
    inject("exp_read", 2, ESP_ERR_TIMEOUT); /* output register read */
    adc_addr = ES7243E_ADDR;
    CHECK(muse_ptest_board_readback(&r) == ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_readback(NULL) == ESP_ERR_INVALID_ARG);
}

static void scenario_service(void)
{
    muse_ptest_state_t s = state(MUSE_PTEST_MIC, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    CHECK(muse_ptest_board_service(MUSE_PTEST_SINE) == ESP_ERR_INVALID_STATE);
    CHECK(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_OK && s_ptest_frames == 160);
    i2s_short = 4;
    CHECK(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_FAIL && s_ptest_frames == 160);
    i2s_short = 0;
    inject("i2s_read", 1, ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_service(MUSE_PTEST_MIC) == ESP_ERR_TIMEOUT && s_ptest_frames == 160);

    s = state(MUSE_PTEST_SINE, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK && s_ptest_frames == 0);
    CHECK(muse_ptest_board_service(MUSE_PTEST_SINE) == ESP_OK && s_ptest_frames == 160);
    int peak = 0;
    for (int i = 0; i < 160; i++) {
        CHECK(i2s_buf[2 * i] == i2s_buf[2 * i + 1]); /* both slots */
        CHECK(i < 16 || i2s_buf[2 * i] == i2s_buf[2 * (i - 16)]); /* 1 kHz at 16 kHz */
        if (abs(i2s_buf[2 * i]) > peak) { peak = abs(i2s_buf[2 * i]); }
    }
    CHECK(peak == 4125); /* -18 dBFS peak */
    inject("i2s_write", 1, ESP_FAIL);
    CHECK(muse_ptest_board_service(MUSE_PTEST_SINE) == ESP_FAIL && s_ptest_frames == 160);

    s = state(MUSE_PTEST_ADC_SAMPLE, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    now_us = 5000000;
    CHECK(muse_ptest_board_service(MUSE_PTEST_ADC_SAMPLE) == ESP_OK && power_reads == 1);
    CHECK(s_ptest_battery_mv == 3900 && s_ptest_adc_samples == 8);
    now_us += 999999;
    CHECK(muse_ptest_board_service(MUSE_PTEST_ADC_SAMPLE) == ESP_OK && power_reads == 1); /* 1 Hz */
    now_us += 1;
    inject("read_power", 1, ESP_ERR_TIMEOUT);
    CHECK(muse_ptest_board_service(MUSE_PTEST_ADC_SAMPLE) == ESP_ERR_TIMEOUT && s_ptest_adc_samples == 8);
    battery_mv = 3800;
    CHECK(muse_ptest_board_service(MUSE_PTEST_ADC_SAMPLE) == ESP_OK && s_ptest_battery_mv == 3800 && s_ptest_adc_samples == 16);

    s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_KEEP);
    CHECK(muse_ptest_board_apply(&s) == ESP_OK);
    CHECK(muse_ptest_board_service(MUSE_PTEST_IDLE) == ESP_OK && power_reads == 2);
}

static void scenario_force_suspend_sequences(void)
{
    /* ES7243E: the whole DAC and ADC vendor sequences run even after an
     * early write error; the first error is returned and warm is sticky. */
    set_codec_bytes(MUSE_PTEST_CODEC_SUSPENDED, false);
    inject("dac_write", 2, 263);
    dac_late_error = true;
    CHECK(ptest_force_suspend() == 263);
    dac_late_error = false;
    CHECK(codec_writes[0] == 15 && codec_writes[1] == 13 && muse_ptest_board_warm_seen());
    CHECK(dac_regs[0x0d] == 0xfc && dac_regs[0x02] == 0x00 && adc_regs[0x01] == 0x00);
    /* ES7243: its own six-step sequence, then a snapshot of the result. */
    codec_writes[0] = codec_writes[1] = 0;
    set_codec_bytes(MUSE_PTEST_CODEC_SUSPENDED, true);
    s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
    CHECK(ptest_force_suspend() == ESP_OK && codec_writes[0] == 15 && codec_writes[1] == 6);
    CHECK(adc_regs[0x06] == 0x5c && s_ptest_codec_regs.adc_variant == 1 && s_ptest_codec_regs.expected);
    /* No ADC answering still runs the DAC sequence and reports the probe. */
    codec_writes[0] = codec_writes[1] = 0;
    adc_addr = 0;
    s_ptest_adc_addr = 0;
    CHECK(ptest_force_suspend() == ESP_FAIL && codec_writes[0] == 15 && codec_writes[1] == 0);
}

static void scenario_codec_history_retention(void)
{
    /* The retained history never claims colder than the hardware is. */
    CHECK(muse_ptest_board_codec_history() == MUSE_PTEST_CODEC_COLD);
    s_ptest_audio_attempted = true; /* constructor started, reset before it returned */
    CHECK(muse_ptest_board_codec_history() == MUSE_PTEST_CODEC_INITIALIZED);
    s_ptest_codec_state = MUSE_PTEST_CODEC_SUSPENDED;
    CHECK(muse_ptest_board_codec_history() == MUSE_PTEST_CODEC_SUSPENDED);
    /* Restoring after a reset accepts only warm states; garbage is ignored. */
    s_ptest_audio_attempted = false;
    s_ptest_codec_state = MUSE_PTEST_CODEC_COLD;
    muse_ptest_board_restore_codec_history(MUSE_PTEST_CODEC_KEEP);
    muse_ptest_board_restore_codec_history(MUSE_PTEST_CODEC_COLD);
    muse_ptest_board_restore_codec_history(MUSE_PTEST_CODEC_SUSPENDED + 1);
    CHECK(s_ptest_codec_state == MUSE_PTEST_CODEC_COLD);
    muse_ptest_board_restore_codec_history(MUSE_PTEST_CODEC_INITIALIZED);
    CHECK(s_ptest_codec_state == MUSE_PTEST_CODEC_INITIALIZED);
    muse_ptest_board_restore_codec_history(MUSE_PTEST_CODEC_SUSPENDED);
    CHECK(muse_ptest_board_codec_history() == MUSE_PTEST_CODEC_SUSPENDED);
    /* A restored warm history blocks a cold label. */
    muse_ptest_state_t s = state(MUSE_PTEST_IDLE, -1, false, 0, MUSE_PTEST_CODEC_COLD);
    CHECK(muse_ptest_board_apply(&s) == ESP_ERR_INVALID_STATE);
}

int main(int argc, char **argv)
{
    if (argc != 2) { return 2; }
    boot();
    switch (atoi(argv[1])) {
    case 0: scenario_all_knobs_round_trip(); break;
    case 1: scenario_i2s_hiz_round_trip(); break;
    case 2: scenario_rejected_knobs_touch_nothing(); break;
    case 3: scenario_uart_parking(); break;
    case 4: scenario_readback_detects_drift(); break;
    case 5: scenario_failed_snapshot_and_restore(); break;
    case 6: scenario_revert_releases_owned_holds(); break;
    case 7: scenario_expander_ownership_and_power(); break;
    case 8: scenario_apply_guards_and_order(); break;
    case 9: scenario_codec_policies(); break;
    case 10: scenario_readback(); break;
    case 11: scenario_service(); break;
    case 12: scenario_force_suspend_sequences(); break;
    case 13: scenario_codec_history_retention(); break;
    default: return 2;
    }
    printf("ok\n");
    return 0;
}
