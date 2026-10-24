# Waveshare ESP32-S3-Touch-LCD-1.54 (SKU 33868)

Port experimental de Muse Gadget SDK para ESP32-S3R8, LCD ST7789 240×240 y touch CST816S.

**Objetivo:** mantener originales Jollybot, expresiones, animaciones, LVGL, emparejamiento y OTA. Cambios aislados a los controladores, perfil, audio y eventual ElevenLabs TTS.

## Fuente de hardware

[Repositorio de Waveshare](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) (ejemplos ESP-IDF 5.5.1):
- `05_lvgl_example/main/main.c`: pantalla/touch.
- `01_factory/components/esp_bsp/bsp_codec.c`: ES8311, ES7210 e I²S.
- `01_factory/components/esp_bsp/bsp_display.h`: pines de LCD/backlight.
- `01_factory/components/esp_bsp/bsp_power_manager.c`: batería.
- `01_factory/main/main.c`: botones físicos.

| Componente | Pines confirmados en el ejemplo |
|---|---|
| ST7789 LCD SPI | SCLK 38, MOSI 39, CS 21, DC 45, RST 40, BL 46 |
| CST816S touch | SDA 42, SCL 41, INT 48, RST 47 |
| ES8311 y ES7210 | MCLK 8, BCLK 9, LRCK 10, DOUT 12, DIN 11 |
| Amplificador | PA enable 7 |
| Botones | BOOT 0, PLUS 4, PWR 5 |
| Batería | ADC GPIO1, habilitación GPIO2, indicador carga GPIO3 |

- Flash: 16 MB. PSRAM: 8 MB octal.
- El ejemplo de Waveshare usa audio **I²S estándar TX + TDM de cuatro ranuras RX**. El núcleo Muse actual consume audio de dos canales. **Debe resolverse el mapeo sin modificar Jollybot**.
- El controlador de LCD debe utilizar `esp_lcd_new_panel_st7789`, no el GC9A01 de la placa 1.28.
- No copiar el binario de otra Waveshare: los GPIO son distintos.

## Plan en este branch

1. Registrar el perfil `sdkconfig.muse-waveshare-s3-lcd154` con ESP-IDF 6.0.1 y dependencias.
2. Implementar `board_waveshare_s3_lcd154.c` (pantalla, touch, audio, botones y batería).
3. Añadir `lcd154` al script de compilación y trabajo CI.
4. Validar compilación y controladores en placa. Solo después incorporar ElevenLabs TTS vía un proxy privado.

## Protección de credenciales y firmware

El SDK token de Muse debe configurarse en `build-*/sdkconfig` ignorado por Git;
la ElevenLabs API key permanece únicamente en el proxy del homelab.
Antes de flashear, respaldar los **16 MB** de Flash. Este PR **no flashea**
ningún dispositivo ni declara probado físicamente el hardware.
