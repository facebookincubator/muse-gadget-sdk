# smartring_plus

Board Support Package (BSP) for **SmartRing-Plus** —— SmartRing-Plus development board designed by VIEWE based on **ESP32-S3-N16R8**.

| | |
|---|---|
| **Author** | Ayang |
| **Company** | SHENZHEN VIEWE TECHNOLOGY CO.,LTD |
| **Target** | `esp32s3` (16 MB Flash + 8 MB Octal PSRAM recommended) |
| **ESP-IDF** | ≥ 5.5 |
| **License** | Apache-2.0 |

This component wraps the board’s verified peripherals behind a stable `bsp_*` API so application code does not hard-code pins, init sequences, or bus wiring.

Vendored into muse-gadget-sdk. The Muse board file uses this BSP for the display, touch, shared I2C bus and battery only. It does not call `bsp_audio_init()` (that opens the codec and leaves the speaker muted) or `bsp_power_shutdown()`. Audio is a 16 kHz I2S in `components/muse/boards/board_viewe_smartring_plus.c`. Power-off is the hardware switch; Muse's `power_off()` returns `ESP_ERR_NOT_SUPPORTED`.

```c
#include "bsp/smartring_plus.h"
```

---

## Features

| Subsystem | Hardware | Primary API |
|-----------|----------|-------------|
| Display | ST77916 QSPI, 360×360 round | `bsp_display_init()`, backlight / color-swap helpers |
| Touch | CST816 (I2C) | Registered as LVGL pointer; `bsp_touch_get_handle()` |
| Shared bus | I2C SDA=8 / SCL=9 / 400 kHz | `bsp_i2c_get_bus()` |
| Audio | ES8311 + I2S + PA | `bsp_audio_init()` / `read` / `write` / `set_mute` |
| Storage | SDMMC 4-bit FAT | `bsp_sd_init()`, mount `/sdcard` |
| Battery / PMIC | **V1** ADC GPIO1; **V2** AXP2101 (runtime probe) | `bsp_battery_*` / `bsp_battery_get_pmic_type()` |
| Power | GPIO47 soft power-off (same for V1/V2) | `bsp_power_shutdown()` |
| IMU | QMI8658A | `bsp_imu_init()` / `get_accel` / `get_tilt` / calib helpers |
| UI stack | `esp_lvgl_adapter` + LVGL 9 | Ready after `bsp_display_init()` |

Pin numbers, I2C addresses, and mount points live in a single source of truth:  
[`include/bsp/board_config.h`](include/bsp/board_config.h).

---

## Add as a dependency

In your project’s `idf_component.yml` (or `main/idf_component.yml`):

```yaml
dependencies:
  viewesmart/smartring_plus: "^1.1.2"
```

Pull dependencies:

```bash
idf.py set-target esp32s3
idf.py reconfigure
```

Alternatively, place this folder under your project's `components/smartring_plus` for a local dependency.

---

## Recommended initialization order

Several modules share the I2C bus created inside `bsp_display_init()`. Call display first, then optional peripherals:

```c
#include "bsp/smartring_plus.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());   /* I2C + touch + LCD + LVGL adapter + backlight */
    ESP_ERROR_CHECK(bsp_battery_init());
    ESP_ERROR_CHECK(bsp_power_init());

    /* Optional — fail softly if card / IMU absent */
    if (bsp_sd_init() != ESP_OK) {
        ESP_LOGW("app", "SD not mounted");
    }
    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW("app", "IMU init failed");
    }
    ESP_ERROR_CHECK(bsp_audio_init());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    /* create LVGL screens here */
    esp_lv_adapter_unlock();
}
```

---

## API reference

All public APIs are declared in [`include/bsp/smartring_plus.h`](include/bsp/smartring_plus.h).

### Display & touch

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_display_init(void)` | Bring up shared I2C, CST816 touch, ST77916 QSPI panel, `esp_lvgl_adapter`, and backlight at 100%. After success, the LVGL task is running. |
| `void bsp_display_backlight_set(uint8_t percent)` | Backlight PWM 0–100 (GPIO46, active high). |
| `void bsp_display_set_swap_bytes(bool enable)` | Runtime RGB565 byte-swap (debug / panel variants); forces full redraw. |
| `bool bsp_display_get_swap_bytes(void)` | Current swap state. |
| `lv_display_t *bsp_display_get_handle(void)` | LVGL display handle. |
| `esp_lcd_touch_handle_t bsp_touch_get_handle(void)` | Touch handle for raw reads; `NULL` if not ready. |
| `i2c_master_bus_handle_t bsp_i2c_get_bus(void)` | Shared I2C bus for codec / IMU; `NULL` before display init. |

**LVGL note:** Always take `esp_lv_adapter_lock(-1)` before creating or updating UI objects, then `esp_lv_adapter_unlock()`.

```c
ESP_ERROR_CHECK(bsp_display_init());
bsp_display_backlight_set(80);

ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
lv_obj_t *scr = lv_obj_create(NULL);
lv_screen_load(scr);
esp_lv_adapter_unlock();
```

---

### Battery / PMIC (V1 ADC and V2 AXP2101)

**Cell (same on both HW revisions):** single-cell LiPo **3.7V / 600mAh**, CV 4.2V. Specs live in `board_config.h` (`BOARD_BAT_*`).

`bsp_battery_init()` **probes** AXP2101 on the shared I2C bus (read `0x03`, expect CHIP ID `0x4A`):

| Path | When | Fuel / charge | Also |
|------|------|---------------|------|
| **V2** | Probe OK | REG `0xA4` %; `0x01[6:5]` direction; VBAT ADC `0x34/0x35` (1 mV/LSB) | ALDO3=3.3V; ICC 300mA (0.5C); CV 4.2V; precharge 75mA; Iterm 25mA |
| **V1** | No AXP / ID mismatch | GPIO1 ADC + same-cell OCV table | Charge from voltage hysteresis; IR-drop compensate while charging |

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_battery_init(void)` | Probe and start 1 Hz sampler (requires `bsp_display_init` first). |
| `bsp_pmic_type_t bsp_battery_get_pmic_type(void)` | `BSP_PMIC_V1_ADC` / `BSP_PMIC_V2_AXP2101`. |
| `void bsp_battery_get_data(bsp_battery_data_t *out)` | Thread-safe snapshot. |
| `float bsp_battery_get_voltage(void)` | Latest voltage (V). |
| `int bsp_battery_get_percent(void)` | 0–100 %. |
| `bool bsp_battery_is_charging(void)` | Charging state. |

```c
ESP_ERROR_CHECK(bsp_battery_init());
ESP_LOGI("bat", "PMIC=%d", (int)bsp_battery_get_pmic_type());

bsp_battery_data_t bat;
bsp_battery_get_data(&bat);
ESP_LOGI("bat", "%.2f V  %d%%  %s",
         bat.voltage_v, bat.percent,
         bat.charging ? "charging" : "discharge");
```

---

### Power

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_power_init(void)` | Configure soft-off GPIO (default idle). |
| `void bsp_power_shutdown(void)` | Soft power-off: hold GPIO47 high for `BOARD_PW_OFF_HOLD_MS` (3.5 s). Normally does not return. |

**V1 and V2 both use GPIO47**; AXP2101 is not used for shutdown.

```c
ESP_ERROR_CHECK(bsp_power_init());
/* … user confirmed … */
bsp_power_shutdown();
```

---

### Audio (ES8311 + I2S)

Fixed stream format: **48 kHz / 16-bit / stereo interleaved** (~192 KB/s).  
`bsp_audio_init()` only opens hardware; it does **not** set gain/volume — configure via `esp_codec_dev` on the codec handle.

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_audio_init(void)` | I2S + ES8311; requires prior `bsp_display_init()` for I2C. Idempotent (second call skips). PA is driven by the codec `pa_pin`. |
| `esp_codec_dev_handle_t bsp_audio_get_codec(void)` | Codec handle for gain / volume / mute / streaming. |
| `i2s_chan_handle_t bsp_audio_get_rx(void)` | I2S RX channel for raw PCM. |
| `esp_err_t bsp_audio_read(void *buf, size_t len, uint32_t timeout_ms)` | Blocking record until `len` bytes filled. |
| `esp_err_t bsp_audio_write(const void *buf, size_t len)` | Blocking playback of `len` bytes. |
| `esp_err_t bsp_audio_set_mute(bool mute)` | Output mute. |

**Playback sequence (important):** unmute → write (can be chunked) → mute again. Skipping the final mute can cause a pop/click.

**Suggested app settings (board-tuned, not hard requirements):**

- Mic gain ≈ **36 dB** via `esp_codec_dev_set_in_gain()` (driver snaps to 6 dB steps).
- Playback volume ≈ **75** via `esp_codec_dev_set_out_vol()` (0–100).

```c
ESP_ERROR_CHECK(bsp_audio_init());

esp_codec_dev_handle_t codec = bsp_audio_get_codec();
esp_codec_dev_set_out_vol(codec, 75);
esp_codec_dev_set_in_gain(codec, 36.0f);

/* playback */
ESP_ERROR_CHECK(bsp_audio_set_mute(false));
ESP_ERROR_CHECK(bsp_audio_write(pcm, pcm_bytes));
ESP_ERROR_CHECK(bsp_audio_set_mute(true));

/* record */
uint8_t buf[48 * 2 * 2 * 100]; /* ~100 ms stereo */
ESP_ERROR_CHECK(bsp_audio_read(buf, sizeof(buf), 1000));
```

> When calling `esp_codec_dev_write()` directly, **return value `0` means success** (opposite of `esp_err_t` convention).

---

### IMU (QMI8658A)

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_imu_init(void)` | ±2 g / 250 Hz accel + 250 Hz sample task + EMA filter. Needs shared I2C (`bsp_display_init()` first). |
| `void bsp_imu_get_accel(float *ax, float *ay, float *az)` | Filtered acceleration (m/s²), board axes, thread-safe. |
| `void bsp_imu_get_tilt(float *gx, float *gy)` | Screen-plane tilt vector (m/s²); subtracts calib zero if calibrated. `+gx` = right, `+gy` = down. |

**Level calibration (optional reference):**

| Function | Description |
|----------|-------------|
| `void bsp_imu_calib_start(void)` | Start / restart calibration window (30 s deadline). |
| `bsp_imu_calib_status_t bsp_imu_calib_poll(void)` | `IDLE` / `RUNNING` / `PASSED` / `FAILED`. Poll from UI every 100–200 ms. |
| `void bsp_imu_calib_get_info(float *std_g, float *tilt_deg, uint32_t *elapsed_ms)` | Debug metrics during `RUNNING`. |

```c
ESP_ERROR_CHECK(bsp_imu_init());

float ax, ay, az, gx, gy;
bsp_imu_get_accel(&ax, &ay, &az);
bsp_imu_get_tilt(&gx, &gy);

bsp_imu_calib_start();
while (bsp_imu_calib_poll() == BSP_IMU_CALIB_RUNNING) {
    vTaskDelay(pdMS_TO_TICKS(100));
}
```

You may ignore calibration entirely and only use `bsp_imu_get_accel()`.

---

### SD card

Mount point: **`/sdcard`** (`BOARD_SD_MOUNT_POINT`). No card-detect pin — software cannot distinguish “no card” from “mount failure”.

| Function | Description |
|----------|-------------|
| `esp_err_t bsp_sd_init(void)` | SDMMC 4-bit + FAT VFS. Does not assert/reboot on failure. Second call → `ESP_ERR_INVALID_STATE`. |
| `bool bsp_sd_is_mounted(void)` | Mount status. |
| `void bsp_sd_deinit(void)` | Unmount (safe if not mounted). |
| `const sdmmc_card_t *bsp_sd_get_card(void)` | Card info for `sdmmc_card_print_info()`. |
| `esp_err_t bsp_sd_list(const char *path, bsp_sd_entry_t **entries, size_t *count)` | Sorted directory listing (dirs first). Caller must `bsp_sd_free_list()`. |
| `void bsp_sd_free_list(bsp_sd_entry_t *entries, size_t count)` | Free list from `bsp_sd_list()`. |

```c
if (bsp_sd_init() == ESP_OK) {
    bsp_sd_entry_t *ents = NULL;
    size_t n = 0;
    if (bsp_sd_list("/sdcard", &ents, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI("sd", "%s %s",
                     ents[i].type == BSP_SD_ENTRY_DIR ? "DIR " : "FILE",
                     ents[i].name);
        }
        bsp_sd_free_list(ents, n);
    }
}
```

---

## Pin map (summary)

| Function | GPIOs / notes |
|----------|----------------|
| I2C | SDA **8**, SCL **9**, 400 kHz |
| Touch CST816 | Addr `0x15`, RST **40**, INT **41** |
| LCD ST77916 QSPI | SCL **10**, CS **11**, D0–D3 **12/13/15/14**, RST **39**, TE **38** |
| Backlight | **46** (LEDC PWM) |
| SDMMC | CLK **4**, CMD **5**, D0–D3 **3/2/7/6** |
| Battery ADC (V1) | **1** (ADC1_CH0, ÷2) |
| AXP2101 (V2) | I2C addr `0x34` (shared bus) |
| Soft power-off | **47** (hold high ~3.5 s; same for V1/V2) |
| Audio I2S | MCLK **48**, BCLK **21**, WS **17**, DOUT **16**, DIN **18** |
| PA enable | **45** |
| Codec ES8311 | Addr `0x18` on shared I2C |
| IMU INT1 | **42** (reserved; driver currently polls) |

Reserved / do not reuse as GPIO: BOOT (0), USB (19/20), UART0 (43/44), Octal PSRAM (33–37).

Full macros: `include/bsp/board_config.h`.

---

## sdkconfig notes

Minimal settings that this BSP expects:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=n
CONFIG_LV_USE_CLIB_MALLOC=y
```

Also enable Montserrat fonts / LVGL options as required by your UI.  
See the SmartRing-Plus **full-device** demo `sdkconfig.defaults` for a known-good complete set.

---

## Component layout

```
smartring_plus/
├── include/bsp/
│   ├── smartring_plus.h   # Public API
│   └── board_config.h     # Pins & board constants
├── smartring_bsp.c        # Display / touch / I2C / LVGL adapter
├── bsp_audio.c
├── bsp_battery.c
├── bsp_axp2101.c              # V2 AXP2101 low-level
├── bsp_power.c
├── bsp_imu.c
├── bsp_sd.c
├── lcd_init_seq.h         # ST77916 init sequence (vendor B)
├── CMakeLists.txt
├── idf_component.yml
└── LICENSE
```

---

## Dependencies (pulled by `idf_component.yml`)

- `espressif/esp_lvgl_adapter`
- `lvgl/lvgl` 9.x
- `espressif/esp_lcd_st77916`
- `espressif/esp_lcd_touch_cst816s`
- `espressif/esp_codec_dev`
- `waveshare/qmi8658`

---

## License

Apache-2.0  
Copyright 2024–2026 Ayang / SHENZHEN VIEWE TECHNOLOGY CO.,LTD

---

## Acknowledgments

Special thanks to **[@nianhua-entropy](https://gitee.com/nianhua-entropy)** for the generous help that made this BSP possible.
