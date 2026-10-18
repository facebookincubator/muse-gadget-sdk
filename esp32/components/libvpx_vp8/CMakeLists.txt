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

# libvpx's VP8 decoder for video streaming (CONFIG_MUSE_VIDEO_VP8).
# See README.md. The libvpx sources keep their BSD license (LICENSE, PATENTS).
if(CONFIG_MUSE_VIDEO_VP8)
    file(GLOB_RECURSE srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
    if(CONFIG_IDF_TARGET_ESP32S3)
        # Bilinear prediction on the S3's SIMD (bit-exact; tests/test_libvpx_vp8.py checks the C).
        list(APPEND srcs "${CMAKE_CURRENT_LIST_DIR}/src/vp8/common/xtensa/bilinear_pie.S")
    endif()
    idf_component_register(
        SRCS ${srcs}
        INCLUDE_DIRS "src"
        LDFRAGMENTS "linker.lf")
    # Upstream code, warnings and all; -O2 for the same IRAM reason as tinyh264.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
    if(CONFIG_IDF_TARGET_ESP32S3)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGX_PIE_S3=1)
    endif()
    if(CONFIG_MUSE_VIDEO_VP8_PROF)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE MGX_PROF=1)
    endif()
else()
    idf_component_register()
endif()
