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

# Restarts the firmware means to make, told apart from the ones it doesn't
# (README.md): every board, Link-only builds too.
set(srcs "muse_restart.c" "muse_restart_core.c" "muse_bootguard.c" "muse_bootguard_core.c" "muse_heapwatch.c"
    "muse_local_press.c" "muse_local_press_core.c"   # the update's confirm press
    "muse_stack.c"    # CONFIG_MUSE_STACKS_IN_PSRAM: the slots' parked workers
    "muse_crashrec.c" "muse_crashrec_core.c")   # a crash's record, kept across the restart
if(IDF_TARGET STREQUAL "esp32s31")
    list(APPEND srcs "muse_faultlog.c")   # the PSRAM access faults' log (README.md)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system esp_rom esp_timer freertos log nvs_flash esp_app_format hal soc spi_flash
)

# Every esp_reset_reason() caller sees the reason with the marker counted.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_reset_reason")
# The crash record (muse_crashrec.c): the panic handler notes what
# happened before it restarts, on every target.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# On the S31 a restart out of a failure (a panic, esp_restart()) gets the same
# whole-system reset as muse_restart(), and a marker naming the failure.
if(IDF_TARGET STREQUAL "esp32s31")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart_noos")
    # The fault log (muse_faultlog.c): each flash operation's window and the
    # panic handler, for the S31's PSRAM access faults.
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=spi_flash_disable_interrupts_caches_and_other_cpu"
        "-Wl,--wrap=spi_flash_enable_interrupts_caches_and_other_cpu")
endif()
