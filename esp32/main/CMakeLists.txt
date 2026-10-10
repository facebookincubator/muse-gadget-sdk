# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(GADGET_SRCS
    "main.c"
    "app.c"
    "ble_server.c"
    "wifi_mgr.c"
    "wifi_known.c"
    "identity.c"
    "config_store.c"
    "vm_api.c"
    "diagnostic_log.c"
    "stack_monitor.c"
    "bug_report.c"
    "ota.c"
    "image_fetch.c"
    "../avatar/happy_anim.c"
    "pixel_font.c"
    "button.c"
    "tunnel_netif.c"
    "net_discovery.c"
    "noise_control.cpp"
    "noise_tunnel.cpp"
    "link_pairing.c"
    "pairing_signer_policy.c"
    "pairing_transcript.c"
    "factory_test.c"
)

set(GADGET_REQUIRES
    nvs_flash
    esp_wifi
    esp_event
    esp_netif
    esp_http_client
    esp_https_ota
    app_update
    esp_app_format
    esp-tls
    noise_core
    product_info
    bt
    driver
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_ledc
    esp_driver_spi
    esp_driver_uart
    esp_lcd
    lwip
    mbedtls
    efuse
    muse
    # The voice board's. Requirements are resolved before the configuration
    # is loaded, so this is unconditional; unused code is not linked.
    esp_driver_i2s
    esp_driver_pcnt
)

# E-paper has its own status screen in place of led_status.c, with
# a per-panel driver behind epd_panel.h.
if(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179)
    list(APPEND GADGET_SRCS "epaper_status.c" "epd_uc8179.c")
elseif(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6)
    list(APPEND GADGET_SRCS "epaper_status.c" "epd_spectra6.c")
elseif(CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_SSD1681)
    list(APPEND GADGET_SRCS "epaper_status.c" "epd_ssd1681.c")
else()
    list(APPEND GADGET_SRCS "led_status.c")
endif()

if(CONFIG_MUSE_ENABLED)
    list(APPEND GADGET_SRCS "muse_glue.c")
endif()

if(CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27 OR CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING OR CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)
    list(APPEND GADGET_REQUIRES led_strip)
endif()

if(CONFIG_HOMEHUB_LED_BACKEND_CYD_ILI9341)
    list(APPEND GADGET_REQUIRES esp_lcd_ili9341)
endif()

if(CONFIG_HOMEHUB_SENSECAP_SENSORS)
    list(APPEND GADGET_SRCS "sensecap_sensors.c")
endif()
if(CONFIG_HOMEHUB_RETERMINAL_SHT4X)
    list(APPEND GADGET_SRCS "reterminal_sht4x.c")
endif()

if(CONFIG_HOMEHUB_VOICE)
    list(APPEND GADGET_SRCS
        "voice.c"
        "voice_muse_chat.c"
        "voice_player.c"
    )
    if(CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)
        list(APPEND GADGET_SRCS "voice_board_respeaker_lite.c")
    else()
        list(APPEND GADGET_SRCS "voice_board.c")
    endif()
endif()

idf_component_register(
    SRCS ${GADGET_SRCS}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../avatar"
    REQUIRES ${GADGET_REQUIRES}
)
