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

cmake_minimum_required(VERSION 3.16)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/project.cmake")
project(muse-gadget)
# Waveshare's CH32V003 component 1.0.2 uses the new I2C API but declares
# only the legacy driver dependency. IDF 6 no longer exports it transitively.
if(CONFIG_MUSE_BOARD_WAVESHARE_C5_28)
    idf_component_get_property(ch32_lib waveshare__custom_io_expander_ch32v003 COMPONENT_LIB)
    idf_component_get_property(i2c_lib esp_driver_i2c COMPONENT_LIB)
    target_link_libraries(${ch32_lib} PUBLIC ${i2c_lib})
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/validate_config.cmake")
