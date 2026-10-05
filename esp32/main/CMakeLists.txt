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
    minimp3     # audio.play_url (muse_hw_commands.c)
    lua               # scripts (muse_script_sandbox.c)
    spi_flash         # ... and their storage (muse_script.c)
    esp_partition
    fatfs             # storage.* on the SD card (muse_hw_storage.c)
    sdmmc
    esp_driver_sdspi
    esp_driver_tsens  # the chip's temperature (muse_hw_io.c)
)

# E-paper has its own status screen in place of led_status.c.
if(CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_UC8179
   OR CONFIG_HOMEHUB_LED_BACKEND_RETERMINAL_SPECTRA6)
    list(APPEND GADGET_SRCS "epaper_status.c")
else()
    list(APPEND GADGET_SRCS "led_status.c")
endif()

if(CONFIG_MUSE_ENABLED)
    list(APPEND GADGET_SRCS "muse_glue.c")
endif()

if(CONFIG_MUSE_HW_COMMANDS)
    list(APPEND GADGET_SRCS "muse_hw_commands.c" "muse_hw_camera.c" "muse_hw_io.c" "muse_hw_storage.c"
        "muse_hw_pet.c" "muse_crash.c")
endif()

if(CONFIG_MUSE_SCRIPTS)
    list(APPEND GADGET_SRCS "muse_script.c" "muse_script_sandbox.c" "muse_hw_apps.c")
    # The app library (muse_hw_apps.c's LIBRARY): each example's page and script.
    file(GLOB GADGET_APP_LIBRARY CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/apps/*.json"
        "${CMAKE_CURRENT_LIST_DIR}/apps/*.lua")
endif()

if(CONFIG_HOMEHUB_LED_BACKEND_DEVKIT_GPIO27 OR CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING OR CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)
    list(APPEND GADGET_REQUIRES led_strip)
endif()

if(CONFIG_HOMEHUB_SENSECAP_SENSORS)
    list(APPEND GADGET_SRCS "sensecap_sensors.c")
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
    EMBED_TXTFILES ${GADGET_APP_LIBRARY}
)

if(CONFIG_MUSE_HW_COMMANDS)
    # muse_crash.c notes a crash for the next boot, then ESP-IDF's handler runs.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()
