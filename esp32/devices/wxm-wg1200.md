<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# WeatherXM WG1200

This profile runs Muse's full LVGL avatar UI, touch settings, BLE setup, Wi-Fi,
the home-network tunnel, indoor barometric pressure (`BMP390`), outdoor sub-GHz
LoRa weather-station reception (`SX1262` for `WS1001` and `WS1300` arrays) and
non-destructive dual-boot (`ota_1`) on the **WeatherXM WG1200** desktop gateway
(the indoor console of the [WeatherXM D1 Wi-Fi Weather Station](https://weatherxm.com/stations/),
an OEM variant of the Seeed SenseCAP Indicator D1L).

- [WeatherXM Weather Stations](https://weatherxm.com/stations/)
- [1-click signed dual-boot web flasher & BLE provisioner (`muse.weatherxm.com`)](https://muse.weatherxm.com/)
- [WeatherXM Muse fork & web flasher source (`WeatherXM/muse-gadget-sdk`)](https://github.com/WeatherXM/muse-gadget-sdk)
- [WeatherXM WG1200 firmware repository](https://github.com/WeatherXM/wg1200-firmware)
- [Seeed SenseCAP Indicator reference](https://wiki.seeedstudio.com/SenseCAP_Indicator_Get_Started/)

## Hardware differences from the Seeed SenseCAP Indicator

Although the WG1200 shares the 4.0-inch 480×480 RGB565 panel and enclosure of
the standard Seeed SenseCAP Indicator (`sdkconfig.sensecap-indicator`), the
**WG1200 v1.1 PCB** differs in flash layout, sensors, audio and boot security:

| Subsystem | Seeed SenseCAP Indicator | WeatherXM WG1200 v1.1 |
|---|---|---|
| **Module & flash** | 8 MB flash (partition table at `0x8000`), 8 MB octal PSRAM | `ESP32-S3-WROOM-1-N16R8`: **16 MB QSPI flash** (bootloader expects partition table at **`0xC000`**), **8 MB octal PSRAM** |
| **Secondary MCU** | RP2040 on UART (`GPIO19`/`GPIO20`) reading air sensors and buzzer | **RP2040 removed** (`GPIO19`/`GPIO20` unpopulated) |
| **Indoor barometer** | None on ESP32 bus | **Bosch BMP390** (`0x77`) on `I2C0` (`GPIO39` SDA, `GPIO40` SCL), power-gated and I2C-isolated via `PCA9535 IO1_2` |
| **Buzzer** | Driven by RP2040 | Passive SMD buzzer driven via **`PCA9535 IO1_5`** (square wave over I2C); no speaker or microphone |
| **Sub-GHz radio** | Unused | **Semtech SX1262** on `SPI3_HOST` (`SCLK=41`, `MOSI=48`, `MISO=47`, shared with `ST7701S` 9-bit SPI init pins released after panel init; `NSS=IO0_0`, `RST=IO0_1`, `BUSY=IO0_2`, `DIO1=IO0_3`) |
| **Touch & RGB timing** | Status-screen build (`MUSE_ENABLED=n`), 16 MHz PCLK | **FT6336U** touch (`0x48` GX / `0x38` DX, `TP_INT=IO0_6`, `TP_RST=IO0_7`), 12 MHz PCLK with 10-line internal DMA bounce buffers (`480×10`) |
| **Buttons** | `GPIO0` BOOT, `GPIO38` top | **`GPIO10`** BOOT, **`GPIO38`** active-low top user button |
| **USB** | CH340 bridge | Internal USB 2.0 hub exposing **both** native ESP32-S3 USB Serial/JTAG (`303a:1001`) and CH340 UART0 (`1a86:7523`) |
| **Secure Boot** | Unsigned | Retail WG1200 bootloaders enforce **RSA-3072 Secure Boot V2** app verification |

### `PCA9535PW` (`0x20` / `0x39`) pin map

| Line | Net | Direction | Role |
|---|---|---|---|
| `IO0_0` | `LORA_NSS` | Output | SX1262 SPI chip select (active low) |
| `IO0_1` | `LORA_RST` | Output | SX1262 reset (active low) |
| `IO0_2` | `LORA_BUSY` | Input | SX1262 busy |
| `IO0_3` | `LORA_DIO1` | Input | SX1262 interrupt (`DIO1`) |
| `IO0_4` | `LCD_CS` | Output | ST7701S 9-bit SPI chip select (active low) |
| `IO0_5` | `LCD_REST` | Output | ST7701S reset (active low) |
| `IO0_6` | `TP_INT` | Input | FT6336U touch interrupt (active low) |
| `IO0_7` | `TP_REST` | Output | FT6336U touch reset (active low) |
| `IO1_2` | `BMP_PWR_SW` | Output | Active-high power and I2C FET enable for `BMP390` (`0x77`) |
| `IO1_3` | `SD_CS` | Output | MicroSD chip select (held high / deselected) |
| `IO1_4` | `SD_DET` | Input | MicroSD card detect (unused) |
| `IO1_5` | `BUZZER` | Output | Drives `Q7` (`BC817W`) passive buzzer |

## Non-destructive dual-boot layout

Retail WG1200 units keep their factory X.509 certificate, station identity,
encrypted NVS credentials and stock WeatherXM firmware across the first
15.1 MB of flash. [`partitions_wg1200_dualboot.csv`](../partitions_wg1200_dualboot.csv)
preserves every stock partition at its exact offset, installs Muse into
`ota_1` (`0x820000`), and redirects all NVS calls in the Muse build (`--wrap=nvs_*`
in [`wxm_boot_mgr.c`](../components/muse/wxm_boot_mgr.c)) to an isolated 64 KB
`muse_nvs` partition at `0xF21000`:

| Partition | Offset | Size | Role |
|---|---|---|---|
| `bootloader` | `0x000000` | 48 KB | Stock WG1200 2nd-stage bootloader (**untouched**) |
| `partition-table` | `0x00C000` | 4 KB | Stock layout + `muse_nvs` appended at `0xF21000` |
| `esp_secure_cert` | `0x00D000` | 8 KB | Station X.509 identity (**untouched**) |
| `nvs` | `0x00F000` | 16 KB | Encrypted WeatherXM NVS (**untouched**) |
| `otadata` | `0x013000` | 8 KB | Boot selector (`ota_1` = Muse, `ota_0`/`factory` = WeatherXM) |
| `phy_init` | `0x015000` | 4 KB | RF calibration (**untouched**) |
| `factory` | `0x020000` | 4 MB | Stock WeatherXM factory firmware (**untouched**) |
| `ota_0` | `0x420000` | 4 MB | Stock WeatherXM OTA firmware (**untouched**) |
| `ota_1` | `0x820000` | 4 MB | **Muse Gadget firmware** |
| `nvs_key` | `0xC20000` | 4 KB | WeatherXM NVS encryption key (**untouched**) |
| `spiffs` | `0xC21000` | 3 MB | WeatherXM SPIFFS storage (**untouched**) |
| `muse_nvs` | `0xF21000` | 64 KB | **Muse isolated NVS** (Wi-Fi, pairing, SDK token, settings) |

Never run `idf.py flash` or `erase-flash` on a production WG1200: `idf.py flash`
targets `bootloader` (`0x0`) and `factory` (`0x20000`). Both
`tools/muse/board.sh flash wg1200` and `tools/wg1200_dualboot.sh install` write
only `0x00C000`, `0x013000` and `0x820000`, and [`ota.c`](../main/ota.c) refuses
OTA writes into `ota_0` or `factory`.

## Build and flash

### Browser install on retail units (`muse.weatherxm.com`)

Retail WG1200 units ship with RSA-3072 Secure Boot V2 enabled in eFuse. The
easiest way to try Muse on a retail WG1200 while keeping your official signed
WeatherXM firmware and station identity intact is
**[muse.weatherxm.com](https://muse.weatherxm.com/)** (source in
[`WeatherXM/muse-gadget-sdk`](https://github.com/WeatherXM/muse-gadget-sdk)):
connect over USB-C in Chrome or Edge to install a WeatherXM-signed Muse build
into `ota_1`, switch between Muse and WeatherXM in ~2 seconds, and pair over
Web Bluetooth with nothing installed locally.

### Building from source (ESP-IDF v6.0.3)

Activate ESP-IDF v6.0.3 and run commands from `esp32/`.

### 1. Back up critical partitions

Before the first install, save a backup of the low flash, certificates and NVS
outside version control:

```sh
tools/wg1200_dualboot.sh backup
```

### 2. Build

Set your SDK token in `build-muse-wxm-wg1200/sdkconfig` (see
[`../AGENTS.md`](../AGENTS.md)) or leave it unset in the build and send it over
USB serial after flashing (`>sdk_token=mgst_…`):

```sh
tools/muse/board.sh build wg1200
```

> **Secure Boot V2 note:** Unfused development boards boot the unsigned
> `build-muse-wxm-wg1200/muse-gadget.bin` directly. Retail WG1200 units ship
> with RSA-3072 Secure Boot V2 enabled in eFuse and only boot images signed
> with the board's RSA-3072 key (`WXM_SIGNING_KEY=/path/to/key.pem tools/wg1200_dualboot.sh install`,
> or WeatherXM's signed dual-boot releases).

### 3. Install into `ota_1` and set SDK token

```sh
tools/muse/board.sh flash wg1200
```

If you did not compile `CONFIG_GADGET_SDK_TOKEN` into the local build, send
your token over either USB serial port at 115200 baud:

```sh
printf '>sdk_token=mgst_YOUR_TOKEN\n' > PORT
```

The console replies `@sdk_token ok`, stores the token in `muse_nvs`, and
restarts ready for BLE pairing in the Muse app (Settings > Devices > Developer
mode).

### 4. Switching between Muse and WeatherXM

- **On screen:** open **Settings > WeatherXM** and tap `ota_0` or `factory`.
- **Top button:** hold the top button (`GPIO38`) for **10 seconds** to boot
  back into WeatherXM without erasing Muse's setup.
- **Over USB:**
  ```sh
  tools/wg1200_dualboot.sh boot-weatherxm
  tools/wg1200_dualboot.sh boot-muse
  ```

## Controls and commands

- **Top button (`GPIO38`):** short press sleeps or wakes the screen; double
  press toggles BLE setup; hold **5 s and release** to reset Muse pairing and
  Wi-Fi; hold **10 s** to reboot into stock WeatherXM firmware.
- **Touchscreen:** tap to confirm pairing or dismiss an image/alert card; swipe
  left from Muse's face to open **Settings** (`Weather`, `WeatherXM`, `Wi-Fi`,
  `Muse`, `Bluetooth`, `Sound`, `Sleep`); swipe down on Muse's face to request
  a `480×480` hourly forecast card scored against live station telemetry
  (customizable over USB/BLE via `card.prompt=…`, with fallback location
  `CONFIG_MUSE_WG1200_FORECAST_LOCATION` when no station GPS fix is cached).
- **Gadget commands (`link.register`):**
  - `sensors.read`: indoor `barometric_pressure` (`hPa`) and
    `gateway_temperature` (`C`, console internal temperature)
    from `BMP390`, plus outdoor `WS1001`/`WS1300` LoRa telemetry
    (`outdoor_temperature`, `outdoor_humidity`, `wind_speed`, `wind_gust`,
    `wind_direction`, `rain_rate`, `rain_daily`, `rain_accumulated`,
    `uv_index`, `illuminance`, `solar_irradiance`, `station_battery`,
    `lora_rssi`, `lora_snr`, each with `age_s`, and `lora_radio` metadata). Tap
    **LoRa Region** in **Settings > Weather** to cycle `EU868` / `US915` / `AS923`.
  - `display.draw_file` / `display.draw_url` / `display.show_animation`: draws
    `480×480` baseline JPEGs on the screen and wakes it if asleep. When
    combining weather data with LAN cameras over the Home Link tunnel
    (`CONFIG_HOMEHUB_TUNNEL=y`), crop/transcode on the VM to a `480×480`
    baseline JPEG first (ESP32 ROM `tjpgd` rejects progressive JPEGs).
  - `wg1200.notify`: shows a dismissible alert card overlay (`message`,
    optional `title`, `beep_ms` `0–1000`, `duration_s`) with an optional buzzer
    chime.
  - `device.boot_weatherxm`: switches `otadata` back to `ota_0` or `factory`
    and reboots into WeatherXM OS when explicitly requested by the user.

## Limits and hardware verification

- No microphone or speaker: replies are shown as captions on the `480×480`
  display, and audio alerts use the passive buzzer (`Settings > Sound` mutes
  it).
- Wall-powered over USB-C; there is no internal battery or fuel gauge, and the
  MicroSD slot (`PCA9535 IO1_3`/`IO1_4`) is unused.
- Checked on a retail WG1200 v1.1 unit paired with an outdoor `WS1001` LoRa
  sensor array:
  - Cold boot and reboot in `ota_1`, 8 MB octal PSRAM, and both USB ports
    (`303a:1001` USB Serial/JTAG and `1a86:7523` CH340) answering `>status`.
  - Non-destructive `ota_1` ↔ `ota_0` switching via touchscreen, 10 s button
    hold, `>boot.weatherxm` and `device.boot_weatherxm`, with stock WeatherXM
    station identity and NVS preserved.
  - BLE pairing, Wi-Fi, Home Link Noise control session, `device.guide`, and
    the `10.42.0.1` Home Link tunnel (`int=82K/62K dma=74K/62K psram=3491K`).
  - `BMP390` barometric pressure, `SX1262` `EU868` LoRa packet reception and
    GPS fix caching, passive buzzer chirps, `wg1200.notify` cards, and
    `480×480` baseline JPEG rendering via `display.draw_file` while awake and
    asleep.
