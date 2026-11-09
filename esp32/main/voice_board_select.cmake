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

set(HOMEHUB_VOICE_BOARD_PRODUCTS
    VOICE_PE
    RESPEAKER_LITE
    RESPEAKER_XVF3800
    WAVESHARE_154G
)

set(HOMEHUB_VOICE_BOARD_SELECTED "")
foreach(product IN LISTS HOMEHUB_VOICE_BOARD_PRODUCTS)
    if(CONFIG_HOMEHUB_VOICE_BOARD_${product})
        if(HOMEHUB_VOICE_BOARD_SELECTED)
            message(FATAL_ERROR
                "HOMEHUB_VOICE_BOARD selects multiple drivers: "
                "${HOMEHUB_VOICE_BOARD_SELECTED} and ${product}")
        endif()
        set(HOMEHUB_VOICE_BOARD_SELECTED "${product}")
    endif()
endforeach()

if(NOT HOMEHUB_VOICE_BOARD_SELECTED)
    message(FATAL_ERROR
        "HOMEHUB_VOICE requires exactly one HOMEHUB_VOICE_BOARD driver; "
        "UNSPECIFIED has no fallback")
endif()
if(CONFIG_HOMEHUB_VOICE_BOARD_UNSPECIFIED)
    message(FATAL_ERROR
        "HOMEHUB_VOICE_BOARD_UNSPECIFIED conflicts with ${HOMEHUB_VOICE_BOARD_SELECTED}")
endif()

if(HOMEHUB_VOICE_BOARD_SELECTED STREQUAL "VOICE_PE")
    if(NOT CONFIG_HOMEHUB_LED_BACKEND_VOICE_RING)
        message(FATAL_ERROR "HOMEHUB_VOICE_BOARD_VOICE_PE requires HOMEHUB_LED_BACKEND_VOICE_RING")
    endif()
    set(HOMEHUB_VOICE_BOARD_SOURCE "voice_board.c")
elseif(HOMEHUB_VOICE_BOARD_SELECTED STREQUAL "RESPEAKER_LITE")
    if(NOT CONFIG_HOMEHUB_LED_BACKEND_RESPEAKER_LITE)
        message(FATAL_ERROR
            "HOMEHUB_VOICE_BOARD_RESPEAKER_LITE requires HOMEHUB_LED_BACKEND_RESPEAKER_LITE")
    endif()
    set(HOMEHUB_VOICE_BOARD_SOURCE "voice_board_respeaker_lite.c")
elseif(HOMEHUB_VOICE_BOARD_SELECTED STREQUAL "RESPEAKER_XVF3800")
    if(NOT CONFIG_HOMEHUB_LED_BACKEND_NONE)
        message(FATAL_ERROR
            "HOMEHUB_VOICE_BOARD_RESPEAKER_XVF3800 requires HOMEHUB_LED_BACKEND_NONE")
    endif()
    set(HOMEHUB_VOICE_BOARD_SOURCE "voice_board_xvf3800.c")
elseif(HOMEHUB_VOICE_BOARD_SELECTED STREQUAL "WAVESHARE_154G")
    if(NOT CONFIG_HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G)
        message(FATAL_ERROR
            "HOMEHUB_VOICE_BOARD_WAVESHARE_154G requires HOMEHUB_LED_BACKEND_WAVESHARE_EPD154G")
    endif()
    set(HOMEHUB_VOICE_BOARD_SOURCE "voice_board_waveshare_154g.c")
    list(APPEND GADGET_REQUIRES esp_codec_dev)
else()
    message(FATAL_ERROR "HOMEHUB_VOICE_BOARD_${HOMEHUB_VOICE_BOARD_SELECTED} has no driver source")
endif()

list(APPEND GADGET_SRCS "${HOMEHUB_VOICE_BOARD_SOURCE}")
