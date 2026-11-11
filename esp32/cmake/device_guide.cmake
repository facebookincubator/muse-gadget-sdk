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

# Embeds the Markdown device.guide returns: CONFIG_MUSE_DEVICE_GUIDE (default
# main/guide.md), looked up in the project directory, then in esp32/. Also runs
# as a script for the host tests:
#   cmake -DGUIDE_OUT=<file> [-DCONFIG_MUSE_DEVICE_GUIDE=<path>] -P cmake/device_guide.cmake
set(MUSE_DEVICE_GUIDE_MAX_BYTES 4096)
get_filename_component(MUSE_DEVICE_GUIDE_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

function(muse_device_guide_assemble out project_dir)
    set(path "${CONFIG_MUSE_DEVICE_GUIDE}")
    if(NOT path)
        set(path "main/guide.md")
    endif()
    get_filename_component(source "${path}" ABSOLUTE BASE_DIR "${project_dir}")
    if(NOT EXISTS "${source}")
        get_filename_component(source "${path}" ABSOLUTE BASE_DIR "${MUSE_DEVICE_GUIDE_SDK_DIR}")
    endif()
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "CONFIG_MUSE_DEVICE_GUIDE: ${path} not found")
    endif()

    # A leading HTML comment (the license header) stays out of the guide.
    file(READ "${source}" guide)
    string(FIND "${guide}" "<!--" open)
    if(open EQUAL 0)
        string(FIND "${guide}" "-->" close)
        if(close EQUAL -1)
            message(FATAL_ERROR "${source}: unterminated HTML comment")
        endif()
        math(EXPR close "${close} + 3")
        string(SUBSTRING "${guide}" ${close} -1 guide)
    endif()
    string(REGEX REPLACE "^[\r\n]+" "" guide "${guide}")

    string(LENGTH "${guide}" guide_len)
    if(guide_len EQUAL 0 OR guide_len GREATER MUSE_DEVICE_GUIDE_MAX_BYTES)
        message(FATAL_ERROR
            "The device guide is ${guide_len} bytes; it must be 1 to ${MUSE_DEVICE_GUIDE_MAX_BYTES} (${source}).")
    endif()
    file(WRITE "${out}.tmp" "${guide}")
    configure_file("${out}.tmp" "${out}" COPYONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    muse_device_guide_assemble("${GUIDE_OUT}" "${MUSE_DEVICE_GUIDE_SDK_DIR}")
endif()
