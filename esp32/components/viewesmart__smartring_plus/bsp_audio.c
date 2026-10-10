/*
 * bsp_audio — ES8311 codec + I2S（48kHz / 16bit / 立体声）
 *
 * 本文件内容：
 *   bsp_audio_init       — 建 I2S TX/RX、挂 ES8311（复用共享 I2C）、默认输出静音
 *   bsp_audio_get_codec  — esp_codec_dev 句柄（增益/音量等由应用设置）
 *   bsp_audio_get_rx     — I2S RX 通道（可读原始 PCM）
 *   bsp_audio_read       — 阻塞读满 len 字节（带有限重试）
 *   bsp_audio_write      — 经 codec 阻塞写 PCM（不做静音管理）
 *   bsp_audio_set_mute   — 输出静音开关
 *
 * 注意：
 *   - I2S 须 auto_clear_after_cb=true，否则停写后 DMA 残留会“尾音循环”
 *   - 回放时序：set_mute(false) → write → set_mute(true)；漏掉结尾静音会爆音
 *   - esp_codec_dev_write 返回 0 才成功（与 esp_err_t 相反）
 *   - 依赖须 CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n，避免与新 i2c_master 冲突重启
 *   - 须先 bsp_display_init()（I2C 在其中创建）；增益/音量推荐值见头文件注释
 */
#include "bsp/smartring_plus.h"
#include "bsp/board_config.h"

#include "esp_log.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/i2s_std.h"

static const char *TAG = "bsp_audio";

static i2s_chan_handle_t      s_tx;
static i2s_chan_handle_t      s_rx;
static esp_codec_dev_handle_t s_codec;

esp_err_t bsp_audio_init(void)
{
    if (s_codec) {
        ESP_LOGW(TAG, "bsp_audio 已初始化，跳过");
        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_bus();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "I2C 总线未初始化，须先 bsp_display_init()");
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- I2S 通道（TX+RX 同参数） ---- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    /* 欠载时自动发零；默认 false 会在停写时循环播放 DMA 残留（“尾音循环”） */
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_IO,
            .bclk = BOARD_I2S_BCLK_IO,
            .ws   = BOARD_I2S_WS_IO,
            .dout = BOARD_I2S_DOUT_IO,
            .din  = BOARD_I2S_DIN_IO,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化/使能失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        i2s_del_channel(s_rx);
        s_tx = s_rx = NULL;
        return err;
    }

    /* ---- ES8311 codec（esp_codec_dev），复用共享 I2C 总线 ---- */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,   /* 0x30（8 位）= 7 位 0x18，驱动内部 >>1 */
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   /* DAC 播放 + ADC 录音 */
        .pa_pin = BOARD_PA_CTRL_IO,                   /* 高电平使能（板载 10k 下拉） */
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,                         /* 模拟麦接 MIC1P */
        .hw_gain = {
            .pa_voltage = 3.3f,
            .codec_dac_voltage = 3.3f,
        },
        .mclk_div = BOARD_AUDIO_MCLK_MULT,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es_cfg);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!ctrl_if || !gpio_if || !data_if || !es8311 || !s_codec) {
        ESP_LOGE(TAG, "codec 接口创建失败");
        if (s_codec) {
            esp_codec_dev_delete(s_codec);
            s_codec = NULL;
        }
        i2s_channel_disable(s_tx);
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_tx);
        i2s_del_channel(s_rx);
        s_tx = s_rx = NULL;
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
        .mclk_multiple = BOARD_AUDIO_MCLK_MULT,
    };
    if (esp_codec_dev_open(s_codec, &fs) != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_open 失败");
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
        i2s_channel_disable(s_tx);
        i2s_channel_disable(s_rx);
        i2s_del_channel(s_tx);
        i2s_del_channel(s_rx);
        s_tx = s_rx = NULL;
        return ESP_FAIL;
    }

    /* 默认静音：增益/音量由应用层设置（推荐值见头文件 bsp_audio_init 注释） */
    esp_codec_dev_set_out_mute(s_codec, true);

    ESP_LOGI(TAG, "音频初始化完成：%d Hz / 16bit / 立体声, MCLK=%dx",
             BOARD_AUDIO_SAMPLE_RATE, BOARD_AUDIO_MCLK_MULT);
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_get_codec(void)
{
    return s_codec;
}

i2s_chan_handle_t bsp_audio_get_rx(void)
{
    return s_rx;
}

esp_err_t bsp_audio_read(void *buf, size_t len, uint32_t timeout_ms)
{
    if (!s_rx || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 单次读失败最多重试若干次，避免硬件异常时死循环挂死录音任务 */
    enum { MAX_RETRY = 8 };

    uint8_t *p = buf;
    size_t done = 0;
    int retries = 0;
    while (done < len) {
        size_t br = 0;
        esp_err_t err = i2s_channel_read(s_rx, p + done, len - done, &br,
                                         pdMS_TO_TICKS(timeout_ms));
        if (err != ESP_OK || br == 0) {
            if (++retries > MAX_RETRY) {
                ESP_LOGE(TAG, "I2S RX 连续失败: %s", esp_err_to_name(err));
                return (err != ESP_OK) ? err : ESP_ERR_TIMEOUT;
            }
            ESP_LOGW(TAG, "I2S RX 读取失败: %s，重试 (%d/%d)",
                     esp_err_to_name(err), retries, MAX_RETRY);
            continue;
        }
        retries = 0;
        done += br;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_write(const void *buf, size_t len)
{
    if (!s_codec || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* esp_codec_dev_write 返回 0 = 成功，负值 = 失败 */
    return (esp_codec_dev_write(s_codec, (void *)buf, len) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bsp_audio_set_mute(bool mute)
{
    if (!s_codec) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_codec_dev_set_out_mute(s_codec, mute) == 0) ? ESP_OK : ESP_FAIL;
}
