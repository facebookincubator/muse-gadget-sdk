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

# The firmware version: set in one place, distinct for
# every image. version.txt holds the release number MAJOR.MINOR.PATCH. A build
# of the commit tagged for that release (<CODE>-v<X.Y.Z>, CODE the board's
# catalog model code, CONFIG_HOMEHUB_BLE_PRODUCT_CODE; v<X.Y.Z> on a board
# without one) with no local changes is that version; anything else appends
# the commit: X.Y.Z+g<8 hex>, and .dirty with uncommitted changes to tracked
# files (X.Y.Z+nogit outside a git checkout). The string becomes PROJECT_VER,
# so esp_app_get_description()->version, DIS Firmware Revision, Link
# device_info and Settings all carry it. -DPROJECT_VER=... still wins.
#
# Tags are made at release, never by a build. tests/test_version.py runs this
# file in CMake script mode (muse_version_compute()).

# The board's product code, before Kconfig has run: the generated sdkconfig if
# the build directory has one with a code in it, else the last overlay in SDKCONFIG_DEFAULTS
# that sets it (overlays apply in order).
function(muse_version_product_code out sdkconfig defaults)
    set(code "")
    if(sdkconfig AND EXISTS "${sdkconfig}")
        file(STRINGS "${sdkconfig}" lines REGEX "^CONFIG_HOMEHUB_BLE_PRODUCT_CODE=")
        if(lines)
            list(GET lines -1 line)
            string(REGEX REPLACE "^CONFIG_HOMEHUB_BLE_PRODUCT_CODE=\"?([^\"]*)\"?$" "\\1" code "${line}")
            if(NOT code STREQUAL "")
                set(${out} "${code}" PARENT_SCOPE)
                return()
            endif()
            # Empty: a build directory from before the board had a code
            # (Kconfig fills it in from the overlay later): ask the overlays.
        endif()
    endif()
    foreach(f IN LISTS defaults)
        if(NOT IS_ABSOLUTE "${f}")
            set(f "${GADGET_SOURCE_DIR}/${f}")
        endif()
        if(EXISTS "${f}")
            file(STRINGS "${f}" lines REGEX "^CONFIG_HOMEHUB_BLE_PRODUCT_CODE=")
            if(lines)
                list(GET lines -1 line)
                string(REGEX REPLACE "^CONFIG_HOMEHUB_BLE_PRODUCT_CODE=\"?([^\"]*)\"?$" "\\1" code "${line}")
            endif()
        endif()
    endforeach()
    set(${out} "${code}" PARENT_SCOPE)
endfunction()

# out: the version string. src: the esp32/ directory (version.txt, the git
# checkout). code: the product code ("" for none).
function(muse_version_compute out src code)
    file(READ "${src}/version.txt" base)
    string(STRIP "${base}" base)
    if(NOT base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "version.txt must hold MAJOR.MINOR.PATCH, not '${base}'")
    endif()
    if(base STREQUAL "999.0.0")
        message(FATAL_ERROR "version.txt: 999.0.0 is the old placeholder; give a real release number")
    endif()
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        set(${out} "${base}+nogit" PARENT_SCOPE)
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" rev-parse --short=8 HEAD
                    OUTPUT_VARIABLE sha RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT rc EQUAL 0 OR sha STREQUAL "")
        set(${out} "${base}+nogit" PARENT_SCOPE)
        return()
    endif()
    # Tracked changes anywhere in the repository count (protocols/ and xplat/
    # are built in too); untracked files don't.
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${src}" tag --points-at HEAD
                    OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REPLACE "\n" ";" tags "${tags}")
    if(code STREQUAL "")
        set(release_tag "v${base}")
    else()
        set(release_tag "${code}-v${base}")
    endif()
    if(changes STREQUAL "" AND release_tag IN_LIST tags)
        set(${out} "${base}" PARENT_SCOPE)
    elseif(changes STREQUAL "")
        set(${out} "${base}+g${sha}" PARENT_SCOPE)
    else()
        set(${out} "${base}+g${sha}.dirty" PARENT_SCOPE)
    endif()
endfunction()

# In a project build: set PROJECT_VER (unless given) and reconfigure when the
# commit or the index changes, so a new commit gets a new string.
macro(muse_version_setup)
    if(NOT DEFINED PROJECT_VER)
        muse_version_product_code(_muse_code "${SDKCONFIG}" "${SDKCONFIG_DEFAULTS}")
        muse_version_compute(PROJECT_VER "${GADGET_SOURCE_DIR}" "${_muse_code}")
        message(STATUS "Firmware version ${PROJECT_VER} (product code '${_muse_code}')")
        find_package(Git QUIET)
        if(GIT_FOUND)
            foreach(_p HEAD index packed-refs)
                execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${GADGET_SOURCE_DIR}" rev-parse --path-format=absolute --git-path ${_p}
                                OUTPUT_VARIABLE _gp OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
                if(_gp AND EXISTS "${_gp}")
                    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gp}")
                endif()
            endforeach()
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GADGET_SOURCE_DIR}/version.txt")
    endif()
endmacro()
