# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise Audio Board controls and failures with fake I2C, I2S and codecs."""

import os
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAKE = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2s_chan_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define ES7210_CODEC_DEFAULT_ADDR 0x80
#define ES7210_SEL_MIC1 1
#define ES7210_SEL_MIC2 2
#define ES7210_SEL_MIC3 4
#define ES7210_SEL_MIC4 8
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define I2S_CHANNEL_DEFAULT_CONFIG(num, role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) 32
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef struct {
 int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
 struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
typedef struct {
 int clk_cfg, slot_cfg;
 struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
typedef struct audio_codec_if_t audio_codec_if_t;
struct audio_codec_if_t {
 int (*enable)(const audio_codec_if_t *, bool);
 int (*set_fs)(const audio_codec_if_t *, esp_codec_dev_sample_info_t *);
 int (*set_vol)(const audio_codec_if_t *, float);
 int (*mute)(const audio_codec_if_t *, bool);
 int (*set_mic_gain)(const audio_codec_if_t *, float);
};
typedef struct { int addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef int audio_codec_ctrl_if_t;
typedef int audio_codec_gpio_if_t;
typedef struct {
 const audio_codec_ctrl_if_t *ctrl_if;
 const audio_codec_gpio_if_t *gpio_if;
 int codec_mode, pa_pin; bool use_mclk;
} es8311_codec_cfg_t;
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; int mic_selected; } es7210_codec_cfg_t;
const char *esp_err_to_name(int);
void *xSemaphoreCreateMutex(void);
void xSemaphoreTake(void *, uint32_t);
void xSemaphoreGive(void *);
void vSemaphoreDelete(void *);
int i2c_new_master_bus(const i2c_master_bus_config_t *, void **);
int i2c_master_bus_add_device(void *, const i2c_device_config_t *, void **);
int i2c_master_transmit_receive(void *, const uint8_t *, size_t, uint8_t *, size_t, int);
int i2c_master_transmit(void *, const uint8_t *, size_t, int);
int i2c_master_bus_rm_device(void *);
int i2c_del_master_bus(void *);
int i2s_new_channel(const i2s_chan_config_t *, void **, void **);
int i2s_channel_init_std_mode(void *, const i2s_std_config_t *);
int i2s_channel_enable(void *);
int i2s_channel_disable(void *);
int i2s_del_channel(void *);
int i2s_channel_read(void *, void *, size_t, size_t *, int);
int i2s_channel_write(void *, const void *, size_t, size_t *, int);
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *);
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *);
int audio_codec_delete_codec_if(const audio_codec_if_t *);
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *);
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *);
"""
HARNESS = r"""
#include "fake.h"
#include "voice_board_waveshare_audio.c"
static int bus, io, tx, rx, lock, ctrl1, ctrl2, gpio;
static bool tx_on, rx_on, adc_on, muted, short_write;
static int locked, read_error, write_error, codec_error, disable_tx;
static uint8_t regs[8] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0xff, 0xff};
static float gain;
static audio_codec_if_t dac, adc;
const char *esp_err_to_name(int e) { (void)e; return "fake"; }
void *xSemaphoreCreateMutex(void) { return &lock; }
void xSemaphoreTake(void *h, uint32_t t) { (void)t; assert(h==&lock && !locked); locked=1; }
void xSemaphoreGive(void *h) { assert(h==&lock && locked); locked=0; }
void vSemaphoreDelete(void *h) { assert(h==&lock && !locked); }
int i2c_new_master_bus(const i2c_master_bus_config_t *c, void **h) {
 assert(c->sda_io_num==11 && c->scl_io_num==10); *h=&bus; return 0;
}
int i2c_master_bus_add_device(void *h, const i2c_device_config_t *c, void **d) {
 assert(h==&bus && c->device_address==0x20); *d=&io; return 0;
}
int i2c_master_transmit_receive(void *h, const uint8_t *r, size_t n, uint8_t *v, size_t m, int t) {
 (void)t; assert(h==&io && n==1 && m==1 && *r<8);
 if(read_error) return read_error;
 *v=regs[*r]; return 0;
}
int i2c_master_transmit(void *h, const uint8_t *v, size_t n, int t) {
 (void)t; assert(h==&io && n==2 && v[0]<8);
 if(write_error) return write_error;
 regs[v[0]]=v[1]; return 0;
}
int i2c_master_bus_rm_device(void *h) { assert(h==&io); return 0; }
int i2c_del_master_bus(void *h) { assert(h==&bus); return 0; }
int i2s_new_channel(const i2s_chan_config_t *c, void **o, void **i) {
 assert(c->dma_frame_num==320); *o=&tx; *i=&rx; return 0;
}
int i2s_channel_init_std_mode(void *h, const i2s_std_config_t *c) {
 assert(h==&tx || h==&rx); assert(c->clk_cfg==16000);
 assert(c->gpio_cfg.mclk==12 && c->gpio_cfg.bclk==13 && c->gpio_cfg.ws==14);
 assert(c->gpio_cfg.din==15 && c->gpio_cfg.dout==16); return 0;
}
int i2s_channel_enable(void *h) {
 if(h==&tx) { assert(!tx_on); tx_on=true; }
 else { assert(h==&rx && !rx_on && adc_on); rx_on=true; } return 0;
}
int i2s_channel_disable(void *h) {
 if(h==&tx) { tx_on=false; disable_tx++; }
 else { assert(h==&rx); rx_on=false; } return 0;
}
int i2s_del_channel(void *h) { assert(h==&tx || h==&rx); return 0; }
int i2s_channel_read(void *h, void *p, size_t n, size_t *read, int t) {
 (void)t; assert(h==&rx && rx_on && tx_on); memset(p, 0, n); *read=n; return 0;
}
int i2s_channel_write(void *h, const void *p, size_t n, size_t *written, int t) {
 (void)t;(void)p; assert(h==&tx && tx_on); *written=short_write?n-4:n; return write_error;
}
static int enable(const audio_codec_if_t *h, bool on) {
 if(codec_error) return codec_error;
 if(h==&adc) adc_on=on;
 return 0;
}
static int set_fs(const audio_codec_if_t *h, esp_codec_dev_sample_info_t *f) {
 (void)h; assert(f->sample_rate==16000 && f->channel==2 && f->bits_per_sample==32); return codec_error;
}
static int vol(const audio_codec_if_t *h, float db) { assert(h==&dac && locked); gain=db; return codec_error; }
static int mute(const audio_codec_if_t *h, bool m) { assert(h==&dac && locked); muted=m; return codec_error; }
static int mic_gain(const audio_codec_if_t *h, float db) { assert(h==&adc && db==30); return codec_error; }
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *c) {
 assert(c->bus_handle==&bus); return c->addr==0x30?&ctrl1:&ctrl2;
}
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { return &gpio; }
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *c) {
 assert(c->pa_pin==-1 && c->ctrl_if==&ctrl1); dac=(audio_codec_if_t){enable,set_fs,vol,mute,mic_gain}; return &dac;
}
const audio_codec_if_t *es7210_codec_new(es7210_codec_cfg_t *c) {
 assert(c->ctrl_if==&ctrl2 && c->mic_selected==15); adc=dac; return &adc;
}
int audio_codec_delete_codec_if(const audio_codec_if_t *h) { assert(h==&adc || h==&dac); return 0; }
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *h) { assert(h==&ctrl1 || h==&ctrl2); return 0; }
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *h) { assert(h==&gpio); return 0; }
int main(int argc, char **argv) {
 assert(argc==2); int scenario=atoi(argv[1]);
 if(scenario==3 || scenario==4) {
  if(scenario==3) read_error=7; else codec_error=7; assert(voice_board_init()!=ESP_OK);
  assert(!s_bus && !s_io && !s_lock && !s_rx && !s_tx);
  read_error=codec_error=0; assert(voice_board_init()==ESP_OK); cleanup(); return 0;
 }
 assert(voice_board_init()==ESP_OK);
 assert(tx_on && !rx_on && !adc_on && !(regs[3]&1));
 assert((regs[3]&0xfe)==0xfe && regs[7]==0xfe); // Preserve unrelated expander lines.
 if(scenario==0) {
  int16_t samples[320]; int peak;
  assert(voice_board_mic_read(samples,320,&peak)==0);
  assert(voice_board_mic_start()==ESP_OK);
  assert(voice_board_mic_read(samples,320,&peak)==320);
  voice_board_mic_stop(); assert(!rx_on && !adc_on && tx_on && !disable_tx);
  assert(voice_board_mic_start()==ESP_OK); voice_board_mic_stop();
  regs[1]&=~2; assert(voice_board_muted()); assert(voice_board_mic_start()!=ESP_OK);
  regs[1]=0xff; read_error=7; assert(voice_board_muted());
 } else if(scenario==1) {
  voice_board_set_volume(-1); assert(gain==-50 && muted);
  voice_board_set_volume(101); assert(gain==0 && !muted);
  voice_board_amp(true); assert(regs[3]==0xff);
  codec_error=7; voice_board_set_volume(50); assert(!(regs[3]&1)); codec_error=0;
  regs[1]=0xf7; assert(voice_board_dial_steps()==0); assert(voice_board_dial_steps()==1);
  assert(voice_board_dial_steps()==0); regs[1]=0xff;
  assert(voice_board_dial_steps()==0); assert(voice_board_dial_steps()==0);
  regs[1]=0xfb; assert(voice_board_dial_steps()==0); assert(voice_board_dial_steps()==-1);
 } else {
  int32_t frames[6]={0}; assert(voice_board_speaker_write(frames,2)==ESP_ERR_INVALID_SIZE);
  assert(voice_board_speaker_write(frames,3)==ESP_OK);
  short_write=true; assert(voice_board_speaker_write(frames,3)==ESP_ERR_TIMEOUT);
  short_write=false; write_error=7; assert(voice_board_speaker_write(frames,3)==7);
 }
 read_error=write_error=0; cleanup(); assert(!tx_on && !s_bus && !s_lock); return 0;
}
"""


class WaveshareAudioBoard(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name)
        (folder / "fake.h").write_text(FAKE)
        for name in [
            "driver/i2c_master.h",
            "driver/i2s_std.h",
            "esp_codec_dev_defaults.h",
            "esp_err.h",
            "esp_log.h",
            "freertos/FreeRTOS.h",
            "freertos/semphr.h",
        ]:
            header = folder / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake.h"\n')
        source = folder / "board.c"
        source.write_text(HARNESS)
        cls.binary = folder / "board"
        subprocess.run(
            shlex.split(os.environ.get("CC", "cc"))
            + [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I",
                str(folder),
                "-I",
                str(ROOT / "main"),
                str(source),
                "-o",
                str(cls.binary),
            ],
            check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def test_capture_lifecycle_and_fail_closed_mute(self) -> None:
        subprocess.run([str(self.binary), "0"], check=True)

    def test_volume_amp_safety_and_key_debounce(self) -> None:
        subprocess.run([str(self.binary), "1"], check=True)

    def test_playback_write_failures(self) -> None:
        subprocess.run([str(self.binary), "2"], check=True)

    def test_failed_init_cleans_up_and_allows_retry(self) -> None:
        subprocess.run([str(self.binary), "3"], check=True)

    def test_codec_init_failure_cleans_up_and_allows_retry(self) -> None:
        subprocess.run([str(self.binary), "4"], check=True)
