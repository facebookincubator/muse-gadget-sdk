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

# Muse wake word: the SDK-owned session core, audio rings, LFBE frontend,
# detector, and ExecuTorch runner. models/hey_muse.pte is embedded here;
# its export lives in tools/export_hey_muse.py. With
# MUSE_WAKEWORD_ENABLE=n this registers empty, like the muse component in
# Link-only builds, so default firmware is unchanged.
set(srcs)
# Requirements are collected by ESP-IDF in an early-expansion pass before
# Kconfig values exist, so executorch must be declared unconditionally.
# This does not compile the runtime in by itself: the executorch
# component still registers empty unless ET_ENABLE is selected.
set(priv_requires executorch esp_timer)
if(CONFIG_MUSE_WAKEWORD_ENABLE)
    list(APPEND srcs "muse_wakeword_core.c")
    # Exactly one runner implementation: each defines the ww_runner_* API.
    if(CONFIG_MUSE_WAKEWORD_RUNNER_BENCH)
        list(APPEND srcs "ww_runner_bench.c")
    else()
        list(APPEND srcs "ww_detector.c" "ww_frontend.c" "ww_runner_et.cpp")
        # Not EMBED_FILES: its generated asm has no alignment directive,
        # and ExecuTorch Program::load requires 8-aligned program data.
        # hey_muse_pte.S.in embeds the model 16-aligned in flash instead.
        set(HEY_MUSE_PTE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/models/hey_muse.pte")
        file(SIZE "${HEY_MUSE_PTE_PATH}" HEY_MUSE_PTE_LEN)
        configure_file(hey_muse_pte.S.in "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S" @ONLY)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S"
                                    PROPERTIES OBJECT_DEPENDS "${HEY_MUSE_PTE_PATH}")
        list(APPEND srcs "${CMAKE_CURRENT_BINARY_DIR}/hey_muse_pte.S")
    endif()
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES ${priv_requires}
)
