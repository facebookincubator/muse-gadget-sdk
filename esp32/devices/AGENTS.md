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

# Adding a device

A recipe for porting the firmware to a new ESP32 board. Read `../AGENTS.md`
first for building, flashing and monitoring. Paths below are relative to
`esp32/`.

## Vendor sources

Before you add a feature to a board (its IMU, camera, RTC, SD card, power
chip and so on) or port a board from the same vendor, read the vendor's own
code for it. Clone the repo rather than reading it on the web, and search it.

| Board | Vendor source | Where to look |
|---|---|---|
| Waveshare ESP32-S3-Touch-AMOLED-1.75C | [waveshareteam/ESP32-S3-Touch-AMOLED-1.75C](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C) | `Schematic/`. `examples/esp-idf/` for the AXP2101 power chip (`01_AXP2101`) and the QMI8658 IMU (`04_Immersive_block`). `examples/arduino/examples/` for the ES7210 mics and the ES8311 codec. Muse drives the display, touch and codec through its BSP, `waveshare/esp32_s3_touch_amoled_1_75c`. |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | [waveshareteam/ESP32-S3-Touch-AMOLED-1.75](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75) | The 1.75C's chips on other pins (LCD and touch reset on GPIO 39 and 40, MCLK on 42), plus an SD slot on GPIO 1 to 3 and a TCA9554 expander. PWR reaches the ESP32 only through the AXP2101. Muse uses the 1.75C driver with its BSP, `waveshare/esp32_s3_touch_amoled_1_75`. |
| Waveshare ESP32-C6-Touch-AMOLED-2.06 | [waveshareteam/ESP32-C6-Touch-AMOLED-2.06](https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.06), [Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_c6_touch_amoled_2_06) | `Schematic/ESP32-C6-Touch-AMOLED-2.06-Schematic-V1.0.pdf` for BOOT GPIO9, the PWR circuit, and ES7210 MIC1/MIC2. Muse polls PWR edges through the AXP2101. Muse uses the published v2.0.0 BSP for display, touch, ES8311 speaker and ES7210 capture. Set `CONFIG_BSP_I2S_NUM=0` and `CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=8`: the BSP defaults to I2S1 and ignores runtime draw-buffer settings. |
| Waveshare ESP32-S3-Touch-AMOLED-2.16 | [waveshareteam/ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16) | `schematic/`. `examples/esp-idf/` for the AXP2101 power chip (`01_AXP2101`) and the QMI8658 IMU (`04_Immersive_block`). Muse drives the display, touch and codec through its BSP, `waveshare/esp32_s3_touch_amoled_2_16`. |
| Seeed SenseCAP Watcher | [Seeed-Studio/SenseCAP-Watcher-Firmware](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware) | `components/sensecap-watcher/` is Seeed's BSP. `include/sensecap-watcher.h` has the pins for the LCD, touch, knob, IO expander, audio, battery, SD card and the Himax camera chip (driven through `components/sscma_client/`). `examples/factory_firmware/` is the firmware it ships with. xiaozhi-esp32's [sensecap-watcher board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/sensecap-watcher) is a second reference. |
| Every M5Stack board (StickS3, StickC Plus2, Cardputer ADV, StopWatch, CoreS3, Core2) | [m5stack/M5Unified](https://github.com/m5stack/M5Unified), and [M5GFX](https://github.com/m5stack/M5GFX) for the panels | `src/M5Unified.inl` for pins, buttons and audio. `src/utility/` for power and the battery (`Power_Class.inl`), the IMU, RTC, mic, speaker and LEDs. `src/M5GFX.cpp` in M5GFX for the panel. Search both for the model's `board_M5...` name. For the StopWatch, M5's factory firmware [m5stack/M5StopWatch-UserDemo](https://github.com/m5stack/M5StopWatch-UserDemo): `main/hal/` for the power chip, IO expander, buttons and audio, and `main/apps/app_stopwatch/view/view.cpp` for where the buttons sit. For the Core2, Espressif's BSP [esp-bsp/bsp/m5stack_core_2](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_2): `m5stack_core_2.c` for the AXP rail setup, display, touch and speaker init. |
| Espressif ESP32-S3-BOX-3 | [espressif/esp-bsp](https://github.com/espressif/esp-bsp) | `bsp/esp-box-3/`: pins in `include/bsp/esp-box-3.h`, LCD/touch revision detection and codecs in `esp-box-3.c`, duplex I2S in `esp-box-3_idf5.c`. |
| M5Stack CoreS3 | [espressif/esp-bsp](https://github.com/espressif/esp-bsp) | `bsp/m5stack_core_s3/`: pins in `include/bsp/m5stack_core_s3.h`, the AXP2101 rails and AW9523 lines each part needs in `src/bsp_feature_en.c` and `src/bsp_io_expander.c`, codecs in `src/bsp_audio.c`. Muse drives it through that BSP, `espressif/m5stack_core_s3`. |
| FoloToy AI Passport | [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) | `components/bsp/include/bsp_pins.h` for every pin, the button ladder's windows and the I2C addresses. `components/bsp/src/` for the ST7789P3's init table (`bsp_display.c`), the ES8311 setup (`bsp_audio.c`, keep `no_dac_ref`) and the CW2017 gauge (`bsp_battery.c`). `docs/hardware-design/` has the hardware guide. |
| AIPI Lite | xiaozhi-esp32's [aipi-lite board](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/xorigin/aipi-lite) | `config.h` for pins, then `aipi-lite.cc` and `power_manager.h`. |
| Guition JC3248W535 | No vendor repo. [me-processware/JC3248W535-Driver](https://github.com/me-processware/JC3248W535-Driver), [sirisakG2/JC3248W535C](https://github.com/sirisakG2/JC3248W535C) | `src/JC3248W535_Display.h` and `src/JC3248W535_Touch.h` in the driver for the LCD and touch pins and the touch read command; the notes repo for the audio pins and USB. Arduino_GFX's `Arduino_AXS15231B.h` for the panel's init sequence. |
| VN ESP32-S3 1.83-inch NV3023 | [TienHuyIoT/xiaozhi-esp32_vietnam](https://github.com/TienHuyIoT/xiaozhi-esp32_vietnam) | `main/boards/xiaozhi-ai-iot-vietnam-1st/`: `config.h` for pins, the `.cc` for the NV3023 init table and the codecs (`BoxAudioCodec`), `power_manager.h` for the battery. `xingzhi-cube-1.83tft-wifi/` has the same panel. |
| Home Assistant Voice Preview Edition | [esphome/home-assistant-voice-pe](https://github.com/esphome/home-assistant-voice-pe) | `home-assistant-voice.yaml`, its ESPHome config. |
| Freenove FNK0104B | [Freenove/Freenove_ESP32_S3_Display](https://github.com/Freenove/Freenove_ESP32_S3_Display) | `Tutorial_With_Touch/Sketches/`: `Sketch_07.1_Music` (ES8311 + I2S pins), `Sketch_11.1_Touch` (FT6336U), `Sketch_05.1_Battery_Voltage` (battery divider), `Sketch_02.1_LedPixel` (WS2812 pin). `Libraries/FNK0104AB/` has the TFT_eSPI setup with the display pins. |
| Waveshare ESP32-S3-Touch-LCD-7 | [waveshareteam/ESP32-S3-Touch-LCD-7](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-7), [waveshareteam/waveshare_boards](https://github.com/waveshareteam/waveshare_boards) | `examples/ESP-IDF/09_lvgl_v9_demo/components/waveshare_rgb_lcd_port.[ch]` for RGB timing, GT911 reset and CH422G output; `boards/esp32_s3_touch_lcd_7/` for the pin map. |
| VIEWE SmartRing-Plus | [VIEWESMART/SmartRing-Plus](https://github.com/VIEWESMART/SmartRing-Plus). Buy: [VIEWE](https://viewedisplay.com/product/smart-ring-plus-esp32-ai-chatbot-1-8-inch-smart-touch-display/). BSP `viewesmart/smartring_plus` 1.1.2, vendored at `components/viewesmart__smartring_plus` | `schematic/` and `examples/esp-idf/` for the hardware. Muse uses the BSP for display, touch and battery. Audio is the board file's own 16 kHz I2S, not `bsp_audio_init()`. Power-off is the hardware switch only. See [`viewe-smartring-plus.md`](viewe-smartring-plus.md). |
| Seeed reSpeaker Lite | [respeaker/reSpeaker_Lite](https://github.com/respeaker/reSpeaker_Lite) | `doc/images/pinout.png`, the I2C examples, `xmos_firmwares/`. |

For the Cardputer ADV keyboard, also read [m5stack/M5Cardputer](https://github.com/m5stack/M5Cardputer), especially `src/utility/Keyboard/KeyboardReader/TCA8418.cpp` and `src/utility/Adafruit_TCA8418/`.

## 1. Gather the facts

Collect these before you write anything, and cite where each came from (the
vendor source, wiki, schematic, BSP or a reference firmware such as
xiaozhi-esp32) in a comment at the top of the overlay or board file. Don't
guess pins.

- Chip and module: target, flash size, PSRAM size and mode (none, quad or
  octal; an `R8` in an S3 part number means 8 MB octal).
- USB: native USB (`usbmodem`, `ttyACM`) or a bridge such as a CH340 or
  CH9102 (`usbserial`, `wchusbserial`, `ttyUSB`), with its USB vendor and
  product ID. The Muse tools find boards by these, not by port name.
- Buttons and their GPIOs. BOOT is GPIO0 on the ESP32 and S3, GPIO9 on the C3
  and C6, and GPIO28 on the C5.
- The status light, if any: type (addressable or PWM RGB) and pins.
- The display: controller, bus (SPI, QSPI, RGB, I80), resolution, pins and
  backlight. Touch controller and pins.
- Audio codec, mic and speaker. The PMU or battery sense.
- Whether the vendor publishes a BSP in the ESP Component Registry.

For an M5Stack board, or any board from a vendor under Vendor sources, start
from that vendor's code.

## 2. Pick the kind

| Kind | Status shown on | You add | Start from |
|---|---|---|---|
| Light | status LED, or nothing | an overlay | `sdkconfig.defaults` (the C5 DevKitC-1) |
| Status screen | edge bars and an animation | an overlay and a display backend in `main/` | `sdkconfig.ideaspark`, `sdkconfig.sensecap-indicator` |
| E-paper | a status screen with text | an overlay and its own `led_status.h` implementation | `sdkconfig.reterminal-e1001`, `sdkconfig.reterminal-e1002`, `main/epaper_status.c` |
| UI | LVGL avatar, voice and settings | an overlay on top of `sdkconfig.muse` and a `muse_board_t` | `sdkconfig.muse-*` |

A board with a screen, mic and speaker should run the full UI. The UI needs 8 MB
of flash or more: 16 MB boards use `partitions_muse.csv`, and 8 MB boards
`partitions_muse_8mb.csv`. Without PSRAM it loses the tunnel and images, and voice
notes go over Link's session with text-only replies. Link builds need 8 MB or
more, since `partitions.csv` ends past 4 MB and its offsets must not move.

## 3. Name it

Use the vendor and model, in lowercase with hyphens, and add the chip and
screen size when the vendor sells the model in several versions
(`waveshare-s3-175c`). Reuse that name everywhere:

| Where | Light / status screen | UI |
|---|---|---|
| Overlay | `devices/sdkconfig.<name>` | `devices/sdkconfig.muse-<name>` |
| Helper | `tools/board.sh <name>` | `tools/muse/board.sh build <alias>` |
| Kconfig | `HOMEHUB_LED_BACKEND_<VENDOR>_<CONTROLLER>` (new backends only) | `MUSE_BOARD_<NAME>`, `MUSE_BOARD_ID "<name_with_underscores>"` |
| Source | `main/led_status.c` block | `components/muse/boards/board_<name_with_underscores>.c` |

## 4. Write the overlay (every board)

Copy the closest overlay and keep its layout: a header comment naming the
board, its chip, flash, PSRAM and USB bridge, which file it loads after, then
settings grouped under short comments saying why. Set:

- `CONFIG_IDF_TARGET="esp32xx"`.
- `CONFIG_HOMEHUB_BUTTON_GPIO`. The button is active low with an internal
  pull-up. Boards with the full UI ignore it, because their talk button confirms pairing.
- The status backend, `CONFIG_HOMEHUB_LED_BACKEND_*=y` (`..._NONE` for no
  light). `sdkconfig.muse` sets `..._MUSE` for you.
- Flash: `CONFIG_ESPTOOLPY_FLASHSIZE_<N>MB=y` and
  `CONFIG_ESPTOOLPY_FLASHSIZE="<N>MB"`. Use `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`
  unless you know the part does QIO.
- PSRAM. `sdkconfig.defaults` assumes quad. Octal needs `CONFIG_SPIRAM_MODE_OCT=y`
  (see `sdkconfig.sensecap-indicator`). With no PSRAM, copy the whole block from
  `sdkconfig.ideaspark`: `CONFIG_SPIRAM=n`, mbedtls internal allocation,
  `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` and `CONFIG_HOMEHUB_TUNNEL=n`.
- `CONFIG_HOMEHUB_BLE_NAME_SUFFIX="-Disp"` on status-screen boards, which
  advertise as `MuseGadget-Disp-XXXXXX`. Light boards keep the defaults, and
  `sdkconfig.muse` sets the `MuseGadget` prefix explicitly.
- On a classic ESP32, `sdkconfig.ideaspark`'s chip block: `CONFIG_ESP32_REV_MIN_3=y`
  (signed apps need it), BLE-only BTDM, and the Wi-Fi and lwIP IRAM options off.

`sdkconfig.defaults` is tuned for the C5 (IRAM placement, Wi-Fi buffers). On
other chips, Kconfig ignores options that don't exist and drops options whose
dependencies aren't met, without failing the build. Step 8 checks for this.

## 5. Status-screen boards: add a display backend

Everything above the panel comes for free under `CONFIG_HOMEHUB_DISPLAY`: bars,
animation, agent name, connected dot, images and the `display.*` commands.
Your backend supplies the panel.

In `main/Kconfig.projbuild`:

- Add `config HOMEHUB_LED_BACKEND_<X>` to the `HOMEHUB_LED_BACKEND` choice,
  with `depends on` for the target (and `SPIRAM` if it needs a frame buffer in
  PSRAM), and mention the board in the choice's help.
- Add `default y if HOMEHUB_LED_BACKEND_<X>` to `HOMEHUB_DISPLAY`.

In `main/led_status.c`:

- Pins and geometry: an `#elif CONFIG_HOMEHUB_LED_BACKEND_<X>` block beside the
  ideaspark and SenseCAP Indicator ones. Define `LCD_NAME`, `LCD_H_RES`,
  `LCD_V_RES` (portrait), `LCD_PIN_BL`, `LCD_BAR_ROWS`, `LCD_ANIM_SCALE`,
  `LCD_DOT_MARGIN`, `LCD_BUF_CAPS` and your bus pins. The animation is 56×59
  cells, so keep `56 * LCD_ANIM_SCALE <= LCD_H_RES`.
- The panel functions: `lcd_draw()` (it returns once `buf` can be reused),
  `lcd_from_be()` (the panel's RGB565 byte order), `lcd_panel_init()` (it
  creates `s_panel`) and `lcd_panel_on()`.
- Four places split `#if ..._IDEASPARK_ST7789` / `#else`, with the SenseCAP
  Indicator as the `#else`: the includes, `lcd_draw()` and `lcd_from_be()`,
  `lcd_panel_init()` and `lcd_panel_on()`, and the pixel copy in
  `lcd_draw_image_rect()`. Turn each `#else` into
  `#elif CONFIG_HOMEHUB_LED_BACKEND_SENSECAP_ST7701` and add yours. The pixel
  copy depends only on byte order. SPI panels usually take big-endian like the
  ideaspark, and RGB frame buffers take native order like the SenseCAP
  Indicator.
- `led_hw_init()` turns the backlight on by driving `LCD_PIN_BL` high. A
  backlight that is active low, PWM or behind an expander needs a change there.

If the panel driver isn't part of `esp_lcd` in IDF v6.0.1 (check
`$IDF_PATH/components/esp_lcd`), add it to `main/idf_component.yml`, and to
`GADGET_REQUIRES` in `main/CMakeLists.txt` under an `if()` for your backend, as
`led_strip` is. Images from Muse are sized with
`tools/image_for_display.py --width W --height H`.

A single addressable LED on another pin needs only the overlay:
`CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27=y` and `CONFIG_HOMEHUB_LED_STRIP_GPIO`
(and `CONFIG_HOMEHUB_LED_RGB_ORDER=n` for the usual GRB order). Another LED type
follows the same two steps. Copy `DEVKIT_GPIO27` (addressable) or `PWM_RGB`.

## 6. Boards with the full UI: add a board

1. **Kconfig.** In `components/muse/Kconfig`, add `config MUSE_BOARD_<NAME>`
   to the `MUSE_BOARD` choice with `depends on IDF_TARGET_<CHIP>`. Then add
   `default "<id>" if MUSE_BOARD_<NAME>` to `MUSE_BOARD_ID`, above
   `default "none"`.
2. **Sources.** In `components/muse/CMakeLists.txt`, add
   `elseif(CONFIG_MUSE_BOARD_<NAME>)` and `list(APPEND srcs "boards/board_<id>.c")`.
   Any new IDF component goes in the unconditional `REQUIRES` or
   `PRIV_REQUIRES`, because requirements resolve before Kconfig.
3. **Managed components.** In `components/muse/idf_component.yml`, add the
   vendor BSP with `rules: - if: "$CONFIG{MUSE_BOARD_ID} == \"<id>\""`. Add
   `<id>` to the `esp_lvgl_adapter` rule if you use it. Always key rules on
   `MUSE_BOARD_ID`, never on `MUSE_BOARD_<NAME>`, which is undefined on other
   targets.
4. **Board file.** Write `components/muse/boards/board_<id>.c`. It defines a
   `static const muse_board_t s_board` (see `muse_board.h`) and
   `muse_board_get()`. Start from `board_waveshare_c6_18.c` or
   `board_waveshare_s3_175c.c` when a vendor BSP does the display, touch,
   codec and PMU. Start from `board_aipi.c` when you drive `esp_lcd`, the codec
   and GPIOs yourself.
5. **Overlay.** Write `devices/sdkconfig.muse-<name>`, loaded after
   `sdkconfig.muse`. It holds the target, `CONFIG_MUSE_BOARD_<NAME>=y`, PSRAM,
   the flash size if it isn't 16 MB (with 8 MB, also
   `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_muse_8mb.csv"`), and any
   LVGL fonts or options the board needs. Leave bench options, like
   screenshots, to `sdkconfig.muse-bench` (`MUSE_BENCH=1`).
6. **Helper.** Add a case to `tools/muse/board.sh` with a short alias, the
   profile (the overlay suffix), the target, and `baud` if its USB bridge
   can't take 460800. Add the alias to the usage comment and to `${2:?...}`.
   In `tools/muse/ports.py`, map the alias to its console's USB vendor and
   product ID in `USB` (a new bridge gets a constant and a `KINDS` name), and
   add it to `COMMANDS` if its console takes Muse's serial commands. Then map
   the board's `name` to the alias in `BOARDS` in `tools/muse/avatar.py`.

The fields of `muse_board_t`:

| Field | Contract |
|---|---|
| `name`, `width`, `height` | The panel as the UI draws it |
| `round`, `touch` | Round panel; `touch` only if `display_start` returns an input device |
| `keyboard` | Dedicated navigation keys: `poll_buttons` emits `MUSE_BTN_UP/DOWN/LEFT/RIGHT/ENTER/ESCAPE` presses. Enter selects and confirms pairing; Talk is not repurposed as Select while the menu is open. Defaults to false for two-button boards. |
| `talk_button`, `aux_button` | On-screen captions ("boot", "pwr"). `talk_hint` and `aux_hint` place them next to the physical button |
| `flat_menu_hints` | With `aux_hint` on `LV_ALIGN_RIGHT_MID` the menu turns that button's hint on end, drawn through a ~14 KB layer. Set it on a board that can't spare the RAM (the ESP32-C3): the hint lies flat on the menu's bar instead |
| `frame_ms` | Avatar frame period: 40 on the S3 boards, 50 on the C6 |
| `avatar_px` | Optional avatar canvas size in pixels; 0 uses the layout default |
| `init` | Runs first: power latches, I2C bus, PMU |
| `display_start` | Panel, LVGL and its task. Returns the display; leaves `*touch` NULL without touch |
| `display_lock`, `display_unlock` | LVGL's lock |
| `set_brightness` | 0 to 100 |
| `panel_sleep` | Puts the panel to sleep when the screen goes off. May be NULL: then only the backlight turns off |
| `display_pause` | Once the screen is dark on battery: stops LVGL (`esp_lv_adapter_pause`), sleeps the touch controller and stops anything else holding a PM lock, so the chip can light-sleep. Only buttons wake it. With a USB host attached the CPU stays at full speed anyway. May be NULL: LVGL keeps running and the CPU stays at full speed. Needs the PM block in the overlay (see `sdkconfig.muse-aipi`), and with PSRAM its static-buffers block too: light sleep's code takes internal RAM Link needs |
| `audio_init` | `esp_codec_dev` speaker and mic. `mic_slot` is 0 or 1, or -1 to mix. `set_mic_gain` may be NULL |
| `poll_buttons` | Called every 10 ms, or 50 ms while the display is paused and `wait_buttons` is NULL. Returns `MUSE_BTN_*` edges. Use `muse_gpio_button_*` for GPIO buttons, or `muse_pmu_poll_key()` for an AXP2101 PWR key |
| `touch_talk` | Optional. Non-NULL turns the mic icon into an on-screen talk button, and the pairing card into a tap target while Muse asks for the talk button. Called from LVGL's task with true on press and false on release; `poll_buttons` reports `MUSE_BTN_TALK_*`. NULL: the icon is only a hint |
| `wait_buttons` | While the display is paused: blocks until a button changes, so the chip light-sleeps instead of waking to poll. `muse_gpio_buttons_wait()` for GPIO buttons; a key only the PMU sees has to be polled. May be NULL: polled every 50 ms |
| `read_power` | May be NULL (no battery). `muse_pmu_read_power` on an AXP2101. Leave `battery_mv` 0 if the board can't measure the voltage in millivolts. The battery meter (`muse_battery.h`: Settings → Battery, `tools/muse/power.py`) reads it |
| `power_off` | Required. Returns only if it fails |

On a classic ESP32 (see `board_m5stack_stickc_plus2.c`):

- There's no USB Serial/JTAG, so the Muse tools use the console UART behind
  the USB bridge. `muse_console.h` covers both; don't call either driver
  directly.
- Set `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` (with
  `CONFIG_ESP32_REV_MIN_3=y`). Muse's voice task keeps its stack in PSRAM, and
  without it the board asserts at boot and restarts in a loop.
- GPIO34 to 39 are inputs only, with no pull-ups. Buttons on them need the
  board's own pull-ups.
- Only I2S0 does PDM. Its PDM-to-PCM filter leaves a large DC offset, so
  remove it before the mic gain or the samples clip. Each 16-bit stereo pair
  lands swapped in memory, so a mic on the right slot is `mic_slot` 0.

## 7. Wire up the tools and docs

- For light and status-screen boards, add a case to `tools/board.sh` with
  `TARGET`, the overlay appended to `DEFAULTS`, and the `PORTS` globs for its
  USB. Add a line to the usage comment, and widen the `sed -n '16,31p'` range
  in `usage()` by the lines you added.
- In `devices/README.md`, add a row to Supported devices, a column to
  Features, and a row to Build. Link only to reference and store pages you
  have opened, and use "—" when there is none.
- If the vendor publishes code for the board, add it to Vendor sources above.
- Add the board to the Supported boards table in `../AGENTS.md` and the Boards
  table in `../README.md`. If the board changes the BLE name or how status is
  shown, update "First boot and pairing" too.

## 8. Verify

1. Build the new board from scratch in its own directory (delete it first if
   it exists): `tools/board.sh <name> build` or
   `tools/muse/board.sh build <alias>`.
2. Check that every line of the overlay reached the generated config. Anything
   listed was misspelled, or had its dependencies unmet:

   ```sh
   o=devices/sdkconfig.<name>; s=build-<name>/sdkconfig
   grep '^CONFIG_' "$o" | while IFS= read -r l; do
     k=${l%%=*}
     case $l in *=n) ! grep -q "^$k=" "$s" ;; *) grep -qxF "$l" "$s" ;; esac ||
       echo "not applied: $l"
   done
   ```

3. Check the `check_sizes` line ("Smallest app partition is ..."). Flag
   anything under 10% free.
4. Rebuild what you touched: `idf.py build`, plus the ideaspark and SenseCAP
   Indicator if you changed `led_status.c`, or another board with the full UI if you changed
   `components/muse`. UI and other builds share `managed_components/`, so
   delete it and `dependencies.lock` between them.
5. Run the host tests: `python3 -m unittest discover -s tests -p 'test_*.py'`.
6. On hardware, flash and capture the boot with `tools/muse/monitor.py PORT 30`
   (for a board with the full UI, `$(tools/muse/ports.py <alias>)` gives the port).
   You should see `link.main: Muse Gadget starting` and the `link.led: LED
   status ready: ...` line for your backend, PSRAM found if the board has it,
   and no panic or reboot loop. The status should breathe orange, and a BLE
   scan should show `<prefix>-XXXXXX`.

If you don't have the board, say that the port was only built, not run.
