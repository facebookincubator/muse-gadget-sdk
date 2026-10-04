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

# The Muse gadget as an ESP-Mosaico application: the firmware in ../.. with the
# board's retained partition layout and ESP-Iris around it, so that Vibe Mode
# stays on the board and installs it. "ESP-Mosaico" in ../README.md has the
# commands.
cmake_minimum_required(VERSION 3.16)

# ESP-Iris, the Recovery adapter and the update bundle's tools come from an
# ESP-Mosaico workspace, a checkout of
# https://github.com/esp-mosaico/esp-mosaico-vibe with its esp-mosaico-utils
# submodule.
if(NOT MOSAICO_WORKSPACE AND DEFINED ENV{MOSAICO_WORKSPACE})
    set(MOSAICO_WORKSPACE "$ENV{MOSAICO_WORKSPACE}" CACHE PATH "ESP-Mosaico workspace (an esp-mosaico-vibe checkout)")
endif()
set(MOSAICO_UTILS_ROOT "${MOSAICO_WORKSPACE}/submodule/esp-mosaico-utils")
if(NOT EXISTS "${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/cmake/mosaico_idf_project.cmake")
    message(FATAL_ERROR
        "Set MOSAICO_WORKSPACE to an esp-mosaico-vibe checkout whose submodule/esp-mosaico-utils is initialised.")
endif()
get_filename_component(GADGET_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Without this ESP-IDF takes the target from the first defaults file, and the
# gadget's names its default board's chip.
set(IDF_TARGET esp32s31)
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "${GADGET_SOURCE_DIR}/sdkconfig.defaults"
        "${GADGET_SOURCE_DIR}/devices/sdkconfig.muse"
        "${GADGET_SOURCE_DIR}/devices/sdkconfig.muse-espressif-mosaico"
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
endif()
# The gadget reports this version to Muse, and the update bundle carries it as
# its release name. Presetting PROJECT_VER instead makes ESP-IDF skip the
# VERSION argument, which leaves the release name empty.
file(STRINGS "${GADGET_SOURCE_DIR}/version.txt" GADGET_VERSION LIMIT_COUNT 1)
# The gadget's main component is this project's main, so there is no main/ here.
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/ESP-Iris/components/esp_iris"
    "${GADGET_SOURCE_DIR}/main"
    "${GADGET_SOURCE_DIR}/components")
include("${MOSAICO_UTILS_ROOT}/esp-mosaico-recovery/cmake/mosaico_idf_project.cmake")
project(muse_gadget VERSION ${GADGET_VERSION})
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")
include("${GADGET_SOURCE_DIR}/cmake/validate_config.cmake")
