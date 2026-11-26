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

# esp_lvgl_adapter's pause handshake, fixed at build time (AGENTS.md,
# Configuration gotchas).
#
# espressif/esp_lvgl_adapter 0.6.4's esp_lv_adapter_pause() marks the adapter
# paused and only then clears any pending acknowledgement from the pause
# semaphore. The LVGL worker acknowledges as soon as it sees the mark, so an
# acknowledgement landing between the two is taken by the clear, and the
# pause then waits for one that never comes: forever with -1, or until its
# timeout, which un-marks the adapter but leaves the worker asleep in
# ulTaskNotifyTake(portMAX_DELAY) with nobody to wake it (LVGL frozen). On a
# gadget that hung whatever paused LVGL (the video show task, the screen-off
# path) until the task watchdog reset the chip.
#
# The fix, applied to a copy of the managed component's esp_lv_adapter.c in
# the build directory (the managed component stays as downloaded, so the
# component manager's hashes still match), which the adapter's library then
# compiles in place of the original:
#   1. the stale acknowledgement is cleared before the adapter is marked
#      paused, so the worker's acknowledgement can only come after it;
#   2. a pause that times out wakes the worker as it un-marks the adapter,
#      so LVGL runs on instead of sleeping for good.
# Our callers never wait unbounded on top of that: components/muse/
# muse_lvgl_hold.c pauses with a 500 ms timeout, retries once and then gives
# up the pause.
#
# Each edit must match the upstream text exactly once, or the build stops:
# on an adapter version bump, check whether upstream fixed the order (then
# drop this file) or re-derive the edits from the new esp_lv_adapter.c. The
# host test tests/test_lvgl_adapter_fix.py runs muse_lvgl_adapter_fix_text()
# on upstream's pause function.

set(MUSE_LVGL_ADAPTER_FIX_VERSION "0.6.4")

# Rewrites esp_lv_adapter.c's text (code) into out_var. Pure CMake, so the
# host test runs it with `cmake -P`.
function(muse_lvgl_adapter_fix_text code out_var)
    # 1. clear the stale acknowledgement first, then mark the adapter paused
    set(old1 [=[
    s_ctx.pause_ack = false;
    s_ctx.paused = true;
]=])
    set(new1 [=[
    s_ctx.pause_ack = false;
    /* muse fix: clear a stale acknowledgement BEFORE marking the adapter
     * paused. The worker acknowledges as soon as it sees the mark; clearing
     * after it could take that acknowledgement, and the pause then waited
     * for good (cmake/esp_lvgl_adapter_fix.cmake). */
    if (s_ctx.pause_done_sem) {
        xSemaphoreTake(s_ctx.pause_done_sem, 0);
    }
    s_ctx.paused = true;
]=])
    # ... and the late clear goes
    set(old2 [=[
    xSemaphoreTake(s_ctx.pause_done_sem, 0); /* clear pending */
]=])
    set(new2 [=[
    /* muse fix: the stale acknowledgement was cleared before the mark */
]=])
    # 2. a timed-out pause wakes the worker it un-marks
    set(old3 [=[
    if (xSemaphoreTake(s_ctx.pause_done_sem, ticks) != pdTRUE) {
        s_ctx.paused = false;
        return ESP_ERR_TIMEOUT;
    }
]=])
    set(new3 [=[
    if (xSemaphoreTake(s_ctx.pause_done_sem, ticks) != pdTRUE) {
        s_ctx.paused = false;
        /* muse fix: the worker may already sleep in its paused wait: wake
         * it, or LVGL stays stopped with the adapter marked running */
        xTaskNotifyGive(s_ctx.task);
        return ESP_ERR_TIMEOUT;
    }
]=])
    foreach(i 1 2 3)
        string(FIND "${code}" "${old${i}}" first)
        string(FIND "${code}" "${old${i}}" last REVERSE)
        if(first EQUAL -1 OR NOT first EQUAL last)
            message(FATAL_ERROR
                "esp_lvgl_adapter pause fix: edit ${i} doesn't match esp_lv_adapter.c exactly once. "
                "The adapter isn't ${MUSE_LVGL_ADAPTER_FIX_VERSION} any more: re-check its pause handshake "
                "and update or drop cmake/esp_lvgl_adapter_fix.cmake (esp32/AGENTS.md, Configuration gotchas).")
        endif()
        string(REPLACE "${old${i}}" "${new${i}}" code "${code}")
    endforeach()
    set(${out_var} "${code}" PARENT_SCOPE)
endfunction()

# In a project build (after project()): compile the fixed copy in the
# adapter's library, when the build has the adapter at all.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    idf_build_get_property(_muse_fix_components BUILD_COMPONENTS)
    set(_muse_fix_name "")
    foreach(_n espressif__esp_lvgl_adapter esp_lvgl_adapter)
        if(_n IN_LIST _muse_fix_components)
            set(_muse_fix_name ${_n})
            break()
        endif()
    endforeach()
    if(_muse_fix_name)
        idf_component_get_property(_muse_fix_lib ${_muse_fix_name} COMPONENT_LIB)
        idf_component_get_property(_muse_fix_dir ${_muse_fix_name} COMPONENT_DIR)
        set(_muse_fix_manifest "${_muse_fix_dir}/idf_component.yml")
        if(EXISTS "${_muse_fix_manifest}")
            file(STRINGS "${_muse_fix_manifest}" _muse_fix_ver REGEX "^version:")
            if(NOT _muse_fix_ver MATCHES "^version: *[\"']?${MUSE_LVGL_ADAPTER_FIX_VERSION}[\"']? *$")
                message(FATAL_ERROR
                    "esp_lvgl_adapter is '${_muse_fix_ver}', the pause fix was made for "
                    "${MUSE_LVGL_ADAPTER_FIX_VERSION}: re-check its pause handshake and update "
                    "cmake/esp_lvgl_adapter_fix.cmake (esp32/AGENTS.md, Configuration gotchas).")
            endif()
        endif()
        set(_muse_fix_src "${_muse_fix_dir}/src/adapter/esp_lv_adapter.c")
        file(READ "${_muse_fix_src}" _muse_fix_code)
        muse_lvgl_adapter_fix_text("${_muse_fix_code}" _muse_fix_code)
        set(_muse_fix_out "${CMAKE_BINARY_DIR}/esp_lvgl_adapter_fix/esp_lv_adapter.c")
        set(_muse_fix_old "")
        if(EXISTS "${_muse_fix_out}")
            file(READ "${_muse_fix_out}" _muse_fix_old)
        endif()
        if(NOT _muse_fix_old STREQUAL _muse_fix_code)
            file(WRITE "${_muse_fix_out}" "${_muse_fix_code}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_muse_fix_src}"
                     "${CMAKE_CURRENT_LIST_FILE}")
        get_target_property(_muse_fix_srcs ${_muse_fix_lib} SOURCES)
        set(_muse_fix_new_srcs "")
        set(_muse_fix_swapped 0)
        foreach(_s IN LISTS _muse_fix_srcs)
            if(_s MATCHES "(^|/)src/adapter/esp_lv_adapter\\.c$")
                list(APPEND _muse_fix_new_srcs "${_muse_fix_out}")
                math(EXPR _muse_fix_swapped "${_muse_fix_swapped} + 1")
            else()
                list(APPEND _muse_fix_new_srcs "${_s}")
            endif()
        endforeach()
        if(NOT _muse_fix_swapped EQUAL 1)
            message(FATAL_ERROR "esp_lvgl_adapter pause fix: esp_lv_adapter.c isn't in ${_muse_fix_lib}'s sources "
                                "once (${_muse_fix_swapped}): update cmake/esp_lvgl_adapter_fix.cmake.")
        endif()
        set_property(TARGET ${_muse_fix_lib} PROPERTY SOURCES ${_muse_fix_new_srcs})
        message(STATUS "esp_lvgl_adapter ${MUSE_LVGL_ADAPTER_FIX_VERSION}: pause handshake fixed (cmake/esp_lvgl_adapter_fix.cmake)")
    endif()
endif()
