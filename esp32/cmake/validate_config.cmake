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

# Shared build-profile checks. Include after project().
set(GADGET_CONFIG_REGEN_HINT
    "Generated sdkconfig values override newer defaults; regenerate the active isolated profile configuration.")

if(NOT CONFIG_CJSON_NESTING_LIMIT EQUAL 16)
    message(FATAL_ERROR
        "ESP32 Device SDK requires cJSON nesting limit 16. ${GADGET_CONFIG_REGEN_HINT}")
endif()

# Old generated profiles retain 64 KiB TCP buffers and can exhaust the DMA heap
# during tunnel bursts even when sdkconfig.defaults has been updated.
# TCP limits: the no-PSRAM Cardputer and AI Passport have no tunnel and use
# four-MSS windows.
# Windows up to 32 KiB are allowed where lwIP and Wi-Fi buffers prefer PSRAM
# (sdkconfig.defaults.esp32c5), so they cost no internal RAM; the data-path
# bench may try anything.
if((NOT CONFIG_LWIP_TCP_SND_BUF_DEFAULT EQUAL 16384 OR
    NOT CONFIG_LWIP_TCP_WND_DEFAULT EQUAL 16384) AND
   NOT CONFIG_HOMEHUB_PIPELINE_BENCH AND
   NOT (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP AND
        CONFIG_LWIP_TCP_SND_BUF_DEFAULT GREATER_EQUAL 16384 AND
        CONFIG_LWIP_TCP_SND_BUF_DEFAULT LESS_EQUAL 32768 AND
        CONFIG_LWIP_TCP_WND_DEFAULT GREATER_EQUAL 16384 AND
        CONFIG_LWIP_TCP_WND_DEFAULT LESS_EQUAL 32768) AND
   NOT ((CONFIG_MUSE_BOARD_M5STACK_CARDPUTER_ADV OR CONFIG_MUSE_BOARD_AI_PASSPORT) AND NOT CONFIG_SPIRAM AND
        NOT CONFIG_HOMEHUB_TUNNEL AND CONFIG_LWIP_TCP_SND_BUF_DEFAULT EQUAL 5760 AND
        CONFIG_LWIP_TCP_WND_DEFAULT EQUAL 5760))
    message(FATAL_ERROR
        "ESP32 Device SDK requires TCP send buffer and receive window 16384 (up to 32768 with CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP, 5760 on the no-tunnel Cardputer ADV and AI Passport) for DMA headroom. ${GADGET_CONFIG_REGEN_HINT}")
endif()

# A token from gadgets.muse.ai is mgst_ plus 43 canonical base64url characters.
# Manufacturer builds pair with fleet attestation instead, and internal builds
# without a token disable CONFIG_GADGET_SDK_TOKEN_WARN.
if("${CONFIG_GADGET_SDK_TOKEN}" STREQUAL "")
    if(CONFIG_GADGET_SDK_TOKEN_WARN AND NOT CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH)
        set(GADGET_SDK_TOKEN_WARNING
            "No SDK token: set CONFIG_GADGET_SDK_TOKEN (idf.py menuconfig > ESP32 Device SDK)"
            "to the token from gadgets.muse.ai. Gadgets without one will stop pairing.")
        list(GET GADGET_SDK_TOKEN_WARNING 0 GADGET_SDK_TOKEN_LINE1)
        list(GET GADGET_SDK_TOKEN_WARNING 1 GADGET_SDK_TOKEN_LINE2)
        string(JOIN " " GADGET_SDK_TOKEN_WARNING_TEXT ${GADGET_SDK_TOKEN_WARNING})
        message(WARNING "${GADGET_SDK_TOKEN_WARNING_TEXT}")
        # Configure-time warnings vanish on incremental builds, so repeat the
        # banner after the app links on every build.
        set(GADGET_SDK_TOKEN_BANNER
            "################################################################################")
        add_custom_target(gadget_sdk_token_warning ALL
            COMMAND ${CMAKE_COMMAND} -E echo ""
            COMMAND ${CMAKE_COMMAND} -E echo "${GADGET_SDK_TOKEN_BANNER}"
            COMMAND ${CMAKE_COMMAND} -E echo "##  WARNING: ${GADGET_SDK_TOKEN_LINE1}"
            COMMAND ${CMAKE_COMMAND} -E echo "##  ${GADGET_SDK_TOKEN_LINE2}"
            COMMAND ${CMAKE_COMMAND} -E echo "${GADGET_SDK_TOKEN_BANNER}"
            COMMAND ${CMAKE_COMMAND} -E echo ""
            COMMENT "Checking SDK token"
            VERBATIM)
        if(TARGET app)
            add_dependencies(gadget_sdk_token_warning app)
        endif()
    endif()
else()
    string(LENGTH "${CONFIG_GADGET_SDK_TOKEN}" GADGET_SDK_TOKEN_LENGTH)
    if(NOT GADGET_SDK_TOKEN_LENGTH EQUAL 48 OR
       NOT CONFIG_GADGET_SDK_TOKEN MATCHES "^mgst_[A-Za-z0-9_-]*[AEIMQUYcgkosw048]$")
        message(FATAL_ERROR
            "CONFIG_GADGET_SDK_TOKEN is not a valid SDK token. Copy it again from gadgets.muse.ai.")
    endif()
endif()

# Manufacturer attestation must remain inaccessible to unsigned firmware.
# Community pairing is independent of logging, OTA signing, and board selection.
if(CONFIG_HOMEHUB_PAIRING_EFUSE_AUTH)
    if(NOT CONFIG_SECURE_BOOT)
        message(FATAL_ERROR
            "The hardware eFuse pairing key requires Secure Boot. ${GADGET_CONFIG_REGEN_HINT}")
    endif()
    if(NOT CONFIG_HOMEHUB_PAIRING_AUTH_EPOCH EQUAL 1)
        message(FATAL_ERROR
            "Manufacturer pairing builds must use production epoch 1. ${GADGET_CONFIG_REGEN_HINT}")
    endif()
endif()
