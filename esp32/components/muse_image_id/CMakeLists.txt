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

# The board an image is for, in ESP-IDF's custom app description section
# (README.md), and the check every updater runs on an image's first bytes.
idf_component_register(
    SRCS "muse_image_id.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_app_format bootloader_support
)
# ESP-IDF's App Image Format: "-u" keeps the description in an image that
# never references it (a build without any updater).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u muse_board_desc")
# The device model in the description (muse_image_id.h): SHA-256 of CONFIG_MUSE_DEVICE_MODEL_ID, its first 8 bytes.
if(CONFIG_MUSE_DEVICE_MODEL_ID)
    string(SHA256 model_sha "${CONFIG_MUSE_DEVICE_MODEL_ID}")
    set(model_bytes "")
    foreach(i RANGE 0 14 2)
        string(SUBSTRING "${model_sha}" ${i} 2 byte)
        list(APPEND model_bytes "0x${byte}")
    endforeach()
    list(JOIN model_bytes "," model_init)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "MUSE_DEVICE_MODEL_HASH=${model_init}")
endif()
